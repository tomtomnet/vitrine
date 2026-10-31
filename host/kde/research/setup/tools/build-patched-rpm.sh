#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs inside a fedora container: RPMs from a source RPM with a patch set of ours added after the spec's
# own patches (the spec must %autosetup -p1, as Fedora's KDE specs do). Generalizes build/kwin/build68.sh.
#   build-patched-rpm.sh SRPM PATCHDIR TAG OUTDIR
#     PATCHDIR  *.patch applied in name order;  TAG  Release N%{?dist} -> N.TAG%{?dist}
set -euo pipefail
srpm=$(realpath "$1") set=$(realpath "$2") tag=$3 out=$4
top=/tmp/rpmbuild-$tag
rm -rf "$top"
mkdir -p "$top"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}
(cd "$top/SOURCES" && rpm2cpio "$srpm" | cpio -idm --quiet)
specs=("$top"/SOURCES/*.spec)
[ ${#specs[@]} = 1 ] || { echo "not one spec in $srpm" >&2; exit 1; }
spec=$top/SPECS/$(basename "${specs[0]}")
mv "${specs[0]}" "$spec"
grep -Eq '^%autosetup\b.*-p1' "$spec" || { echo "$(basename "$spec") does not %autosetup -p1" >&2; exit 1; }
n=$(grep -o -E '^Patch[0-9]+' "$spec" | sed 's/Patch//' | sort -n | tail -1 || true)
n=$(( ${n:-0} < 1000 ? 1000 : n ))
patches=""
for p in "$set"/*.patch; do
	n=$((n + 1))
	cp "$p" "$top/SOURCES/"
	patches+="Patch$n: $(basename "$p")"$'\n'
done
python3 - "$spec" "$tag" "$patches" <<'PY'
import re, sys
spec, tag, patches = sys.argv[1:4]
s = open(spec).read()
s, k = re.subn(r'^(Release:\s*)(\d+)(%\{\?dist\})', lambda m: f'{m.group(1)}{m.group(2)}.{tag}{m.group(3)}', s, count=1, flags=re.M)
if k != 1:
    sys.exit("no 'Release: N%{?dist}' line")
if patches:
    last = list(re.finditer(r'^(Source\d*|Patch\d*):.*$', s, flags=re.M))[-1]
    s = s[: last.end() + 1] + f'# virtio-gpu-desktop patch set {tag}\n' + patches + s[last.end() + 1 :]
open(spec, 'w').write(s)
PY
echo "$(grep -m1 '^Name:' "$spec" | awk '{print $2}') $(grep -m1 '^Version:' "$spec" | awk '{print $2}') $(grep -m1 '^Release:' "$spec" | awk '{print $2}'): $(printf '%s' "$patches" | grep -c '^Patch' || true) patches"
# a log per stage in OUTDIR (builddep-TAG.log, build-TAG.log); VGD_VERBOSE=1: on stdout too
logged() {   # LOG CMD...
	local log=$1; shift
	if [ "${VGD_VERBOSE:-0}" = 1 ]; then
		"$@" 2>&1 | tee "$log"
		return "${PIPESTATUS[0]}"
	fi
	"$@" > "$log" 2>&1
}
logged "$out/builddep-$tag.log" dnf -y -q builddep "$spec" || { tail -20 "$out/builddep-$tag.log"; exit 1; }
# KDE Frameworks' specs build their API docs (%cmake_build_kf6) with kde-qdoc-common's files under /usr/share/doc,
# which a container image leaves out (tsflags=nodocs): builddep's copy has none, so it goes in again with them
if grep -q '%cmake_build_kf6' "$spec"; then
	qdoc=install
	rpm -q kde-qdoc-common > /dev/null 2>&1 && qdoc=reinstall
	logged "$out/qdoc-$tag.log" dnf -y -q "$qdoc" --setopt=tsflags= kde-qdoc-common || { tail -20 "$out/qdoc-$tag.log"; exit 1; }
fi
# what the set was built against (KWin uses Qt's and kdecoration's private API: a Qt update that passes a
# version lock needs a rebuild - compare this list with the host before installing)
rpm -qa --qf '%{NAME} %{EVR}\n' | LC_ALL=C sort > "$out/buildroot-$tag.txt"
logged "$out/build-$tag.log" rpmbuild --define "_topdir $top" --define "_smp_mflags -j${JOBS:-$(nproc)}" \
	--define "debug_package %{nil}" -bb "$spec" || { tail -30 "$out/build-$tag.log"; exit 1; }
cp "$top"/RPMS/*/*.rpm "$out/"
