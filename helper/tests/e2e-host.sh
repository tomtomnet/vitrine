#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# vitrine-helper end to end on this host, as root through sudo (pkexec would
# need the polkit files installed): a test VM boots, the helper watches its
# QEMU and applies the fair server, the GPU clock floor (an AMD APU), the
# udmabuf limits and real-time threads; the VM powers off and everything
# must be back as it was.  Then: a helper killed while holding the settings
# (the next one restores them), vitrine gone while the VM runs (stdin
# closed), two helpers, a udmabuf limit changed by hand meanwhile (left as
# it is), focus priority on a real QEMU's threads (rt for the VM in front,
# behind for the others: vCPUs at nice -5), the capability an older vitrine
# gave its QEMU taken back by the app, and the app's HostSettings driving
# this helper.
#
#   helper/tests/e2e-host.sh [BUILD_DIR]
#
# E2E_NAME, E2E_PORT and E2E_MEM name the test VM, its ssh port and its
# memory (vitrine-helper-e2e, 2234, 2G): each work stream has its own.
#
# The udmabuf limits: where they are at host tuning's values already (a
# host booted with them), the test lowers them to 32768 entries / 1024 MB
# first, so that the helper has something to raise - values that still
# take any guest window, for VMs running meanwhile - and the exit trap puts
# back the values found.  They are kept in .dev/helper-e2e/udmabuf-before
# until then: a run killed before its trap (SIGKILL, an OOM in its scope)
# leaves the file, and the next run puts those values back first.
#
# Needs password-less sudo, the research export's QEMU and the KDE base
# image (an overlay is made in .dev; the base is never written).  Not part
# of ctest: it changes host-wide settings for a minute and puts them back,
# which only a person who agreed to it may run.  Whatever happens, the exit
# trap leaves the host as it found it.
set -u
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
build=${1:-$repo/build}
helper=$build/helper/vitrine-helper
export_dir=/home/user/Documents/qemu-gui-experimental
qemu=$export_dir/build/qemu/qemu-system-x86_64
pcbios=$export_dir/build/src/qemu/pc-bios
base=/home/user/vms/f44-kde-base.qcow2
key=/home/user/.ssh/vmtest_ed25519
port=${E2E_PORT:-2234}
name=${E2E_NAME:-vitrine-helper-e2e}
mem=${E2E_MEM:-2G}
dev=$repo/.dev/helper-e2e
fs=/sys/kernel/debug/sched/fair_server
udma=/sys/module/udmabuf/parameters

fails=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

[ -x "$helper" ] || { echo "no $helper: build first" >&2; exit 2; }
sudo -n true 2> /dev/null || { echo "needs password-less sudo" >&2; exit 2; }
# a VM of 2 GiB (E2E_MEM): the host keeps 10 GiB free first (notes/agents/COMMON.md);
# run the script in a capped scope, e.g. systemd-run --user --scope -p MemoryMax=6G
avail=$(awk '/^MemAvailable:/ {print int($2 / 1048576)}' /proc/meminfo)
[ "$avail" -ge 10 ] || { echo "only $avail GiB available: wait and retry" >&2; exit 3; }
mkdir -p "$dev"

# --- the host as found: what must be back at the end ---
card=
for c in /sys/class/drm/card[0-9]*; do
	[ "$(cat "$c/device/vendor" 2> /dev/null)" = 0x1002 ] && [ -e "$c/device/pp_od_clk_voltage" ] &&
		{ card=$(basename "$c"); break; }
done
fair_now() { for c in $(sudo ls $fs | sort -V); do echo "$c $(sudo cat $fs/$c/period) $(sudo cat $fs/$c/runtime)"; done; }
gpu_now() { [ -n "$card" ] && { cat "/sys/class/drm/$card/device/power_dpm_force_performance_level"; cat "/sys/class/drm/$card/device/pp_od_clk_voltage"; }; }
udma_now() { [ -d $udma ] && echo "$(cat $udma/list_limit) $(cat $udma/size_limit_mb)"; }
set_udma() {  # LIST SIZE_MB
	echo "$1" | sudo tee $udma/list_limit > /dev/null
	echo "$2" | sudo tee $udma/size_limit_mb > /dev/null
}

