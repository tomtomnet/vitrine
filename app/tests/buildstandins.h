// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * Stand-ins for host/build.sh, guest/build-rpms.sh and guest/build-medium.sh,
 * shared by the tests of the build manager and of the GTK app's Builds window
 */

/*
 * A stand-in for host/build.sh: its lines, per component, as the real one
 * says them, and a QEMU, a virglrenderer, a rutabaga_gfx and a passt where
 * it says (passt alone before the libraries, as the real one builds it).
 * HOST_HOLD holds virglrenderer's build until that file is there,
 * QEMU_HOLD QEMU's compile half-way; HOST_FAIL=qemu fails it.  (No line of
 * these scripts starts with a #, nor has a \": moc would take them for a
 * directive or a string's end.)
 */
inline constexpr char kBuildSh[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
stack=
target=qemu
while [ $# -gt 0 ]; do
	case "$1" in
	--stack) stack=$2; shift ;;
	--work|-j) shift ;;
	--clean) echo clean >> "$here/calls" ;;
	--*) ;;
	*) target=$1 ;;
	esac
	shift
done
echo "$target" >> "$here/calls"
passt() {
	echo "vitrine-build: component: passt"
	echo "[1/1] cc passt.c"
	p=$stack/passt/cccccccccccccccc
	mkdir -p "$p/share/vitrine" "$p/bin"
	echo KEY=k > "$p/share/vitrine/passt.conf"
	echo "vitrine-build: done: passt built $p"
}
if [ "$target" = passt ]; then
	passt
	echo "vitrine-build: built: $p"
	exit 0
fi
echo "vitrine-build: step 1/4: checking the build dependencies"
echo "vitrine-build: component: virglrenderer"
echo "vitrine-build: step 2/4: building virglrenderer"
[ -z "${HOST_HOLD:-}" ] || while [ ! -e "$HOST_HOLD" ]; do sleep 0.05; done
echo "[1/2] cc a.c"
echo "[2/2] cc b.c"
lib=$stack/virglrenderer/aaaaaaaaaaaaaaaa
mkdir -p "$lib/share/vitrine"
echo KEY=k > "$lib/share/vitrine/virglrenderer.conf"
echo "vitrine-build: done: virglrenderer built $lib"
if [ "$target" = virglrenderer ]; then
	echo "vitrine-build: built: $lib"
	exit 0
fi
echo "vitrine-build: component: rutabaga_gfx"
echo "vitrine-build: step 3/4: building rutabaga_gfx"
echo "[1/1] rustc lib.rs"
ffi=$stack/rutabaga_gfx/bbbbbbbbbbbbbbbb
mkdir -p "$ffi/share/vitrine"
echo KEY=k > "$ffi/share/vitrine/rutabaga_gfx.conf"
echo "vitrine-build: done: rutabaga_gfx built $ffi"
if [ "$target" = rutabaga_gfx ]; then
	echo "vitrine-build: built: $ffi"
	exit 0
fi
passt
echo "vitrine-build: component: qemu"
echo "vitrine-build: step 4/4: compiling QEMU"
if [ "${HOST_FAIL:-}" = qemu ]; then
	echo "vitrine-build: error: compiling QEMU failed (status 2)"
	exit 2
fi
echo "[1/4] cc q.c"
echo "[2/4] cc r.c"
[ -z "${QEMU_HOLD:-}" ] || while [ ! -e "$QEMU_HOLD" ]; do sleep 0.05; done
echo "[4/4] link qemu-system-x86_64"
prefix=$stack/0123456789abcdef
mkdir -p "$prefix/share/vitrine" "$prefix/bin"
echo STAMP=s > "$prefix/share/vitrine/stack.conf"
ln -sfn 0123456789abcdef "$stack/current"
echo "vitrine-build: done: qemu built $prefix"
echo "vitrine-build: built: $prefix"
)sh";

/* A stand-in for guest/build-rpms.sh: GUEST_FAIL=PACKAGE fails it,
   GUEST_RESTORE=PACKAGE brings its build before back */
inline constexpr char kRpmsSh[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
pkg=${@: -1}
echo "$pkg" >> "$here/rpms-calls"
echo "vitrine-build: building: $pkg (log: $RPMS_DIR/$pkg.log)"
echo "vitrine-build: phase: installing rpmbuild"
echo "vitrine-build: phase: building the packages"
echo "Executing(%build): /bin/sh -e /var/tmp/rpm-tmp.b"
echo "[1/10] cc a.c"
echo "[10/10] cc z.c"
echo "Executing(%install): /bin/sh -e /var/tmp/rpm-tmp.i"
if [ "${GUEST_FAIL:-}" = "$pkg" ]; then
	echo "vitrine-build: error: building $pkg failed (status 1)"
	exit 1
fi
if [ "${GUEST_RESTORE:-}" = "$pkg" ]; then
	echo "vitrine-build: restored: $RPMS_DIR/$pkg"
	exit 0
fi
mkdir -p "$RPMS_DIR/$pkg"
echo x > "$RPMS_DIR/$pkg/.inputs"
echo "vitrine-build: built: $RPMS_DIR/$pkg"
)sh";

/* A stand-in for guest/build-medium.sh: a medium, with its manifest */
inline constexpr char kMediumSh[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
echo medium >> "$here/medium-calls"
echo "vitrine-build: phase: making the repository"
mkdir -p "$(dirname "$OUT")"
echo image > "$OUT"
printf '{"mediumId": "m1", "fedora": "44", "tools": "0.1.0-15.fc44", "built": "2026-10-14T10:00:00Z", "inputs": {}, "packages": []}\n' > "${OUT%.img}.json"
echo "vitrine-build: built: $OUT"
)sh";
