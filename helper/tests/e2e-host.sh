#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# vitrine-helper end to end on this host, as root through sudo (pkexec would
# need the polkit files installed): a test VM boots, the helper watches its
# QEMU and applies the fair server, the GPU clock floor (an AMD APU) and
# real-time threads; the VM powers off and everything must be back as it
# was.  Then: a helper killed while holding the settings (the next one
# restores them), vitrine gone while the VM runs (stdin closed), and the
# file capability on a copy of QEMU, with x-vcpu-priority through it, and
# the app's HostSettings driving this helper.
#
#   helper/tests/e2e-host.sh [BUILD_DIR]
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
port=2234
name=vitrine-helper-e2e
dev=$repo/.dev/helper-e2e
fs=/sys/kernel/debug/sched/fair_server

fails=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

[ -x "$helper" ] || { echo "no $helper: build first" >&2; exit 2; }
sudo -n true 2> /dev/null || { echo "needs password-less sudo" >&2; exit 2; }
# a 2 GiB VM: the host keeps 10 GiB free first (notes/agents/COMMON.md);
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
fair_before=$(fair_now)
gpu_before=$(gpu_now)
ncpu=$(echo "$fair_before" | wc -l)
echo "host: $ncpu cpus, fair server $(echo "$fair_before" | head -1 | cut -d' ' -f2-), gpu ${card:-none} level $(echo "$gpu_before" | head -1)"
[ "$(echo "$fair_before" | head -1 | cut -d' ' -f2-)" = "1000000000 50000000" ] ||
	echo "  note: the fair server is not at the kernel's default (another tool holds it?)"

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
	done <<< "$fair_before"
	if [ -n "$card" ] && [ "$(cat /sys/class/drm/$card/device/power_dpm_force_performance_level)" = manual ] &&
		[ "$(echo "$gpu_before" | head -1)" != manual ]; then
		echo r | sudo tee /sys/class/drm/$card/device/pp_od_clk_voltage > /dev/null
		echo c | sudo tee /sys/class/drm/$card/device/pp_od_clk_voltage > /dev/null
		echo "$gpu_before" | head -1 | sudo tee /sys/class/drm/$card/device/power_dpm_force_performance_level > /dev/null
	fi
}
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
	if [ "$(fair_now)" != "$fair_before" ] || [ "$(gpu_now)" != "$gpu_before" ]; then
		echo "cleanup: the host is not as found, restoring by hand"
		restore_by_hand
	fi
	rm -rf "$dev/data" "$dev"/h*.in "$dev"/h*.out
	if [ "$(fair_now)" = "$fair_before" ] && [ "$(gpu_now)" = "$gpu_before" ]; then
		echo "host settings as found"
	else
		echo "HOST SETTINGS NOT RESTORED:"; diff <(echo "$fair_before"; echo "$gpu_before") <(fair_now; gpu_now)
	fi
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

threads_rt() {   # pid: "rt total" counts of the threads' policies (stat field 41: 1 = FIFO, 2 = RR)
	local t rt=0 all=0 pol
	for t in /proc/"$1"/task/*; do
		pol=$(awk '{print $41}' "$t/stat" 2> /dev/null) || continue
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
"$qemu" -name "$name,debug-threads=on" -machine q35 -accel kvm -cpu host -smp 2 -m 2G \
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

start_helper "session 1"
say "watch $vm"; expect "watch" "ok watch $vm"
say "fair-server on"
if [ "$(echo "$fair_before" | head -1 | cut -d' ' -f2-)" = "1000000000 50000000" ]; then
	expect "fair server" "ok fair-server on: $ncpu cpus at 10 ms / 1 ms \(was 1000 ms / 50 ms\)"
else
	expect "fair server" "ok fair-server on: .*"
fi
if [ -n "$card" ]; then
	say "gpu-floor $card auto"; expect "gpu floor" "ok gpu-floor $card 1800 MHz \(was [0-9]+ MHz, level auto\)"
fi
say "rt $vm"; expect "rt" "ok rt $vm: ([0-9]+) of ([0-9]+) threads real-time"
check "rt: every thread" '[ "${BASH_REMATCH[1]:-x}" = "${BASH_REMATCH[2]:-y}" ]'
check "every cpu at 10 ms / 1 ms" 'fair_all "10000000 1000000"'
if [ -n "$card" ]; then
	check "gpu level manual" '[ "$(cat /sys/class/drm/$card/device/power_dpm_force_performance_level)" = manual ]'
	check "gpu OD_SCLK minimum 1800" 'grep -q "^0: *1800Mhz" /sys/class/drm/$card/device/pp_od_clk_voltage'
fi
read -r rt all <<< "$(threads_rt $vm)"
check "QEMU threads real-time: $rt of $all" '[ "$rt" = "$all" ]'
check "vCPU threads SCHED_FIFO 1" '[ -z "$(for t in /proc/$vm/task/*; do grep -q "^CPU" $t/comm && chrt -p ${t##*/} | grep -v "SCHED_FIFO\|priority: 1$"; done)" ]'
check "state recorded in /run/vitrine-helper" 'sudo test -e /run/vitrine-helper/fair-server.state'
check "the journal has the record" 'sudo journalctl -q --since "-2min" -t vitrine-helper | grep -q "fair server"'

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
wait_for "fair server restored" "restored fair-server: $ncpu cpus back to [0-9]+ ms / [0-9]+ ms"
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
check "fair server as found" '[ "$(fair_now)" = "$fair_before" ]'
check "gpu as found" '[ "$(gpu_now)" = "$gpu_before" ]'
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
exec {H_IN}>&-
sleep 1
check "stdin closed, the VM runs: helper still there" 'kill -0 $H_PID 2> /dev/null'
check "stdin closed, the VM runs: settings kept" 'fair_all "10000000 1000000"'
sudo kill -KILL "$(helper_child)"
wait "$H_PID" 2> /dev/null
check "helper killed: settings still applied" 'fair_all "10000000 1000000"'
check "helper killed: its state is there" 'sudo test -e /run/vitrine-helper/fair-server.state'
out=$(sudo -n "$helper" session < /dev/null)
echo "$out" | sed 's/^/       /'
check "the next helper restores them at its start" 'echo "$out" | grep -q "^restored fair-server"'
check "fair server as found" '[ "$(fair_now)" = "$fair_before" ]'
check "gpu as found" '[ "$(gpu_now)" = "$gpu_before" ]'
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
start_helper "helper B"; B_IN=$H_IN B_OUT=$H_OUT B_PID=$H_PID
say "watch $qb"; expect "B watch" "ok watch $qb"
say "fair-server on"; expect "B fair server" "ok fair-server on: set by another vitrine session"
kill $qa; wait $qa 2> /dev/null
H_IN=$A_IN H_OUT=$A_OUT H_PID=$A_PID
wait_for "A ends" "bye"
check "A gone, B holds: still applied" 'fair_all "10000000 1000000"'
kill $qb; wait $qb 2> /dev/null
H_IN=$B_IN H_OUT=$B_OUT H_PID=$B_PID
wait_for "B restores" "restored fair-server: .*"
wait_for "B ends" "bye"
check "fair server as found" '[ "$(fair_now)" = "$fair_before" ]'

