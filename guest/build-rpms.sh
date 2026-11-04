#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the guest tools' packages in a Fedora container (rootless podman),
# whatever the host's distribution:
#   tools  vitrine-guest-tools (noarch): the patched virtio-gpu driver as a
#          DKMS module, its options, the compositor's settings, the boot-time
#          fix-ups and the vitrine agent (guest/tools/)
#   mesa   Mesa 26.2.6 + the Xe native context, the iris upload fix, longer BO
#          caches for radeonsi/iris over virtio and a floor of big buffers
#          for iris's, RADV's virtio probe fix
#   kwin   KWin 6.8.2 + the research's guest set g24 (VM frame pacing, the nested
#          desktop): for Plasma 6.8.2 guests (guest/kwin/kwin.spec)
#   qtbase, ksvg, kirigami  Qt 6.11.2's qtbase, KSvg and Kirigami 6.31.0 with
#          the research's sets (q4, k2, f1): Fedora's specs, as the others
# A package is a folder of guest/ with its spec; components.conf gives
# their order (the app's Builds window shows them from there), and with a
# releases= line the Fedora releases a package is built for (every one
# without it): the KDE packages are Fedora 44's for now.  Two files of the
# folder are this script's too:
#   sources             the files the spec downloads, as Fedora's dist-git
#                       pins them ("SHA512 (FILE) = HASH"): they come from
#                       Fedora's lookaside cache by that hash (the very files
#                       Fedora built from; upstream moves and respins some),
#                       else from the spec's URLs; a copy in the download
#                       cache that is another file goes before the build, and
#                       a build counts only when each is that file
#   container-setup.sh  run in the build's container (as root) before
#                       rpm-build-inside.sh: what the build needs besides the
#                       spec's build requirements
#
#   guest/build-rpms.sh [--force] [--verbose] [PACKAGE...]  (default: all of
#                       them that are built for FEDORA_RELEASE)
#   guest/build-rpms.sh --print-inputs [PACKAGE...]
#   guest/build-rpms.sh --print-deps
#
# A package is built again only when its inputs changed (its directory here,
# and the script that builds it in the container), or with --force.  Each
# one goes to a directory of its own, replaced only once a build succeeded:
# a failed or stopped build keeps the last good one, and the one it
# replaced is kept as PACKAGE.previous: inputs that go back to it bring it
# back, without a build.  The compilers go through ccache, whose cache stays
# for the next build: an update compiles again only the files it changes,
# although rpmbuild unpacks all the sources again.  Mesa takes ~15-25 min
# from nothing, KWin ~10, tools ~1.  Stopping the script (Ctrl+C, SIGTERM)
# stops its container.
#   --verbose       the build's output here too, not only in its log
#   --print-inputs  print "PACKAGE HASH" for each package: the hash of its
#                   inputs, which its build records in PACKAGE/.inputs (and
#                   the lines it sums up in PACKAGE/.inputs.list)
#   --print-deps    tell what the build needs that is missing, then stop
#                   (status 1 if something is)
#
# Output: $RPMS_DIR [~/.local/share/vitrine/guest-tools/rpms/fcRELEASE]/PACKAGE/,
#   one build at a time into it.
# Downloads (dnf's cache, source tarballs) and the compiler cache (ccache/):
#   $CACHE_DIR [~/.cache/vitrine/guest-build/fcRELEASE], kept for the next run.
# Environment: FEDORA_RELEASE [44], FEDORA_IMAGE [registry.fedoraproject.org/
#   fedora:RELEASE], JOBS [nproc, at most 8], MEMORY [10g: the container's
#   memory cap; KWin's LTO link and Mesa's parallel compiles take several
#   GiB], DEBUGINFO [0; 1 keeps debuginfo packages], CCACHE [1; 0: no
#   compiler cache], CCACHE_MAXSIZE [8G].
#
# Lines starting with "vitrine-build: " are for vitrine, as host/build.sh's:
#   vitrine-build: building: PACKAGE (log: FILE)
#   vitrine-build: phase: WHAT     what the build in the container does now
#   vitrine-build: up to date: DIRECTORY
#   vitrine-build: restored: DIRECTORY   the build before, of the same inputs
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

