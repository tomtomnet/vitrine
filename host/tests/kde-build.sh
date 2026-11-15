#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# host/kde/build.sh's helpers, taken out of it as they are.  Why a host build
# failed (failed_why), from the build logs of rpm 6.1 as the research's
# recipe leaves them: the patch that does not apply in %prep named, with the
# package and version rpm was preparing; a failure past %prep, or in %prep
# before any patch, or no log at all: the recipe's status.  The cached
# metadata of every repository expired before a build, the packages kept
# (expire_metadata).  How many parts make a release (release_parts, of
# components.conf), and which release is older (older_release).
set -u
here=$(dirname "$(readlink -f "$0")")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL: $*"; fail=1; }
for f in say section_of failed_why release_parts older_release expire_metadata; do
	eval "$(sed -n "/^$f() {/,/^}/p" "$here/../kde/build.sh")"
	declare -F "$f" > /dev/null || { echo "FAIL: no $f in build.sh"; exit 1; }
done
here=$here/../kde
package=kwin

# rpm 6.1's %prep, as rpmbuild -bp prints it (checked 2026-11-15), up to $1
prep() {
	cat <<'LOG'
Executing(%prep): /bin/sh -e /var/tmp/rpm-tmp.A2ROSM
+ umask 022
+ cd /builddir/build/BUILD/kwin-6.8.4-build
+ rm -rf kwin-6.8.4
+ /usr/bin/rpmuncompress -x /builddir/build/SOURCES/kwin-6.8.4.tar.xz
+ STATUS=0
+ cd kwin-6.8.4
+ /usr/bin/rpmuncompress /builddir/build/SOURCES/0001-Patch100-kwin-vm-frame-timing-ported-to-6.8.patch
+ /usr/bin/patch -p1 -s --fuzz=0 --no-backup-if-mismatch -f
+ /usr/bin/rpmuncompress /builddir/build/SOURCES/0074-EXP-renderloop-lone-frame-lead-hl.patch
+ /usr/bin/patch -p1 -s --fuzz=0 --no-backup-if-mismatch -f
1 out of 3 hunks FAILED -- saving rejects to file src/core/renderloop.cpp.rej
error: Bad exit status from /var/tmp/rpm-tmp.A2ROSM (%prep)
LOG
}

case=patch
logs=$work/patch && mkdir -p "$logs" && prep > "$logs/build-h681zz2_no82.log"
got=$(failed_why 1)
[[ $got == "the patch 0074-EXP-renderloop-lone-frame-lead-hl.patch does not apply to kwin 6.8.4: "* ]] ||
	bad "$case: $got"
[[ $got == *"a newer Vitrine may have them for this one (log: $logs/build-h681zz2_no82.log)" ]] ||
	bad "$case: no remedy or log: $got"

case=unpack
logs=$work/unpack && mkdir -p "$logs"
prep | sed '/^+ cd kwin-6.8.4$/,/^1 out of/d' > "$logs/build-h681zz2_no82.log"
got=$(failed_why 1)
[ "$got" = "the research's kde-rpms.sh failed in preparing the sources (status 1, log: $logs/build-h681zz2_no82.log)" ] ||
	bad "$case: $got"

case=build
logs=$work/build && mkdir -p "$logs"
prep | sed 's/^error: Bad exit status from \(.*\) (%prep)$/+ cmake --build/' > "$logs/build-h681zz2_no82.log"
echo 'error: Bad exit status from /var/tmp/rpm-tmp.B (%build)' >> "$logs/build-h681zz2_no82.log"
got=$(failed_why 1)
[ "$got" = "the research's kde-rpms.sh failed (status 1)" ] || bad "$case: $got"

case=nolog
logs=$work/none
got=$(failed_why 2)
[ "$got" = "the research's kde-rpms.sh failed (status 2)" ] || bad "$case: $got"

