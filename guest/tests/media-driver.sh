#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The installer's choice of the Intel media driver (guest/tools/install:
# media_driver_choice), its functions taken out of it as they are, with a
# stand-in for rpm (the installed "NAME ARCH" in a file).
#   The medium's takes the place of RPM Fusion's where the guest has it. Not
#   without RPM Fusion's: Fedora's libva-intel-media-driver does not count. Not
#   with its 32-bit package too, nor from a medium without it.
set -u
here=$(dirname "$(readlink -f "$0")")
install=$here/../tools/install
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0
mkdir "$work/bin"
cat > "$work/bin/rpm" <<'EOS'
#!/bin/bash
# rpm -q --qf '%{ARCH}\n' NAME
[ "$1" = -q ] && [ "$2" = --qf ] || exit 2
awk -v n="$4" '$1 == n { print $2; found = 1 } END { exit !found }' "$FAKE_DB"
EOS
chmod +x "$work/bin/rpm"
PATH=$work/bin:$PATH
export FAKE_DB=$work/db
packages_txt=$work/packages.txt
for f in medium_nevra installed media_driver_choice; do
	code=$(sed -n "/^$f() {/,/^}/p" "$install")
	[ -n "$code" ] || { echo "FAIL: no $f in $install"; exit 1; }
	eval "$code"
done

ours="intel-media-driver 26.3.5-1.1.xe.fc45 x86_64 intel-media-driver.rpm"
# check NAME MEDIUM(line of packages.txt) INSTALLED("NAME ARCH" lines) WANTED NOTE(a word of it)
check() {
	printf '%s\n' "$2" > "$packages_txt"
	printf '%s\n' "$3" > "$FAKE_DB"
	media_driver_choice
	[ "${media_driver[*]:-}" = "$4" ] || { echo "FAIL: $1: took '${media_driver[*]:-}', not '$4'"; fail=1; }
	case "$media_driver_note" in
	*"$5"*) ;;
	*) echo "FAIL: $1: said '$media_driver_note'"; fail=1 ;;
	esac
	[ -n "$5" ] || [ -z "$media_driver_note" ] || { echo "FAIL: $1: said '$media_driver_note'"; fail=1; }
}
nevra=intel-media-driver-26.3.5-1.1.xe.fc45.x86_64
check "RPM Fusion's installed" "$ours" "intel-media-driver x86_64" "$nevra" "LIBVA_DRIVER_NAME=iHD"
check "another package only" "$ours" "mesa-dri-drivers x86_64" "" ""
check "Fedora's driver only" "$ours" "libva-intel-media-driver x86_64" "" ""
check "its 32-bit package too" "$ours" "intel-media-driver x86_64
intel-media-driver i686" "" "32-bit"
check "32-bit only" "$ours" "intel-media-driver i686" "" ""
check "a medium without it" "mesa-dri-drivers 26.2.6-2.xe.fc45 x86_64 m.rpm" "intel-media-driver x86_64" "" ""
[ "$fail" = 0 ] && echo "media driver choice: all cases pass"
exit "$fail"