# The packages: a folder here with one spec, in the order of components.conf
is_package() {
	[[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9_.+-]*$ ]] &&
		[ "$(find "$here/$1" -maxdepth 1 -name '*.spec' 2> /dev/null | wc -l)" = 1 ]
}
all_packages() {
	local p
	{
		sed -n 's/^[[:space:]]*\[\([^]]*\)\][[:space:]]*$/\1/p' "$here/components.conf" 2> /dev/null
		find "$here" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | LC_ALL=C sort
	} | awk '!seen[$0]++' | while read -r p; do
		! is_package "$p" || ! for_release "$p" || echo "$p"
	done
}
# The package @1 is built for Fedora $release: its section of components.conf
# has no releases= line, or one that names it (build-medium.sh has the same)
for_release() {
	[ -f "$here/components.conf" ] || return 0
	awk -v p="$1" -v r="$release" '
		/^[[:space:]]*\[/ { s = $0; gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", s); next }
		s == p && /^releases=/ {
			found = 1; sub(/^releases=/, ""); n = split($0, a, /[[:space:]]+/)
			for (i = 1; i <= n; i++) if (a[i] == r) ok = 1
		}
		END { exit found && !ok }' "$here/components.conf"
}

action=build
force=0
verbose=0
targets=()
for a in "$@"; do
	case "$a" in
	--force) force=1 ;;
	-v|--verbose) verbose=1 ;;
	--print-inputs) action=print-inputs ;;
	--print-deps) action=print-deps ;;
	-h|--help) usage; exit 0 ;;
	-*) echo "unknown argument $a (see --help)" >&2; exit 2 ;;
	*)
		is_package "$a" || { echo "no package $a: no folder $a with its spec in $here" >&2; exit 2; }
		for_release "$a" ||
			{ echo "$a is not built for Fedora $release (its releases= in $here/components.conf)" >&2; exit 2; }
		targets+=("$a")
		;;
	esac
