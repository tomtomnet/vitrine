#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build vitrine's Mesa for Flatpak apps: freedesktop-sdk's GL extension of
# org.freedesktop.Platform (GL.default, the one Flathub ships, every video
# codec: flatpak-images/mesa-extra.bst) for each runtime branch of
# flatpak/branches.conf, from freedesktop-sdk's recipe at the commit pinned
# there, with the patches it names (the Xe native context, vitrine's guest
# Mesa's) and the Intel and AMD native contexts on.  BuildStream builds it in
# freedesktop-sdk's CI image (rootless podman, flatpak/build-inside.sh); all
# but Mesa comes built from freedesktop-sdk's cache, so a build compiles Mesa
# alone, 5 to 10 min a branch.  Each branch goes into one OSTree repository
# as GL.default//BRANCH and //BRANCH-extra (Flathub installs both, and a
# sandbox takes -extra), stamped with the build's time: flatpak replaces a
# commit only with a later one.  The medium carries it; the guest tools
# install it over Flathub's.
#
#   guest/build-flatpak.sh [--force] [--verbose]
#   guest/build-flatpak.sh --print-inputs
#   guest/build-flatpak.sh --print-deps
#
# Built again only when its inputs changed (this script, flatpak/, the
# patches branches.conf names), or with --force.  A failed or stopped build
# keeps the last one.  Stopping the script (Ctrl+C, SIGTERM) stops its
# container.
#   --verbose       the build's output here too, not only in its log
#   --print-inputs  print "flatpak HASH": the hash of its inputs, which a
#                   build records in .inputs (and the lines it sums up in
#                   .inputs.list)
#   --print-deps    tell what the build needs that is missing, then stop
#                   (status 1 if something is)
#
# Output: $FLATPAK_DIR [~/.local/share/vitrine/guest-tools/flatpak]: repo/
#   (the OSTree repository), branches ("BRANCH MESA COMMIT" lines), .inputs,
#   .inputs.list; replaced once complete; its log beside it (flatpak.log).
# Cache: $CACHE_DIR [~/.cache/vitrine/flatpak-build]: BuildStream's (what it
#   downloads of freedesktop-sdk's builds, ~10 GB) and freedesktop-sdk's
#   checkouts, kept for the next build.
# Environment: JOBS [nproc, at most 8], MEMORY [10g: the container's memory
#   cap].
#
# Lines starting with "vitrine-build: " are for vitrine (see build-rpms.sh):
# building, phase, up to date, built, missing, install, error.
set -eu
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}/vitrine/guest-tools
out=${FLATPAK_DIR:-$data/flatpak}
cache=${CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/vitrine/flatpak-build}
conf=$here/flatpak/branches.conf
registry=registry.gitlab.com/freedesktop-sdk/infrastructure/freedesktop-sdk-docker-images

say() { echo "vitrine-build: $*"; }
die() { say "error: $*" >&2; exit 1; }
usage() { sed -n '3,/^set /{/^#/p}' "$here/build-flatpak.sh" | sed 's/^# \{0,1\}//'; }

force=0
verbose=0
action=build
for a in "$@"; do
	case $a in
	-f|--force) force=1 ;;
	-v|--verbose) verbose=1 ;;
	--print-inputs) action=print-inputs ;;
	--print-deps) action=print-deps ;;
	-h|--help) usage; exit 0 ;;
	*) echo "unknown argument $a (see --help)" >&2; exit 2 ;;
	esac
done

