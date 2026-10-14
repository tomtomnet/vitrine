// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * Stand-ins for guest/build-rpms.sh and guest/build-medium.sh, shared by
 * the tests of the guest tools builder and of the guest tools dialog
 */

/* What build-rpms.sh hashes, for the stand-ins */
inline constexpr char kInputsHash[] = R"sh(
inputs_hash() {
	(cd "$here" && find "$1" rpm-build-inside.sh -type f -print0 | LC_ALL=C sort -z |
		xargs -0 sha256sum) | sha256sum | cut -d' ' -f1
}
)sh";

/*
 * A stand-in for build-rpms.sh: the lines of a package's build as the real
 * one passes them on (its own, the container's, rpmbuild's, ninja's...),
 * and the package's inputs recorded; GATE=PACKAGE:FILE holds PACKAGE's
 * until FILE is there.  (No line of these scripts starts with a #, nor has
 * a \": moc would take them for a directive or a string's end, and see no
 * class.)
 */
inline constexpr char kRpmsStandIn[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
@HASH@
echo "$*" >> "$here/rpms-args"
env | grep -E '^(RPMS_DIR|CACHE_DIR|OUT|FEDORA_RELEASE)=' | sort > "$here/rpms-env"
pkg=${@: -1}
echo "vitrine-build: building: $pkg (log: $RPMS_DIR/$pkg.log)"
echo "vitrine-build: phase: installing rpmbuild"
echo "[ 1/82] emacs-filesystem-1:30.2-2.fc44. 100% | 297.9 KiB/s |   8.0 KiB |  00m00s"
echo "[82/82] rpmdevtools-0:9.6-14.fc44.noarc 100% |   1.6 MiB/s |  94.4 KiB |  00m00s"
echo "vitrine-build: phase: downloading the sources"
echo "[3/9] not a count of this phase"
echo "vitrine-build: phase: building the packages"
echo "[5/9] nor of this one"
echo "Executing(%prep): /bin/sh -e /var/tmp/rpm-tmp.a"
echo "Executing(%build): /bin/sh -e /var/tmp/rpm-tmp.b"
echo "[1/5808] /usr/bin/python3 ../src/nir_intrinsics_h.py"
echo "[5808/5808] g++ -o src/libvulkan_nouveau.so"
echo "[ 13%] Building CXX object a.o"
echo "[  9%] Building CXX object b.o"
echo "Executing(%install): /bin/sh -e /var/tmp/rpm-tmp.c"
echo "Processing files: $pkg-1.0-1.fc44.x86_64"
echo "Processing files: $pkg-devel-1.0-1.fc44.x86_64"
echo "vitrine-build: warning: a warning of $pkg"
if [ -n "${GATE:-}" ] && [ "$pkg" = "${GATE%%:*}" ]; then
	while [ ! -e "${GATE#*:}" ]; do sleep 0.05; done
fi
mkdir -p "$RPMS_DIR/$pkg"
inputs_hash "$pkg" > "$RPMS_DIR/$pkg/.inputs"
printf 'vitrine-build: built: %s' "$RPMS_DIR/$pkg"
)sh";

/* A stand-in for build-medium.sh: a medium, with the manifest the real one writes */
inline constexpr char kMediumStandIn[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
echo "$*" >> "$here/medium-args"
env | grep -E '^(RPMS_DIR|CACHE_DIR|OUT|FEDORA_RELEASE)=' | sort > "$here/medium-env"
echo "vitrine-build: phase: making the repository"
[ "${FAKE_MEDIUM_FAIL:-0}" = 1 ] && { echo "vitrine-build: error: no room left" >&2; exit 1; }
inputs=
for t in tools mesa kwin; do
	[ -s "$RPMS_DIR/$t/.inputs" ] && inputs+=$(printf '"%s": "%s", ' "$t" "$(cat "$RPMS_DIR/$t/.inputs")")
done
inputs+=$(printf '"medium": "%s"' "$(sha256sum < "$here/build-medium.sh" | cut -d' ' -f1)")
mkdir -p "$(dirname "$OUT")"
echo image > "$OUT"
cat > "${OUT%.img}.json" <<EOF
{"mediumId": "m$RANDOM", "fedora": "44", "tools": "0.1.0-14.fc44", "mesa": "26.2.3-1.xe.fc44",
 "kwin": "6.7.5-1.22.fc44", "built": "2026-10-05T10:00:00Z", "inputs": {$inputs}, "packages": []}
EOF
echo "vitrine-build: built: $OUT"
)sh";