done
[ ${#targets[@]} -gt 0 ] || mapfile -t targets < <(all_packages)

# What a package's build depends on: its directory and the script that
# builds it in the container, in the C locale's order (sort's own would
# change with the user's locale); a build records these lines, and their hash
inputs_lines() {
	(cd "$here" && find "$1" rpm-build-inside.sh -type f -print0 | LC_ALL=C sort -z |
		xargs -0 sha256sum)
}
inputs_hash() { inputs_lines "$1" | sha256sum | cut -d' ' -f1; }

# Run in the container before rpm-build-inside.sh, for a package with a sources
# file or a container-setup.sh (prepare DIST-GIT-NAME PACKAGE): the files the
# sources file pins and the cache lacks, from Fedora's lookaside cache by their
# hash.  KDE respun kirigami-6.31.0.tar.xz after Fedora had taken it (without
# the delegates' new textFormat): the spec's URL gives the other file.  A file
# the lookaside does not have is left to the spec's URL.
prepare='set -eu
name=$1 t=$2
if [ -f "/src/$t/sources" ]; then
	while IFS= read -r line || [ -n "$line" ]; do
		[[ $line =~ ^SHA512\ \((.+)\)\ =\ ([0-9a-f]{128}) ]] || continue
		f=${BASH_REMATCH[1]} h=${BASH_REMATCH[2]}
		[ ! -f "/sources/$f" ] || continue
		echo "vitrine-build: phase: downloading $f from Fedora"
		curl -fsSL --retry 2 -o "/sources/.$f.part" \
			"https://src.fedoraproject.org/repo/pkgs/rpms/$name/$f/sha512/$h/$f" &&
			mv "/sources/.$f.part" "/sources/$f" ||
			{ rm -f "/sources/.$f.part"; echo "not in the Fedora lookaside cache: $f"; }
	done < "/src/$t/sources"
fi
[ ! -f "/src/$t/container-setup.sh" ] || bash "/src/$t/container-setup.sh"
exec bash /src/rpm-build-inside.sh "$t"'

# The files of the package @1's sources file not in the download cache as it
# pins them, one per line (none without the file)
sources_wrong() {
	local line file sum
	[ -f "$here/$1/sources" ] || return 0
	while IFS= read -r line || [ -n "$line" ]; do
		[[ $line =~ ^SHA512\ \((.+)\)\ =\ ([0-9a-f]{128})[[:space:]]*$ ]] || continue
		file=${BASH_REMATCH[1]} sum=${BASH_REMATCH[2]}
		case "$file" in */* | . | ..) die "$1/sources names $file: a file name, not a path" ;; esac
		[ -f "$cache/sources/$file" ] &&
			[ "$(sha512sum < "$cache/sources/$file" | cut -d' ' -f1)" = "$sum" ] ||
			echo "$file"
	done < "$here/$1/sources"
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
ccache=()
if [ "${CCACHE:-1}" != 0 ]; then
	mkdir -p "$cache/ccache"
	ccache=(-v "$cache/ccache:/ccache" -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-8G}")
fi
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
	# the build before, of these inputs: back in its place, the other one kept
	if [ "$force" = 0 ] && [ "$(cat "$out/$t.previous/.inputs" 2> /dev/null)" = "$hash" ]; then
		rm -rf "$out/.$t.swap"
		[ ! -d "$out/$t" ] || mv "$out/$t" "$out/.$t.swap"
		mv "$out/$t.previous" "$out/$t"
		[ ! -d "$out/.$t.swap" ] || mv "$out/.$t.swap" "$out/$t.previous"
		say "restored: $out/$t"
		continue
	fi
	# a download that is not the pinned file (another release under its name,
	# one cut short): downloaded again
	stale=$(sources_wrong "$t") || exit 1
	for f in $stale; do
		rm -f "$cache/sources/$f"
	done
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
	# what the package's build needs in the container before rpm-build-inside.sh
	inside=(bash /src/rpm-build-inside.sh "$t")
	if [ -f "$here/$t/sources" ] || [ -f "$here/$t/container-setup.sh" ]; then
		spec=$(find "$here/$t" -maxdepth 1 -name '*.spec' | head -n 1)
		inside=(bash -c "$prepare" prepare "$(basename "$spec" .spec)" "$t")
	fi
	# label=disable: no SELinux relabelling of the source tree
	# the container gets a cgroup of its own, outside the caller's: capped here
	# in the background, for a stop to come at once (bash waits for a command
	# in the foreground before it runs a trap); without the lock's descriptor
	podman run --rm --name "$container" --security-opt label=disable --memory="${MEMORY:-10g}" \
		-v "$here:/src:ro" -v "$new:/out" -v "$cache/dnf:/var/cache/libdnf5" \
		-v "$cache/sources:/sources" ${ccache[@]+"${ccache[@]}"} \
		-e JOBS="${JOBS:-$(( $(nproc) < 8 ? $(nproc) : 8 ))}" -e DEBUGINFO="${DEBUGINFO:-0}" \
		"$image" "${inside[@]}" >> "$log" 2>&1 9>&- &
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
	# built from what its sources file pins, or not at all
	wrong=
	[ "$rc" != 0 ] || wrong=$(sources_wrong "$t" | tr '\n' ' ')
	if [ -n "$wrong" ]; then
		for f in $wrong; do
			rm -f "$cache/sources/$f"
		done
		rm -rf "$new"
		new=
		die "building $t: the download of ${wrong% } is not the file guest/$t/sources pins (removed: the next build downloads it again)"
	fi
	if [ "$rc" = 0 ]; then
		inputs_lines "$t" > "$new/.inputs.list"
		echo "$hash" > "$new/.inputs"
		# the one it replaces stays, the one before that goes
		rm -rf "$out/$t.previous"
		[ ! -d "$out/$t" ] || mv "$out/$t" "$out/$t.previous"
		mv "$new" "$out/$t"
		new=
		[ "$verbose" = 1 ] || tail -n 2 "$log"
		say "built: $out/$t"
	else
		rm -rf "$new"
		new=
		if [ "$verbose" = 0 ]; then
			echo "the end of its log ($log):" >&2
			tail -n 25 "$log" >&2
		fi
		die "building $t failed (status $rc)"
	fi
done
echo
find "$out" -mindepth 2 -name '*.rpm' | sort | sed "s|^$out/||"