# --- what a run killed before its exit trap left ---
# its helpers' states first: a new helper puts them back at its start
out=$(sudo -n "$helper" session < /dev/null 2>&1 | grep -v '^ready \|^bye$')
[ -z "$out" ] || { echo "left by an earlier run, put back:"; echo "$out" | sed 's/^/       /'; }
# then the udmabuf limits it lowered (that helper's state put back the lowered ones)
udma_saved=$dev/udmabuf-before
if [ -e "$udma_saved" ]; then
	read -r l s < "$udma_saved"
	if [[ "$l $s" =~ ^[0-9]+\ [0-9]+$ ]] && [ -d $udma ]; then
		echo "an earlier run did not end: the udmabuf limits back to the $l $s it found (they were $(udma_now))"
		set_udma "$l" "$s"
		rm -f "$udma_saved"
	else
		echo "$udma_saved: '$l $s' is not two numbers; put the limits back by hand:" >&2
		echo "  echo VALUE | sudo tee $udma/list_limit; echo VALUE | sudo tee $udma/size_limit_mb" >&2
		exit 2
	fi
fi

fair_before=$(fair_now)
gpu_before=$(gpu_now)
udma_before=$(udma_now)
ncpu=$(echo "$fair_before" | wc -l)
# the cpus the helper changes, and puts back: those not at 10 ms / 1 ms (a
# tool may have left some there)
fair_todo=$(echo "$fair_before" | awk '$2 != 10000000 || $3 != 1000000' | wc -l)
# what the helper leaves after its runs: cpus at 10 ms / 1 ms beside others
# that are not are an earlier run's leftovers, put back with them (to their
# value, or the kernel's 1 s / 50 ms if they differ); all of them there stay
fair_base=$(echo "$fair_before" | awk '$2 != 10000000 || $3 != 1000000 {print $2 " " $3}' | sort -u)
[ "$(echo "$fair_base" | wc -l)" = 1 ] || fair_base="1000000000 50000000"
if [ "$fair_todo" -gt 0 ] && [ "$fair_todo" -lt "$ncpu" ]; then
	fair_after=$(echo "$fair_before" | awk -v b="$fair_base" '{if ($2 == 10000000 && $3 == 1000000) print $1 " " b; else print}')
else
	fair_after=$fair_before
fi
echo "host: $ncpu cpus, fair server $(echo "$fair_before" | head -1 | cut -d' ' -f2-), gpu ${card:-none} level $(echo "$gpu_before" | head -1)"
echo "udmabuf before: ${udma_before:-no parameters} (list_limit size_limit_mb)"
# what the helper raises from in this test, both below host tuning's values
udma_low=
if [ -n "$udma_before" ]; then
	read -r l s <<< "$udma_before"
	if [ "$l" -lt 65536 ] && [ "$s" -lt 2048 ]; then udma_low=$udma_before; else udma_low="32768 1024"; fi
fi
udma_raised() { local l s; read -r l s <<< "$(udma_now)"; [ "$l" -ge 65536 ] && [ "$s" -ge 2048 ]; }
[ "$fair_todo" = "$ncpu" ] || [ "$fair_todo" = 0 ] ||
	echo "  note: $((ncpu - fair_todo)) cpus have the fair server at 10 ms / 1 ms already, others not: leftovers, the helper puts them back to $fair_base"
[ "$fair_todo" = 0 ] &&
	echo "  note: every cpu has the fair server at 10 ms / 1 ms already (another tool?): left as it is"

