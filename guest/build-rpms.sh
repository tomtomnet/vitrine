#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the guest tools' packages in a Fedora container (rootless podman),
# whatever the host's distribution:
#   tools  vitrine-guest-tools (noarch): the patched virtio-gpu driver as a
#          DKMS module, its options, the compositor's settings, the boot-time
#          fix-ups and the vitrine agent (guest/tools/)
#   mesa   Mesa 26.2.3 + the Xe native context, the iris upload fix, longer BO
#          caches for radeonsi/iris over virtio, RADV's virtio probe fix
#   kwin   KWin 6.7.5 + the VM frame pacing and zero-copy patches; for Plasma
#          6.7.5 guests only (guest/kwin/kwin.spec)
#
#   guest/build-rpms.sh [--force] [tools] [mesa] [kwin]     (default: all three)
#
# A package is built again only when its inputs changed (its directory here,
# and the scripts that build it), or with --force.  Each one goes to a
# directory of its own, replaced only once a build succeeded: a failed build
# keeps the last good one.  Mesa takes ~15-25 min, KWin ~10, tools ~1.
#
# Output: $RPMS_DIR [~/.local/share/vitrine/guest-tools/rpms/fcRELEASE]/PACKAGE/
# Downloads (dnf's cache, source tarballs): $CACHE_DIR
#   [~/.cache/vitrine/guest-build/fcRELEASE], kept for the next run.
# Environment: FEDORA_RELEASE [44], FEDORA_IMAGE [registry.fedoraproject.org/
#   fedora:RELEASE], JOBS [nproc, at most 8], MEMORY [10g: the container's
#   memory cap; KWin's LTO link and Mesa's parallel compiles take several
#   GiB], DEBUGINFO [0; 1 keeps debuginfo packages].
set -eu
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
release=${FEDORA_RELEASE:-44}
image=${FEDORA_IMAGE:-registry.fedoraproject.org/fedora:$release}
data=${XDG_DATA_HOME:-$HOME/.local/share}/vitrine/guest-tools
out=${RPMS_DIR:-$data/rpms/fc$release}
cache=${CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/vitrine/guest-build/fc$release}
force=0
targets=()
for a in "$@"; do
	case "$a" in
	tools|mesa|kwin) targets+=("$a") ;;
	--force) force=1 ;;
	-h|--help) sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown argument $a (tools, mesa, kwin, --force)" >&2; exit 2 ;;
	esac
done
[ ${#targets[@]} -gt 0 ] || targets=(tools mesa kwin)
command -v podman > /dev/null || { echo "needs podman (Fedora: dnf install podman)" >&2; exit 1; }
mkdir -p "$out" "$cache/dnf" "$cache/sources"

# What a package's build depends on: its directory and the build scripts
inputs_hash() {
	(cd "$here" && find "$1" rpm-build-inside.sh -type f -print0 | sort -z |
		xargs -0 sha256sum) | sha256sum | cut -d' ' -f1
}

for t in "${targets[@]}"; do
	hash=$(inputs_hash "$t")
	if [ "$force" = 0 ] && [ "$(cat "$out/$t/.inputs" 2> /dev/null)" = "$hash" ]; then
		echo "== $t: up to date ($out/$t)"
		continue
	fi
	new=$out/.$t.new
	rm -rf "$new"
	mkdir -p "$new"
	echo "== $t (log: $out/$t.log)"
	# a named container (podman pause holds a build); one left over from an
	# interrupted run is removed first
	name=vitrine-rpm-$t-fc$release
	podman container exists "$name" 2> /dev/null && podman rm -f "$name" > /dev/null
	# label=disable: no SELinux relabelling of the source tree
	# the container gets a cgroup of its own, outside the caller's: capped here
	if podman run --rm --name "$name" --security-opt label=disable --memory="${MEMORY:-10g}" \
		-v "$here:/src:ro" -v "$new:/out" -v "$cache/dnf:/var/cache/libdnf5" \
		-v "$cache/sources:/sources" \
		-e JOBS="${JOBS:-$(( $(nproc) < 8 ? $(nproc) : 8 ))}" -e DEBUGINFO="${DEBUGINFO:-0}" \
		"$image" bash /src/rpm-build-inside.sh "$t" > "$out/$t.log" 2>&1; then
		echo "$hash" > "$new/.inputs"
		rm -rf "$out/$t.old"
		[ -d "$out/$t" ] && mv "$out/$t" "$out/$t.old"
		mv "$new" "$out/$t"
		rm -rf "$out/$t.old"
		tail -n 2 "$out/$t.log"
	else
		rm -rf "$new"
		echo "$t failed; the end of its log ($out/$t.log):" >&2
		tail -n 25 "$out/$t.log" >&2
		exit 1
	fi
done
echo
find "$out" -mindepth 2 -name '*.rpm' | sort | sed "s|^$out/||"
