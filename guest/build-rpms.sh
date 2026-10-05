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
#   guest/build-rpms.sh [--force] [--verbose] [tools] [mesa] [kwin]  (default: all three)
#   guest/build-rpms.sh --print-inputs [tools] [mesa] [kwin]
#   guest/build-rpms.sh --print-deps
#
# A package is built again only when its inputs changed (its directory here,
# and the scripts that build it), or with --force.  Each one goes to a
# directory of its own, replaced only once a build succeeded: a failed or
# stopped build keeps the last good one.  Mesa takes ~15-25 min, KWin ~10,
# tools ~1.  Stopping the script (Ctrl+C, SIGTERM) stops its container.
#   --verbose       the build's output here too, not only in its log
#   --print-inputs  print "PACKAGE HASH" for each package: the hash of its
#                   inputs, which its build records in PACKAGE/.inputs
#   --print-deps    tell what the build needs that is missing, then stop
#                   (status 1 if something is)
#
# Output: $RPMS_DIR [~/.local/share/vitrine/guest-tools/rpms/fcRELEASE]/PACKAGE/,
#   one build at a time into it.
# Downloads (dnf's cache, source tarballs): $CACHE_DIR
#   [~/.cache/vitrine/guest-build/fcRELEASE], kept for the next run.
# Environment: FEDORA_RELEASE [44], FEDORA_IMAGE [registry.fedoraproject.org/
#   fedora:RELEASE], JOBS [nproc, at most 8], MEMORY [10g: the container's
#   memory cap; KWin's LTO link and Mesa's parallel compiles take several
#   GiB], DEBUGINFO [0; 1 keeps debuginfo packages].
#
# Lines starting with "vitrine-build: " are for vitrine, as host/build.sh's:
#   vitrine-build: building: PACKAGE (log: FILE)
#   vitrine-build: phase: WHAT     what the build in the container does now
#   vitrine-build: up to date: DIRECTORY
#   vitrine-build: built: DIRECTORY
#   vitrine-build: missing: COMMAND
#   vitrine-build: install: THE COMMAND INSTALLING WHAT IS MISSING
#   vitrine-build: error: MESSAGE
set -eu
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
release=${FEDORA_RELEASE:-44}
image=${FEDORA_IMAGE:-registry.fedoraproject.org/fedora:$release}
data=${XDG_DATA_HOME:-$HOME/.local/share}/vitrine/guest-tools
out=${RPMS_DIR:-$data/rpms/fc$release}
cache=${CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/vitrine/guest-build/fc$release}

say() { echo "vitrine-build: $*"; }
die() { say "error: $*" >&2; exit 1; }
usage() { sed -n '3,/^set /{/^#/p}' "$here/build-rpms.sh" | sed 's/^# \{0,1\}//'; }

action=build
force=0
verbose=0
targets=()
for a in "$@"; do
	case "$a" in
	tools|mesa|kwin) targets+=("$a") ;;
	--force) force=1 ;;
	-v|--verbose) verbose=1 ;;
	--print-inputs) action=print-inputs ;;
	--print-deps) action=print-deps ;;
	-h|--help) usage; exit 0 ;;
	*) echo "unknown argument $a (see --help)" >&2; exit 2 ;;
	esac