pids=()          # processes started here, stopped by the trap
helpers=()       # sudo pids of helpers
restore_by_hand() {
	# last resort: the values found at the start, written back in an order the kernel takes
	local c p r cp
	while read -r c p r; do
		cp=$(sudo cat $fs/$c/period)
		if [ "$p" -lt "$cp" ]; then
			echo "$r" | sudo tee $fs/$c/runtime > /dev/null; echo "$p" | sudo tee $fs/$c/period > /dev/null
		else
			echo "$p" | sudo tee $fs/$c/period > /dev/null; echo "$r" | sudo tee $fs/$c/runtime > /dev/null
		fi
	done <<< "$fair_after"
	if [ -n "$card" ] && [ "$(cat /sys/class/drm/$card/device/power_dpm_force_performance_level)" = manual ] &&
		[ "$(echo "$gpu_before" | head -1)" != manual ]; then
		echo r | sudo tee /sys/class/drm/$card/device/pp_od_clk_voltage > /dev/null
		echo c | sudo tee /sys/class/drm/$card/device/pp_od_clk_voltage > /dev/null
		echo "$gpu_before" | head -1 | sudo tee /sys/class/drm/$card/device/power_dpm_force_performance_level > /dev/null
	fi
	# shellcheck disable=SC2086
	[ "$(udma_now)" = "$udma_before" ] || set_udma $udma_before
}
host_now() { fair_now; gpu_now; echo "udmabuf $(udma_now)"; }
host_before() { echo "$fair_after"; echo "$gpu_before"; echo "udmabuf $udma_before"; }
cleanup() {
	local p c
	for p in "${helpers[@]}"; do
		for c in $(pgrep -P "$p") $p; do sudo kill -TERM "$c" 2> /dev/null; done
	done
	for p in "${pids[@]}"; do kill -TERM "$p" 2> /dev/null; done
	sleep 1
	for p in "${pids[@]}"; do kill -KILL "$p" 2> /dev/null; done
	# whatever a helper left: a new one restores it at its start
	sudo -n "$helper" session < /dev/null > /dev/null 2>&1
	# the udmabuf limits the test lowered: the values found
	if [ -n "$udma_before" ] && [ "$(udma_now)" = "$udma_low" ] && [ "$udma_low" != "$udma_before" ]; then
		# shellcheck disable=SC2086
		set_udma $udma_before
	fi
	if [ "$(host_now)" != "$(host_before)" ]; then
		echo "cleanup: the host is not as found, restoring by hand"
		restore_by_hand
	fi
	rm -rf "$dev/data" "$dev"/h*.in "$dev"/h*.out
	echo "udmabuf after: $(udma_now)"
	if [ "$(host_now)" = "$(host_before)" ]; then
		echo "host settings as found"
	else
		echo "HOST SETTINGS NOT RESTORED:"; diff <(host_before) <(host_now)
	fi
	[ "$(udma_now)" = "$udma_before" ] && rm -f "$udma_saved"
}
trap cleanup EXIT

# --- a helper session: requests through a FIFO, replies through another ---
nh=0
start_helper() {   # label
	nh=$((nh + 1))
	rm -f "$dev/h$nh.in" "$dev/h$nh.out"
	mkfifo "$dev/h$nh.in" "$dev/h$nh.out"
	sudo -n "$helper" session < "$dev/h$nh.in" > "$dev/h$nh.out" 2>&1 &
	H_PID=$!
	helpers+=("$H_PID")
	exec {H_IN}> "$dev/h$nh.in"
	exec {H_OUT}< "$dev/h$nh.out"
	expect "$1: ready" "ready 1"
}
say() { echo "$1" >&"$H_IN"; }
next() { local l; IFS= read -r -t "${2:-15}" l <&"$H_OUT" && echo "$l"; }
LAST=
expect() {   # label, regex for the next line (in LAST)
	LAST=$(next)
	if [[ "$LAST" =~ ^$2$ ]]; then ok "$1: $LAST"; else bad "$1: got '$LAST', wanted '$2'"; fi
}
wait_for() {  # label, regex: lines until one matches (a minute at most)
	local l
	while l=$(next "" 60); do
		[[ "$l" =~ ^$2$ ]] && { ok "$1: $l"; return 0; }
		echo "       ($l)"
	done
	bad "$1: no line matching '$2'"
}
# the root helper: sudo's child, or sudo itself if it exec'ed it
helper_child() { local c; c=$(pgrep -P "$H_PID"); echo "${c:-$H_PID}"; }
# a process of ours ended: gone, or a zombie (kill -0 still finds those)
gone() { local st; st=$(ps -o stat= -p "$1" 2> /dev/null); [ -z "$st" ] || [ "${st#Z}" != "$st" ]; }
wait_gone() {   # pid, seconds: whether it ended in time
	local _
	for _ in $(seq "$2"); do gone "$1" && return 0; sleep 1; done
	gone "$1"
}