# branches.conf: its [sections], and a NAME=value of one (the last, as
# vitrine reads it)
branches() { sed -n 's/^[[:space:]]*\[\([^]]*\)\][[:space:]]*$/\1/p' "$conf"; }
value() {
	awk -v b="$1" -v n="$2" '
		/^[[:space:]]*\[/ { s = $0; gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", s); next }
		s == b && match($0, "^[[:space:]]*" n "[[:space:]]*=") {
			v = substr($0, RLENGTH + 1); gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
		}
		END { print v }' "$conf"
}
[ -f "$conf" ] || die "no $conf"
[ -n "$(branches)" ] || die "$conf has no [branch]"
for b in $(branches); do
	[[ $b =~ ^[0-9]+\.[0-9]+$ ]] || die "$conf: [$b] is no runtime branch, as 25.08"
	[[ $(value "$b" commit) =~ ^[0-9a-f]{40}$ ]] || die "$conf: [$b] has no commit= of 40 hex digits"
	[[ $(value "$b" image) =~ ^bst2:[0-9a-f]{40}$ ]] || die "$conf: [$b] has no image=bst2:ID"
	[[ $(value "$b" mesa) =~ ^[0-9]+(\.[0-9]+)+$ ]] || die "$conf: [$b] has no mesa= version"
	[ -n "$(value "$b" patches)" ] || die "$conf: [$b] has no patches="
	for p in $(value "$b" patches); do
		[[ /$p/ != */../* ]] && [ -f "$here/$p" ] || die "$conf: [$b]'s patch $p is no file of $here"
	done
done

# What a build depends on: this script, flatpak/ and the patches it names, in
# the C locale's order; a build records these lines, and their hash
inputs_lines() {
	local b p
	{
		printf '%s\0' build-flatpak.sh
		(cd "$here" && find flatpak -type f -print0)
		for b in $(branches); do
			for p in $(value "$b" patches); do
				printf '%s\0' "$p"
			done
		done
	} | LC_ALL=C sort -zu | (cd "$here" && xargs -0 sha256sum)
}
inputs_hash() { inputs_lines | sha256sum | cut -d' ' -f1; }

if [ "$action" = print-inputs ]; then
	echo "flatpak $(inputs_hash)"
	exit 0
fi

# podman runs the build; everything else is in freedesktop-sdk's image
missing=()
packages=()
for c in podman:podman flock:util-linux-core; do
	command -v "${c%%:*}" > /dev/null || { missing+=("${c%%:*}"); packages+=("${c#*:}"); }
done
for m in ${missing[@]+"${missing[@]}"}; do
	say "missing: $m"
done
if [ ${#missing[@]} -gt 0 ]; then
	say "install: sudo dnf install ${packages[*]}"
	[ "$action" = print-deps ] && exit 1
	die "${missing[*]} missing: podman runs the build in freedesktop-sdk's container"
fi
[ "$action" = print-deps ] && exit 0

hash=$(inputs_hash)
if [ "$force" = 0 ] && [ "$(cat "$out/.inputs" 2> /dev/null)" = "$hash" ]; then
	say "up to date: $out"
	exit 0
fi
mkdir -p "$(dirname "$out")" "$cache/buildstream"
# one build at a time into $out, and none while build-medium.sh copies it
# (its lock is shared)
exec 9> "$out.lock"
flock -n 9 || die "another build of the Mesa for Flatpak apps is running into $out"
# containers named after $out: a build into another folder is not this one's
tag=$(printf '%s' "$out" | sha256sum | cut -c1-8)
jobs=${JOBS:-$(( $(nproc) < 8 ? $(nproc) : 8 ))}
log=$out.log
new=$out.new

# A stop (Ctrl+C, vitrine's Stop, vitrine gone): the container goes, and with
# it the build's partial output (as build-rpms.sh's)
pid=
container=
stop() {
	trap '' INT TERM HUP PIPE
	if [ -n "$pid" ]; then
		podman rm -f -t 0 "$container" > /dev/null 2>&1 || true
		for _ in $(seq 30); do
			kill -0 "$pid" 2> /dev/null || break
			sleep 0.1
		done
		podman rm -f -t 0 "$container" > /dev/null 2>&1 || true
		kill -KILL "$pid" 2> /dev/null || true
	fi
	rm -rf "$new"
	say "error: stopped" >&2
	exit "$1"
}
trap 'stop 130' INT
trap 'stop 143' TERM
trap 'stop 129' HUP
trap 'stop 141' PIPE

# run NAME IMAGE COMMAND...: in a container of freedesktop-sdk's image, the
# output into the log (and here with --verbose); BuildStream sandboxes its
# builds with bubblewrap, so the container is privileged (rootless: within
# the user's own namespaces), and capped
run() {
	local name=$1 image=$2 rc=0 follow='' start
	shift 2
	# the log from here: the branches before are in it
	start=$(( $(stat -c %s "$log") + 1 ))
	container=vitrine-flatpak-$name-$tag
	if podman container exists "$container" 2> /dev/null; then
		podman rm -f -t 0 "$container" > /dev/null
	fi
	podman run --rm --name "$container" --privileged --security-opt label=disable \
		--memory="${MEMORY:-10g}" -v "$here:/guest:ro" -v "$cache/buildstream:/root/.cache/buildstream" \
		-v "$new:/out" ${src:+-v "$src:/src"} -w / "$image" "$@" >> "$log" 2>&1 9>&- &
	pid=$!
	if [ "$verbose" = 1 ]; then
		tail -c +"$start" -f --pid="$pid" "$log" 9>&- &
		follow=$!
	fi
	wait "$pid" || rc=$?
	pid=
	[ -z "$follow" ] || wait "$follow" || true
	return "$rc"
}

rm -rf "$new"
mkdir -p "$new"
: > "$log"
say "building: flatpak (log: $log)"
image=
for b in $(branches); do
	commit=$(value "$b" commit)
	image=$registry/$(value "$b" image)
	mesa=$(value "$b" mesa)
	src=$cache/fdsdk-$b
	mkdir -p "$src"
	say "phase: downloading freedesktop-sdk's image for $b"
	podman image exists "$image" || podman pull -q "$image" > /dev/null 9>&- ||
		die "podman pull $image failed"
	read -r -a patches <<< "$(value "$b" patches)"
	rc=0
	run "$b" "$image" bash /guest/flatpak/build-inside.sh "$b" "$commit" "${image##*:}" "$mesa" \
		"$jobs" "${patches[@]}" || rc=$?
	if [ "$rc" != 0 ]; then
		rm -rf "$new"
		if [ "$verbose" = 0 ]; then
			echo "the end of its log ($log):" >&2
			tail -n 25 "$log" >&2
		fi
		die "building the Mesa for Flatpak runtime $b failed (status $rc)"
	fi
	echo "$b $mesa $commit" >> "$new/branches"
done
src=
say "phase: making the repository"
run summary "$image" sh -c 'flatpak build-update-repo /out/repo > /dev/null && flatpak repo --branches /out/repo' ||
	{ rm -rf "$new"; die "flatpak build-update-repo failed: see $log"; }
inputs_lines > "$new/.inputs.list"
echo "$hash" > "$new/.inputs"
rm -rf "$out.previous"
[ ! -d "$out" ] || mv "$out" "$out.previous"
mv "$new" "$out"
rm -rf "$out.previous"
[ "$verbose" = 1 ] || tail -n 4 "$log"
say "built: $out"