# ======================================================================
echo "== 4. setcap on a copy of QEMU in a stack layout, x-vcpu-priority through it"
stackbin=$dev/data/vitrine/stack/e2e0000000000000/bin
mkdir -p "$stackbin"
cp "$qemu" "$stackbin/qemu-system-x86_64"
out=$(sudo -n "$helper" setcap "$stackbin/qemu-system-x86_64")
check "setcap: $out" '[ "$out" = "ok setcap $stackbin/qemu-system-x86_64: cap_sys_nice=ep, mode 0700" ]'
check "getcap shows cap_sys_nice=ep" 'getcap "$stackbin/qemu-system-x86_64" | grep -q "cap_sys_nice=ep"'
out=$(sudo -n "$helper" setcap "$qemu")
check "setcap refuses the export's QEMU: $out" 'echo "$out" | grep -q "^error setcap .*: not a QEMU of vitrine.s stack"'
out=$(sudo -n "$helper" setcap /usr/bin/python3)
check "setcap refuses a system file: $out" 'echo "$out" | grep -q "^error setcap /usr/bin/python3: not in the caller.s home folder"'
"$stackbin/qemu-system-x86_64" -L "$pcbios" -name e2e-cap,debug-threads=on -machine q35 -accel kvm -smp 2 -m 256M -display none -S \
	-qmp unix:"$dev/q4.sock",server=on,wait=off > "$dev/q4.log" 2>&1 &
q4=$!
pids+=("$q4")
sleep 1
check "the capable QEMU runs" 'kill -0 $q4'
check "it has CAP_SYS_NICE effective" '[ $(( 0x$(awk "/^CapEff/ {print \$2}" /proc/$q4/status) >> 23 & 1 )) = 1 ]'
qmp() { printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" | socat -t 2 - UNIX-CONNECT:"$dev/q4.sock" | tail -1 | tr -d '\r'; }
r=$(qmp '{"execute":"x-vcpu-priority","arguments":{"realtime":true}}')
check "x-vcpu-priority realtime: $r" '[ "$r" = "{\"return\": {}}" ]'
vcpus_fifo() { local t n=0; for t in /proc/$q4/task/*; do grep -q "^CPU" $t/comm || continue; chrt -p ${t##*/} | grep -q "policy: SCHED_FIFO" || return 1; n=$((n + 1)); done; [ $n -gt 0 ]; }
check "its vCPUs SCHED_FIFO" vcpus_fifo
r=$(qmp '{"execute":"x-vcpu-priority","arguments":{"realtime":false}}')
check "x-vcpu-priority back to ordinary (nice -5): $r" '[ "$r" = "{\"return\": {}}" ] && ! vcpus_fifo'
check "and back to real-time again (needs the capability)" '[ "$(qmp "{\"execute\":\"x-vcpu-priority\",\"arguments\":{\"realtime\":true}}")" = "{\"return\": {}}" ] && vcpus_fifo'
kill $q4; wait $q4 2> /dev/null
# a write drops the capability, as the kernel does on every write
cat "$qemu" > "$stackbin/qemu-system-x86_64"
check "rewritten: the capability is gone" '[ -z "$(getcap "$stackbin/qemu-system-x86_64")" ]'
rm -rf "$dev/data"

# ======================================================================
echo "== 5. the app's side: HostSettings with this helper through sudo"
if [ -x "$build/app/test_hostsettings" ]; then
	out=$(VITRINE_HELPER_E2E=$helper VITRINE_TEST_QEMU=$qemu "$build/app/test_hostsettings" realHost 2>&1)
	echo "$out" | grep -E "helper:|FAIL|Loc:|Actual|Expected" | sed 's/^/       /'
	check "HostSettings applied and reverted on the real host" 'echo "$out" | grep -q "^PASS   : TestHostSettings::realHost()"'
	check "fair server as found" '[ "$(fair_now)" = "$fair_before" ]'
	check "gpu as found" '[ "$(gpu_now)" = "$gpu_before" ]'
else
	echo "  (no $build/app/test_hostsettings: skipped)"
fi

echo
if [ "$fails" = 0 ]; then echo "PASS"; else echo "$fails FAILED"; fi
exit $((fails > 0))