# a thread's stat field N (N >= 3), counted after its name, which may hold
# spaces ("CPU 0/KVM"): awk's $41 would be off by one there
stat_field() {   # stat-file N
	local s
	s=$(cat "$1" 2> /dev/null) || return 1
	# after "pid (name) ": field 3 first
	echo "${s##*) }" | awk -v n="$(($2 - 2))" '{print $n}'
}
threads_rt() {   # pid: "rt total" counts of the threads' policies (stat field 41: 1 = FIFO, 2 = RR)
	local t rt=0 all=0 pol
	for t in /proc/"$1"/task/*; do
		pol=$(stat_field "$t/stat" 41) || continue
		all=$((all + 1)); [ "$pol" = 1 ] || [ "$pol" = 2 ] && rt=$((rt + 1))
	done
	echo "$rt $all"
}
fair_all() { [ "$(fair_now | awk '{print $2 " " $3}' | sort -u)" = "$1" ]; }

ssh_vm() { ssh -p $port -i "$key" -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR tester@127.0.0.1 "$@"; }

# ======================================================================
echo "== 1. a VM's run: apply, check, power off, check restored"
[ -e "$dev/disk.qcow2" ] || qemu-img create -q -f qcow2 -b "$base" -F qcow2 "$dev/disk.qcow2"
[ -e "$dev/OVMF_VARS.fd" ] || cp /usr/share/edk2/ovmf/OVMF_VARS.fd "$dev/OVMF_VARS.fd"
"$qemu" -name "$name,debug-threads=on" -machine q35 -accel kvm -cpu host -smp 2 -m "$mem" \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
	-drive if=pflash,format=raw,file="$dev/OVMF_VARS.fd" \
	-drive file="$dev/disk.qcow2",if=virtio,cache=none,discard=unmap \
	-vga none -device virtio-gpu-pci -display none \
	-nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$port-:22 \
	-qmp unix:"$dev/qmp.sock",server=on,wait=off -serial file:"$dev/serial.log" \
	> "$dev/qemu.log" 2>&1 &
vm=$!
pids+=("$vm")
sleep 2
kill -0 $vm 2> /dev/null || { echo "QEMU did not start:"; cat "$dev/qemu.log"; exit 1; }
echo "  VM $name: QEMU $vm"

# shellcheck disable=SC2086
if [ -n "$udma_before" ]; then
	# for the next run, should this one end without its exit trap
	echo "$udma_before" > "$udma_saved"
	set_udma $udma_low
fi
start_helper "session 1"
say "watch $vm"; expect "watch" "ok watch $vm"
say "fair-server on"
if [ "$(echo "$fair_before" | awk '{print $2 " " $3}' | sort -u)" = "1000000000 50000000" ]; then
	expect "fair server" "ok fair-server on: $ncpu cpus at 10 ms / 1 ms \(was 1000 ms / 50 ms\)"
elif [ "$fair_todo" = 0 ]; then
	expect "fair server" "ok fair-server on: already 10 ms / 1 ms"
else
	expect "fair server" "ok fair-server on: $fair_todo cpus at 10 ms / 1 ms .*"
fi
if [ -n "$card" ]; then
	say "gpu-floor $card auto"; expect "gpu floor" "ok gpu-floor $card 1800 MHz \(was [0-9]+ MHz, level auto\)"
fi
say "rt $vm"; expect "rt" "ok rt $vm: ([0-9]+) of ([0-9]+) threads real-time"
check "rt: every thread" '[ "${BASH_REMATCH[1]:-x}" = "${BASH_REMATCH[2]:-y}" ]'
if [ -n "$udma_before" ]; then
	say "udmabuf $vm"
	expect "udmabuf" "ok udmabuf $vm: list_limit 65536 \(was ${udma_low% *}\), size_limit_mb 2048 \(was ${udma_low#* }\)"
	check "udmabuf raised: $(udma_now)" udma_raised
	check "udmabuf state recorded" 'sudo test -e /run/vitrine-helper/udmabuf.state'
fi
check "every cpu at 10 ms / 1 ms" 'fair_all "10000000 1000000"'
if [ -n "$card" ]; then
	check "gpu level manual" '[ "$(cat /sys/class/drm/$card/device/power_dpm_force_performance_level)" = manual ]'
	check "gpu OD_SCLK minimum 1800" 'grep -q "^0: *1800Mhz" /sys/class/drm/$card/device/pp_od_clk_voltage'
fi
read -r rt all <<< "$(threads_rt $vm)"
check "QEMU threads real-time: $rt of $all" '[ "$rt" = "$all" ]'
check "vCPU threads SCHED_FIFO 1" '[ -z "$(for t in /proc/$vm/task/*; do grep -q "^CPU" $t/comm && chrt -p ${t##*/} | grep -v "SCHED_FIFO\|priority: 1$"; done)" ]'
[ "$fair_todo" -gt 0 ] && check "state recorded in /run/vitrine-helper" 'sudo test -e /run/vitrine-helper/fair-server.state'
check "the journal has the record" 'sudo journalctl -q --since "-2min" -t vitrine-helper | grep -q "fair server\|udmabuf\|clock floor"'