done
[ ${#targets[@]} -gt 0 ] || targets=(tools mesa kwin)

# What a package's build depends on: its directory and the build scripts, in
# the C locale's order (sort's own would change with the user's locale)
inputs_hash() {
	(cd "$here" && find "$1" rpm-build-inside.sh -type f -print0 | LC_ALL=C sort -z |
		xargs -0 sha256sum) | sha256sum | cut -d' ' -f1
}

if [ "$action" = print-inputs ]; then
	for t in "${targets[@]}"; do
		echo "$t $(inputs_hash "$t")"
	done
	exit 0
fi

# podman runs the build; flock keeps it to one at a time; Fedora's packages
missing=()
packages=()
command -v podman > /dev/null || { missing+=(podman); packages+=(podman); }
command -v flock > /dev/null || { missing+=(flock); packages+=(util-linux-core); }
for m in ${missing[@]+"${missing[@]}"}; do
	say "missing: $m"
done
if [ ${#missing[@]} -gt 0 ]; then
	say "install: sudo dnf install ${packages[*]}"
	[ "$action" = print-deps ] && exit 1
	die "${missing[*]} missing: podman builds the packages in a Fedora container"
fi
[ "$action" = print-deps ] && exit 0

mkdir -p "$out" "$cache/dnf" "$cache/sources"
# one build at a time into $out: two would replace each other's packages
exec 9> "$out/.lock"
flock -n 9 || die "another build of the guest tools is running into $out"
# containers named after $out: a build into another folder is not this one's
tag=$(printf '%s' "$out" | sha256sum | cut -c1-8)

# A stop (Ctrl+C, vitrine's Stop, vitrine gone): podman passes a SIGTERM to
# the build, whose bash, the container's init, ignores it; the container
# goes here, and with it the build's partial output.  Another signal does
# not cut that short.
pid=
container=
new=
stop() {
	trap '' INT TERM HUP PIPE
	if [ -n "$pid" ]; then
		podman rm -f -t 0 "$container" > /dev/null 2>&1 || true
		for _ in $(seq 30); do
			kill -0 "$pid" 2> /dev/null || break
			sleep 0.1
		done
		# one podman was still making as the first went
		podman rm -f -t 0 "$container" > /dev/null 2>&1 || true
		kill -KILL "$pid" 2> /dev/null || true
	fi
	[ -z "$new" ] || rm -rf "$new"
	say "error: stopped" >&2
	exit "$1"
}
trap 'stop 130' INT
trap 'stop 143' TERM
trap 'stop 129' HUP
trap 'stop 141' PIPE

for t in "${targets[@]}"; do
	hash=$(inputs_hash "$t")
	if [ "$force" = 0 ] && [ "$(cat "$out/$t/.inputs" 2> /dev/null)" = "$hash" ]; then
		say "up to date: $out/$t"
		continue
	fi
	log=$out/$t.log
	new=$out/.$t.new
	rm -rf "$new"
	mkdir -p "$new"
	say "building: $t (log: $log)"
	container=vitrine-rpm-$t-fc$release-$tag
	# one left over from a run that was killed: no other build's, by the lock
	if podman container exists "$container" 2> /dev/null; then
		podman rm -f -t 0 "$container" > /dev/null
	fi
	# emptied first: tail must not show the last build's
	: > "$log"
	# label=disable: no SELinux relabelling of the source tree
	# the container gets a cgroup of its own, outside the caller's: capped here
	# in the background, for a stop to come at once (bash waits for a command
	# in the foreground before it runs a trap); without the lock's descriptor
	podman run --rm --name "$container" --security-opt label=disable --memory="${MEMORY:-10g}" \
		-v "$here:/src:ro" -v "$new:/out" -v "$cache/dnf:/var/cache/libdnf5" \
		-v "$cache/sources:/sources" \
		-e JOBS="${JOBS:-$(( $(nproc) < 8 ? $(nproc) : 8 ))}" -e DEBUGINFO="${DEBUGINFO:-0}" \
		"$image" bash /src/rpm-build-inside.sh "$t" >> "$log" 2>&1 9>&- &
	pid=$!
	follow=
	if [ "$verbose" = 1 ]; then
		tail -n +1 -f --pid="$pid" "$log" 9>&- &
		follow=$!
	fi
	rc=0
	wait "$pid" || rc=$?
	pid=
	# the log to its end
	[ -z "$follow" ] || wait "$follow" || true
	if [ "$rc" = 0 ]; then
		echo "$hash" > "$new/.inputs"
		rm -rf "$out/$t.old"
		[ -d "$out/$t" ] && mv "$out/$t" "$out/$t.old"
		mv "$new" "$out/$t"
		new=
		rm -rf "$out/$t.old"
		[ "$verbose" = 1 ] || tail -n 2 "$log"
		say "built: $out/$t"
	else
		rm -rf "$new"
		new=
		if [ "$verbose" = 0 ]; then
			echo "the end of its log ($log):" >&2
			tail -n 25 "$log" >&2
		fi
		die "building $t failed (status $rc): see $log"
	fi
done
echo
find "$out" -mindepth 2 -name '*.rpm' | sort | sed "s|^$out/||"