case=nobuilddir
logs=$work/nobuilddir && mkdir -p "$logs"
prep | sed '/BUILD\/kwin-6.8.4-build$/d' > "$logs/build-h683a_no82.log"
got=$(failed_why 1)
[[ $got == "the patch 0074-EXP-renderloop-lone-frame-lead-hl.patch does not apply to kwin: "* ]] ||
	bad "$case: $got"

# --- the releases ---
case=parts
[ "$(release_parts kwin)" = 3 ] || bad "$case: kwin $(release_parts kwin)"
[ "$(release_parts kf6-ksvg)" = 2 ] || bad "$case: kf6-ksvg $(release_parts kf6-ksvg)"
[ "$(release_parts kf6-kirigami)" = 2 ] || bad "$case: kf6-kirigami $(release_parts kf6-kirigami)"
[ "$(release_parts qt6-qtbase)" = 0 ] || bad "$case: qt6-qtbase $(release_parts qt6-qtbase)"
case=older
older_release 6.8.2 6.8.3 3 || bad "$case: 6.8.2 is older than 6.8.3"
! older_release 6.8.3 6.8.3 3 || bad "$case: 6.8.3 is not older than itself"
! older_release 6.8.4 6.8.3 3 || bad "$case: 6.8.4 is not older than 6.8.3"
older_release 6.8.10 6.9.0 3 || bad "$case: 6.8.10 is older than 6.9.0"
! older_release 6.8.10 6.8.9 3 || bad "$case: 6.8.10 is not older than 6.8.9 (numbers, not text)"
older_release 6.31.0 6.32.0 2 || bad "$case: KF 6.31 is older than 6.32"
! older_release 6.32.1 6.32.0 2 || bad "$case: a framework's bugfix release is the same release"
! older_release 6.32.0 6.32.1 2 || bad "$case: the same release, the other way"
older_release 6.11.2 6.11.3 0 || bad "$case: all parts: 6.11.2 is older than 6.11.3"

# --- the cached metadata ---
case=expire
w=$work/w
mkdir -p "$w/dnf-cache/kde-host/updates-6f655cd2c39e9765/repodata" "$w/dnf-cache/kde-host/updates-6f655cd2c39e9765/packages" \
	"$w/dnf-cache/kde-host/copr:copr.fedorainfracloud.org:group_kdesig:kde-beta-0123/attrs"
echo rpm > "$w/dnf-cache/kde-host/updates-6f655cd2c39e9765/packages/kwin-6.8.3-1.fc45.src.rpm"
touch "$w/dnf-cache/kde-host/metalink-not-a-folder"
out=$(expire_metadata "$w")
for repo in updates-6f655cd2c39e9765 copr:copr.fedorainfracloud.org:group_kdesig:kde-beta-0123; do
	[ -f "$w/dnf-cache/kde-host/$repo/attrs/expired" ] || bad "$case: $repo not expired"
done
[ -f "$w/dnf-cache/kde-host/updates-6f655cd2c39e9765/packages/kwin-6.8.3-1.fc45.src.rpm" ] || bad "$case: a package went"
[ -z "$out" ] || bad "$case: said $out"
expire_metadata "$work/nothing-yet" > /dev/null || bad "$case: no cache yet is no failure"
# a folder it cannot write in (as root, there is none)
rm -rf "$w/dnf-cache/kde-host/updates-6f655cd2c39e9765/attrs"
chmod a-w "$w/dnf-cache/kde-host/updates-6f655cd2c39e9765"
if [ "$(id -u)" != 0 ]; then
	out=$(expire_metadata "$w")
	[[ $out == *"warning: the cached metadata of updates-6f655cd2c39e9765 stays as it is"* ]] ||
		bad "$case: a folder it cannot write in: $out"
	[ -f "$w/dnf-cache/kde-host/copr:copr.fedorainfracloud.org:group_kdesig:kde-beta-0123/attrs/expired" ] ||
		bad "$case: the others not expired after one it could not"
fi
chmod u+w "$w/dnf-cache/kde-host/updates-6f655cd2c39e9765"

[ "$fail" = 0 ] && echo "kde-build: all cases passed"
exit "$fail"