echo "  waiting for the guest (ssh on $port)..."
for _ in $(seq 60); do ssh_vm true 2> /dev/null && break; sleep 3; done
if ssh_vm true 2> /dev/null; then
	ok "guest up"
	threads_before=$(ls /proc/$vm/task | sort)
	# disk work: QEMU starts worker threads now, from threads already real-time
	ssh_vm 'dd if=/dev/zero of=/var/tmp/e2e bs=1M count=200 oflag=direct status=none; sync; rm -f /var/tmp/e2e' 2> /dev/null
	threads_after=$(ls /proc/$vm/task | sort)
	new=$(comm -13 <(echo "$threads_before") <(echo "$threads_after") | wc -l)
	read -r rt all <<< "$(threads_rt $vm)"
	check "after guest disk work ($new threads started since rt): $rt of $all real-time" '[ "$rt" = "$all" ]'
	echo "  powering the guest off"
	ssh_vm 'sudo systemctl poweroff' 2> /dev/null
	wait_gone $vm 120
else
	bad "guest up (no ssh): stopping the VM through QMP"
	printf '{"execute":"qmp_capabilities"}\n{"execute":"system_powerdown"}\n' |
		socat -t 2 - UNIX-CONNECT:"$dev/qmp.sock" > /dev/null 2>&1
	wait_gone $vm 30
fi
# the helper restores only once QEMU is gone: never wait for it unbounded,
# with the host tuned meanwhile
if ! gone $vm; then
	bad "the guest did not power off: QEMU killed"
	kill -TERM $vm 2> /dev/null
	wait_gone $vm 10 || kill -KILL $vm 2> /dev/null
	wait_gone $vm 10
fi
wait_for "QEMU exit seen" "exited $vm"
[ -n "$udma_before" ] && wait_for "udmabuf restored" "restored udmabuf: list_limit ${udma_low% *}, size_limit_mb ${udma_low#* }"
[ "$fair_todo" -gt 0 ] && wait_for "fair server restored" "restored fair-server: [0-9]+ cpus back to [0-9]+ ms / [0-9]+ ms"
[ -n "$card" ] && wait_for "gpu restored" "restored gpu-floor $card: level auto"
wait_for "helper ends" "bye"
if ! wait_gone "$H_PID" 15; then
	# SIGTERM: it puts everything back before it ends
	bad "the helper did not end: terminated"
	exec {H_IN}>&-
	sudo kill -TERM "$(helper_child)" 2> /dev/null
	wait_gone "$H_PID" 10
fi
gone "$H_PID" && wait "$H_PID" 2> /dev/null
check "fair server as found" '[ "$(fair_now)" = "$fair_after" ]'
check "gpu as found" '[ "$(gpu_now)" = "$gpu_before" ]'
[ -n "$udma_before" ] && check "udmabuf back to $udma_low" '[ "$(udma_now)" = "$udma_low" ]'
check "nothing left in /run/vitrine-helper but its lock" '[ "$(sudo ls /run/vitrine-helper)" = lock ]'
gone $vm && wait $vm 2> /dev/null

