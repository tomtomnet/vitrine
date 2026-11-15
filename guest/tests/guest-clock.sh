#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The guest's clock (guest/tools/libexec/guest-clock and chronyd.service's
# drop-in), with sysfs's ptp class, /dev, modprobe and chronyd's sysconfig in
# a scratch folder: KVM's PTP clock as the preferred source when there is
# one, none for another kind of PTP clock or a missing device, none either
# when the configuration has a reference clock of its own; the include of
# OPTIONS' -f file; corrections capped whatever the source; the drop-in
# naming the script and its file, and handing chronyd what it wrote (checked
# with chronyd -p where chronyd is installed).
set -u
here=$(dirname "$(readlink -f "$0")")
script=$here/../tools/libexec/guest-clock
dropin=$here/../tools/systemd/chronyd.service.d/vitrine-guest-tools.conf
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0

bad() {
	echo "FAIL: $*"
	fail=1
}

# a fresh guest: no PTP clock until modprobe ptp_kvm makes ptp0 (FAKE_KVM=1),
# Fedora's chrony.conf and sysconfig
setup() {
	rm -rf "$work/g" && mkdir -p "$work/g/class" "$work/g/dev" "$work/g/run" "$work/g/bin" "$work/g/etc"
	printf 'pool 2.fedora.pool.ntp.org iburst\nsourcedir /run/chrony-dhcp\nmakestep 1.0 3\nrtcsync\n' > "$work/g/etc/chrony.conf"
	echo 'OPTIONS="-F 2"' > "$work/g/etc/sysconfig"
	cat > "$work/g/bin/modprobe" <<'EOF'
#!/bin/bash
echo "$*" >> "$G/modprobe.calls"
if [ "$*" = "-q ptp_kvm" ] && [ "${FAKE_KVM:-0}" = 1 ]; then
	mkdir -p "$G/class/ptp0" && echo "KVM virtual PTP" > "$G/class/ptp0/clock_name"
	touch "$G/dev/ptp0"
	exit 0
fi
exit 1
EOF
	chmod +x "$work/g/bin/modprobe"
}

run() {
	G=$work/g VITRINE_CLOCK_RUN=$work/g/run VITRINE_CHRONY_SYSCONFIG=$work/g/etc/sysconfig \
		VITRINE_CHRONY_CONF=$work/g/etc/chrony.conf \
		VITRINE_PTP_CLASS=$work/g/class VITRINE_DEV=$work/g/dev VITRINE_MODPROBE=$work/g/bin/modprobe \
		bash "$script" > "$work/g/out" 2>&1
	echo $? > "$work/g/status"
}

# VAR's value in the written file, as systemd reads it (the quotes taken off)
value() {
	sed -n "s/^$1=\"\\(.*\\)\"\$/\\1/p" "$work/g/run/guest-clock.env"
}

expect() {
	local name=$1 want=$2 got
	got=$(value "$name")
	[ "$got" = "$want" ] || bad "$case: $name is '$got', not '$want'"
}

# 1. KVM's PTP clock after modprobe: the preferred source, the caps
case=kvm
setup
FAKE_KVM=1 run
[ "$(cat "$work/g/status")" = 0 ] || bad "$case: exit $(cat "$work/g/status")"
grep -qx -- '-q ptp_kvm' "$work/g/modprobe.calls" || bad "$case: ptp_kvm not loaded"
expect VITRINE_CLOCK_CONF "include $work/g/etc/chrony.conf"
expect VITRINE_CLOCK_REF "refclock PHC $work/g/dev/ptp0 poll 2 dpoll -2 prefer"
expect VITRINE_CLOCK_STEP "makestep 0.1 -1"
expect VITRINE_CLOCK_SLEW "maxslewrate 1000"
grep -q "the host's clock through $work/g/dev/ptp0" "$work/g/out" || bad "$case: not said: $(cat "$work/g/out")"
[ ! -e "$work/g/run/guest-clock.env.new" ] || bad "$case: its temporary file left"

# 2. no KVM PTP clock (modprobe fails): Fedora's sources, capped all the same
case=none
setup
FAKE_KVM=0 run
[ "$(cat "$work/g/status")" = 0 ] || bad "$case: exit $(cat "$work/g/status")"
expect VITRINE_CLOCK_REF ""
expect VITRINE_CLOCK_STEP "makestep 0.1 -1"
expect VITRINE_CLOCK_SLEW "maxslewrate 1000"
grep -q "no KVM PTP clock" "$work/g/out" || bad "$case: not said: $(cat "$work/g/out")"

# 3. another kind of PTP clock (a NIC's, Hyper-V's): not the host's
case=other
setup
mkdir -p "$work/g/class/ptp0" && echo hyperv > "$work/g/class/ptp0/clock_name" && touch "$work/g/dev/ptp0"
FAKE_KVM=0 run
expect VITRINE_CLOCK_REF ""

# 4. KVM's clock second, after a NIC's: that one, by its own name
case=second
setup
mkdir -p "$work/g/class/ptp0" "$work/g/class/ptp1" && echo "igb ptp" > "$work/g/class/ptp0/clock_name"
echo "KVM virtual PTP" > "$work/g/class/ptp1/clock_name" && touch "$work/g/dev/ptp0" "$work/g/dev/ptp1"
FAKE_KVM=0 run
expect VITRINE_CLOCK_REF "refclock PHC $work/g/dev/ptp1 poll 2 dpoll -2 prefer"

