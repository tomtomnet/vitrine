#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Why a host build failed (host/kde/build.sh's failed_why), from the build
# logs of rpm 6.1 as the research's recipe leaves them: the patch that does
# not apply in %prep named, with the package and version rpm was preparing;
# a failure past %prep, or in %prep before any patch, or no log at all: the
# recipe's status.
set -u
here=$(dirname "$(readlink -f "$0")")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL: $*"; fail=1; }
eval "$(sed -n '/^failed_why() {/,/^}/p' "$here/../kde/build.sh")"
declare -F failed_why > /dev/null || { echo "FAIL: no failed_why in build.sh"; exit 1; }
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

[ "$fail" = 0 ] && echo "kde-build-why: all cases passed"
exit "$fail"