# ======================================================================
echo "== 2. vitrine gone while the VM runs, then a helper killed holding the settings"
# a QEMU that starts at once: no machine, stopped
"$qemu" -machine none -display none -S -qmp unix:"$dev/q2.sock",server=on,wait=off > /dev/null 2>&1 &
q2=$!
pids+=("$q2")
sleep 1
start_helper "session 2"
say "watch $q2"; expect "watch" "ok watch $q2"
say "fair-server on"; expect "fair server" "ok fair-server on: .*"
[ -n "$card" ] && { say "gpu-floor $card auto"; expect "gpu floor" "ok gpu-floor $card .*"; }
[ -n "$udma_before" ] && { say "udmabuf $q2"; expect "udmabuf" "ok udmabuf $q2: .*"; }
exec {H_IN}>&-
sleep 1
check "stdin closed, the VM runs: helper still there" 'kill -0 $H_PID 2> /dev/null'
check "stdin closed, the VM runs: settings kept" 'fair_all "10000000 1000000"'
sudo kill -KILL "$(helper_child)"
wait "$H_PID" 2> /dev/null
check "helper killed: settings still applied" 'fair_all "10000000 1000000"'
[ "$fair_todo" -gt 0 ] && check "helper killed: its state is there" 'sudo test -e /run/vitrine-helper/fair-server.state'
[ -n "$udma_before" ] && check "helper killed: udmabuf still raised: $(udma_now)" udma_raised
out=$(sudo -n "$helper" session < /dev/null)
echo "$out" | sed 's/^/       /'
[ "$fair_todo" -gt 0 ] && check "the next helper restores them at its start" 'echo "$out" | grep -q "^restored fair-server"'
check "fair server as found" '[ "$(fair_now)" = "$fair_after" ]'
check "gpu as found" '[ "$(gpu_now)" = "$gpu_before" ]'
if [ -n "$udma_before" ]; then
	check "the next helper restores udmabuf too" 'echo "$out" | grep -q "^restored udmabuf"'
	check "udmabuf back to $udma_low" '[ "$(udma_now)" = "$udma_low" ]'
fi
kill $q2; wait $q2 2> /dev/null

# ======================================================================
echo "== 3. two helpers: the last one out restores"
"$qemu" -machine none -display none -S > /dev/null 2>&1 &
qa=$!
"$qemu" -machine none -display none -S > /dev/null 2>&1 &
qb=$!
pids+=("$qa" "$qb")
sleep 1
start_helper "helper A"; A_IN=$H_IN A_OUT=$H_OUT A_PID=$H_PID
say "watch $qa"; expect "A watch" "ok watch $qa"
say "fair-server on"; expect "A fair server" "ok fair-server on: .*"
[ -n "$udma_before" ] && { say "udmabuf $qa"; expect "A udmabuf" "ok udmabuf $qa: .*"; }
start_helper "helper B"; B_IN=$H_IN B_OUT=$H_OUT B_PID=$H_PID
say "watch $qb"; expect "B watch" "ok watch $qb"
say "fair-server on"; expect "B fair server" "ok fair-server on: set by another vitrine session"
[ -n "$udma_before" ] && { say "udmabuf $qb"; expect "B udmabuf" "ok udmabuf $qb: set by another vitrine session"; }
kill $qa; wait $qa 2> /dev/null
H_IN=$A_IN H_OUT=$A_OUT H_PID=$A_PID
wait_for "A ends" "bye"
check "A gone, B holds: still applied" 'fair_all "10000000 1000000"'
[ -n "$udma_before" ] && check "A gone, B holds: udmabuf still raised" udma_raised
kill $qb; wait $qb 2> /dev/null
H_IN=$B_IN H_OUT=$B_OUT H_PID=$B_PID
[ -n "$udma_before" ] && wait_for "B restores udmabuf" "restored udmabuf: .*"
[ "$fair_todo" -gt 0 ] && wait_for "B restores" "restored fair-server: .*"
wait_for "B ends" "bye"
check "fair server as found" '[ "$(fair_now)" = "$fair_after" ]'
[ -n "$udma_before" ] && check "udmabuf back to $udma_low" '[ "$(udma_now)" = "$udma_low" ]'