# 5. the class says so, the device is not there: no source that would fail chronyd
case=nodev
setup
mkdir -p "$work/g/class/ptp0" && echo "KVM virtual PTP" > "$work/g/class/ptp0/clock_name"
FAKE_KVM=0 run
expect VITRINE_CLOCK_REF ""

# 6. a reference clock of the user's own: left as the only one
case=ownref
setup
printf '# pool 2.fedora.pool.ntp.org iburst\n  refclock PHC /dev/ptp0 poll 3\n' > "$work/g/etc/own.conf"
echo "OPTIONS=\"-F 2 -f $work/g/etc/own.conf\"" > "$work/g/etc/sysconfig"
FAKE_KVM=1 run
expect VITRINE_CLOCK_CONF "include $work/g/etc/own.conf"
expect VITRINE_CLOCK_REF ""
expect VITRINE_CLOCK_SLEW "maxslewrate 1000"
grep -q "a reference clock of its own" "$work/g/out" || bad "$case: not said: $(cat "$work/g/out")"

# 7. OPTIONS' -f, single quotes, no reference clock in it: that file, with the host's clock
case=minusf
setup
cp "$work/g/etc/chrony.conf" "$work/g/etc/other.conf"
echo "OPTIONS='-f $work/g/etc/other.conf -F 2'" > "$work/g/etc/sysconfig"
FAKE_KVM=1 run
expect VITRINE_CLOCK_CONF "include $work/g/etc/other.conf"
expect VITRINE_CLOCK_REF "refclock PHC $work/g/dev/ptp0 poll 2 dpoll -2 prefer"

# 8. a commented-out refclock is no reference clock
case=comment
setup
echo '#refclock PHC /dev/ptp0' >> "$work/g/etc/chrony.conf"
FAKE_KVM=1 run
expect VITRINE_CLOCK_REF "refclock PHC $work/g/dev/ptp0 poll 2 dpoll -2 prefer"

# 9. the drop-in: the script before chronyd, outside its sandbox (ptp_kvm: chronyd's unit
# protects kernel modules), failing it no failure; its file read,
# chronyd's own ExecStart replaced by one with the four directives after OPTIONS
case=dropin
grep -qx 'ExecStartPre=-+/usr/libexec/vitrine-guest-tools/guest-clock' "$dropin" || bad "$case: ExecStartPre (outside chronyd's sandbox, failure allowed)"
grep -qx 'EnvironmentFile=-/run/vitrine-guest-tools/guest-clock.env' "$dropin" || bad "$case: EnvironmentFile"
grep -q '^run=${VITRINE_CLOCK_RUN:-/run/vitrine-guest-tools}$' "$script" || bad "$case: the script's folder"
grep -q '^conf=${VITRINE_CHRONY_CONF:-/etc/chrony.conf}$' "$script" || bad "$case: the script's default configuration"
grep -qx 'ExecStart=' "$dropin" || bad "$case: chronyd's ExecStart not reset"
start=$(sed -n 's/^ExecStart=\(.\)/\1/p' "$dropin")
[ "$start" = '/usr/sbin/chronyd -n $OPTIONS ${VITRINE_CLOCK_CONF} ${VITRINE_CLOCK_REF} ${VITRINE_CLOCK_STEP} ${VITRINE_CLOCK_SLEW}' ] ||
	bad "$case: ExecStart is $start"
grep -q '^Environment="VITRINE_CLOCK_CONF=include /etc/chrony.conf" VITRINE_CLOCK_REF= VITRINE_CLOCK_STEP= VITRINE_CLOCK_SLEW=$' "$dropin" ||
	bad "$case: no fallback to chronyd's own configuration"

# 10. what chronyd makes of it (case 1's file): the configuration, then the host's clock and the caps
case=chronyd
if [ -x /usr/sbin/chronyd ]; then
	setup
	FAKE_KVM=1 run
	got=$(/usr/sbin/chronyd -p "$(value VITRINE_CLOCK_CONF)" "$(value VITRINE_CLOCK_REF)" "$(value VITRINE_CLOCK_STEP)" \
		"$(value VITRINE_CLOCK_SLEW)" 2>&1)
	want="pool 2.fedora.pool.ntp.org iburst
sourcedir /run/chrony-dhcp
makestep 1.0 3
rtcsync
refclock PHC $work/g/dev/ptp0 poll 2 dpoll -2 prefer
makestep 0.1 -1
maxslewrate 1000"
	[ "$got" = "$want" ] || bad "$case: chronyd -p gave
$got"
	# guest-clock not run: chronyd's own configuration, nothing else
	got=$(/usr/sbin/chronyd -p "include $work/g/etc/chrony.conf" "" "" "" 2>&1)
	[ "$got" = "$(cat "$work/g/etc/chrony.conf")" ] || bad "$case: the fallback gave
$got"
else
	echo "chronyd not installed here: case $case skipped"
fi

[ "$fail" = 0 ] && echo "guest-clock: all cases passed"
exit "$fail"