# ======================================================================
if [ -n "$udma_before" ]; then
	echo "== 3b. a udmabuf limit changed by hand meanwhile: left as it is"
	"$qemu" -machine none -display none -S > /dev/null 2>&1 &
	qc=$!
	pids+=("$qc")
	sleep 1
	start_helper "helper C"
	say "watch $qc"; expect "C watch" "ok watch $qc"
	say "udmabuf $qc"; expect "C udmabuf" "ok udmabuf $qc: .*"
	# still above what a guest window needs, for VMs running meanwhile
	echo 40000 | sudo tee $udma/list_limit > /dev/null
	kill $qc; wait $qc 2> /dev/null
	wait_for "C restores the other one" "restored udmabuf: size_limit_mb ${udma_low#* }"
	wait_for "C leaves the one changed" "left udmabuf: list_limit changed by someone else since"
	wait_for "C ends" "bye"
	check "list_limit as changed, size_limit_mb back: $(udma_now)" '[ "$(udma_now)" = "40000 ${udma_low#* }" ]'
	# shellcheck disable=SC2086
	set_udma $udma_low
fi

# ======================================================================
if [ "$fair_todo" = "$ncpu" ] && [ "$(echo "$fair_base" | wc -l)" = 1 ]; then
	echo "== 3c. an earlier run's leftovers: two cpus at 10 ms / 1 ms, no record"
	"$qemu" -machine none -display none -S > /dev/null 2>&1 &
	q3c=$!
	pids+=("$q3c")
	sleep 1
	# as a run cut short would leave them; the others as found
	for c in cpu0 cpu1; do
		echo 1000000 | sudo tee $fs/$c/runtime > /dev/null
		echo 10000000 | sudo tee $fs/$c/period > /dev/null
	done
	start_helper "helper D"
	say "watch $q3c"; expect "D watch" "ok watch $q3c"
	say "fair-server on"
	expect "D finds them" "ok fair-server on: $((ncpu - 2)) cpus at 10 ms / 1 ms \(was [0-9]+ ms / [0-9]+ ms\); 2 cpus found at 10 ms / 1 ms without a record, put back after with the others"
	say "release"
	wait_for "D puts every cpu back" "restored fair-server: $ncpu cpus back to [0-9]+ ms / [0-9]+ ms"
	wait_for "D ends" "bye"
	check "every cpu as found, the leftovers too" '[ "$(fair_now)" = "$fair_before" ]'
	kill $q3c; wait $q3c 2> /dev/null
fi

# ======================================================================
echo "== 4. focus priority on a real QEMU: rt in front, behind (vCPUs at nice -5), back"
"$qemu" -L "$pcbios" -name e2e-focus,debug-threads=on -machine q35 -accel kvm -smp 2 -m 256M \
	-display none -S > "$dev/q4.log" 2>&1 &
q4=$!
pids+=("$q4")
sleep 1
# "policy nice" of a thread: /proc/PID/task/TID/stat fields 41 and 19
thread_sched() { echo "$(stat_field "/proc/$q4/task/$1/stat" 41) $(stat_field "/proc/$q4/task/$1/stat" 19)"; }
vcpus4() { local t; for t in /proc/$q4/task/*; do grep -q "^CPU [0-9]*/KVM" "$t/comm" && echo "${t##*/}"; done; }
others4() { local t; for t in /proc/$q4/task/*; do grep -q "^CPU [0-9]*/KVM" "$t/comm" || echo "${t##*/}"; done; }
all_threads() {   # expected "policy nice" for the vCPUs, then for the others
	local t
	for t in $(vcpus4); do [ "$(thread_sched "$t")" = "$1" ] || return 1; done
	for t in $(others4); do [ "$(thread_sched "$t")" = "$2" ] || return 1; done
	[ -n "$(vcpus4)" ]
}
check "QEMU's vCPU threads named CPU n/KVM: $(vcpus4 | wc -l)" '[ "$(vcpus4 | wc -l)" = 2 ]'
start_helper "session 4"
say "watch $q4"; expect "watch" "ok watch $q4"
say "fair-server on"; expect "fair server" "ok fair-server on: .*"
say "rt $q4"; expect "rt (in front)" "ok rt $q4: ([0-9]+) of ([0-9]+) threads real-time"
check "in front: every thread SCHED_FIFO" 'all_threads "1 0" "1 0"'
say "behind $q4"; expect "behind" "ok behind $q4: [0-9]+ threads ordinary, 2 vCPUs at nice -5"
check "behind: vCPUs SCHED_OTHER nice -5, the others SCHED_OTHER nice 0" 'all_threads "0 -5" "0 0"'
say "rt $q4"; expect "rt (in front again)" "ok rt $q4: .*"
check "in front again: every thread SCHED_FIFO" 'all_threads "1 -5" "1 0"'
say "behind $q4"; expect "behind again" "ok behind $q4: .*"
say "release"
wait_for "release puts the threads back" "restored rt $q4: [0-9]+ threads back to SCHED_OTHER, vCPUs back to nice 0"
wait_for "helper ends" "bye"
check "released: every thread SCHED_OTHER nice 0" 'all_threads "0 0" "0 0"'
check "fair server as found" '[ "$(fair_now)" = "$fair_after" ]'
kill $q4; wait $q4 2> /dev/null

echo "== 4b. the capability an older vitrine gave its QEMU: taken back by the app"
stackbin=$dev/data/vitrine/stack/e2e0000000000000/bin
mkdir -p "$stackbin"
cp "$qemu" "$stackbin/qemu-system-x86_64"
sudo -n setcap cap_sys_nice=ep "$stackbin/qemu-system-x86_64"
check "set by hand, as an older helper did: $(getcap "$stackbin/qemu-system-x86_64")" \
	'getcap "$stackbin/qemu-system-x86_64" | grep -q "cap_sys_nice=ep"'
# a VM running from it meanwhile: the capability goes all the same (no write)
"$stackbin/qemu-system-x86_64" -L "$pcbios" -machine none -display none -S > /dev/null 2>&1 &
q4b=$!
pids+=("$q4b")
sleep 1
if [ -x "$build/app/test_hostsettings" ]; then
	out=$(VITRINE_TEST_CAP_STACK=$dev/data/vitrine/stack "$build/app/test_hostsettings" capabilityStripped 2>&1)
	check "the app took it back" 'echo "$out" | grep -q "^PASS   : TestHostSettings::capabilityStripped()"'
else
	echo "  (no $build/app/test_hostsettings: skipped)"
fi
check "getcap shows none" '[ -z "$(getcap "$stackbin/qemu-system-x86_64")" ]'
kill $q4b; wait $q4b 2> /dev/null
rm -rf "$dev/data"

# ======================================================================
echo "== 5. the app's side: HostSettings with this helper through sudo"
if [ -x "$build/app/test_hostsettings" ]; then
	out=$(VITRINE_HELPER_E2E=$helper VITRINE_TEST_QEMU=$qemu "$build/app/test_hostsettings" realHost 2>&1)
	echo "$out" | grep -E "helper:|FAIL|Loc:|Actual|Expected" | sed 's/^/       /'
	check "HostSettings applied and reverted on the real host" 'echo "$out" | grep -q "^PASS   : TestHostSettings::realHost()"'
	check "fair server as found" '[ "$(fair_now)" = "$fair_after" ]'
	check "gpu as found" '[ "$(gpu_now)" = "$gpu_before" ]'
	[ -n "$udma_before" ] && check "udmabuf back to $udma_low" '[ "$(udma_now)" = "$udma_low" ]'
else
	echo "  (no $build/app/test_hostsettings: skipped)"
fi

echo
echo "fair server now: $(fair_now | awk '{print $2 "/" $3}' | sort | uniq -c | tr '\n' ' ')"
if [ "$fails" = 0 ]; then echo "PASS"; else echo "$fails FAILED"; fi
exit $((fails > 0))
