#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the host's KWin set: this computer's own KWin with the research's
# host patches (frame pacing for VMs, the pointer grab inside Vitrine's
# Desktop view...).  It runs the research's recipe as it is: research/ is a
# pinned copy of virtio-gpu-desktop's setup/tools/kde-rpms.sh, of what it
# sources and of the patch sets it reads (README.md).  kde-rpms.sh takes
# the source RPM of the newest KWin of the host's Plasma repositories
# (Fedora's, or the KDE SIG's beta COPR when the host's Plasma comes from
# it), the set its version maps to (setup/versions.sh: kde_set_for), and
# builds the RPMs in a Fedora container (rootless podman).  This only
# builds: install.sh installs them, as root.
#
#   host/kwin/build.sh [--plasma beta|testing|fedora] [--out DIR] [--work DIR] [-j N]
#   host/kwin/build.sh --print-set VERSION | --print-recipe VERSION FEDORA |
#                      --print-plasma | --print-inputs | --print-key | --print-deps
#
#   --plasma WHERE where the host's Plasma comes from: beta (the COPR that
#                  versions.sh's GUEST_PLASMA_COPR names), testing (Fedora's
#                  updates-testing) or fedora [from the repository
#                  plasma-workspace was installed from]
#   --out DIR      the builds [${XDG_DATA_HOME:-~/.local/share}/vitrine/host-kwin]
#   --work DIR     kde-rpms.sh's work folder (its VGD_BUILD) and dnf's cache,
#                  kept for the next build [${XDG_CACHE_HOME:-~/.cache}/vitrine/host-kwin-build]
#   -j N           parallel jobs [nproc, at most 8]
#   --print-set VERSION  the research's set for KWin VERSION: "SERIES TAG";
#                  status 1 if there is none
#   --print-recipe VERSION FEDORA  what the recipe makes of a host with
#                  Plasma VERSION on Fedora FEDORA, for the app: fedora= the
#                  container's Fedora, copr= the COPR's repository for beta,
#                  set= the set ("SERIES TAG", empty if none), key=
#   --print-plasma beta, testing or fedora for this host's Plasma; status 1
#                  if none of them
#   --print-inputs the lines the key sums up: research/ and this script
#   --print-key    their sha256
#   --print-deps   tell what the build needs that is missing, then stop
#                  (status 1 if something is)
#
# Environment: MEMORY [10g], the container's memory cap (podman's --memory);
# OS_RELEASE [/etc/os-release].
#
# A build goes to DIR/.new, then to DIR/<EVR>/, replacing one of the same
# EVR (KWin's version-release, 6.7.91-1.hl39.fc44): kde-rpms.sh's kwin/
# (the RPMs, the container's builddep and rpmbuild logs, and
# buildroot-TAG.txt: the packages they were built against, Qt's among them)
# and build.conf (what it is, and the RPMs' header digests, which tell
# Vitrine's KWin from another once installed).
#
# Lines starting with "vitrine-build: " are for the app, as build-rpms.sh's:
#   vitrine-build: phase: WHAT
#   vitrine-build: warning: MESSAGE
#   vitrine-build: built: DIRECTORY
#   vitrine-build: missing: COMMAND
#   vitrine-build: install: THE COMMAND INSTALLING WHAT IS MISSING
#   vitrine-build: error: MESSAGE
set -eu
shopt -s nullglob

here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
recipe=$here/research
versions=$recipe/setup/versions.sh

say() { echo "vitrine-build: $*"; }
die() { say "error: $*" >&2; exit 1; }
usage() { sed -n '/^#   host\/kwin\/build.sh \[/,/^#   --print-deps/p' "$here/build.sh" | sed 's/^# \{0,3\}//'; }

# What the key sums up: the copy of the recipe and this script, in the C
# locale's order
inputs_lines() {
	(cd "$here" && find research build.sh -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum)
}
key() { inputs_lines | sha256sum | cut -d' ' -f1; }

# The research's set for KWin $1: "SERIES TAG [EXCLUDED...]", as kde-rpms.sh
# takes it
set_for() {
	bash -c 'set -eu; . "$1"; kde_set_for host kwin "$2"' _ "$versions" "$1"
}

# The COPR the recipe enables for PLASMA=beta, by dnf's name for it, as the
# research's host-kde.sh names it
copr_id() {
	bash -c 'set -eu; . "$1"; c=copr:copr.fedorainfracloud.org:group_${GUEST_PLASMA_COPR#@}; echo "${c/\//:}"' \
		_ "$versions"
}

# beta, testing or fedora: where plasma-workspace came from (dnf5 knows).
# Fedora's own repositories have their names, and the installation's medium
# a hash of its own: anything but a COPR or a local file is Fedora's
plasma_of_host() {
	local repo
	repo=$(dnf -q -C repoquery --installed --qf '%{from_repo}\n' plasma-workspace 2> /dev/null | head -n 1)
	case "$repo" in
	"$(copr_id)") echo beta ;;
	updates-testing) echo testing ;;
	'' | copr:* | @commandline | '<unknown>') return 1 ;;
	*) echo fedora ;;
	esac
}

# --- arguments ----------------------------------------------------------------
action=build
plasma=
out=${XDG_DATA_HOME:-$HOME/.local/share}/vitrine/host-kwin
work=${XDG_CACHE_HOME:-$HOME/.cache}/vitrine/host-kwin-build
jobs=$(nproc)
[ "$jobs" -le 8 ] || jobs=8
version=
release=
while [ $# -gt 0 ]; do
	case "$1" in
	--plasma) plasma=${2:?--plasma needs beta, testing or fedora}; shift ;;
	--plasma=*) plasma=${1#*=} ;;
	--out) out=${2:?--out needs a folder}; shift ;;
	--out=*) out=${1#*=} ;;
	--work) work=${2:?--work needs a folder}; shift ;;
	--work=*) work=${1#*=} ;;
	-j) jobs=${2:?-j needs a number}; shift ;;
	-j*) jobs=${1#-j} ;;
	--print-set) action=print-set; version=${2:?--print-set needs a version}; shift ;;
	--print-recipe)
		action=print-recipe
		version=${2:?--print-recipe needs a version and a Fedora release}
		release=${3:?--print-recipe needs a version and a Fedora release}
		shift 2
		;;
	--print-plasma) action=print-plasma ;;
	--print-inputs) action=print-inputs ;;
	--print-key) action=print-key ;;
	--print-deps) action=print-deps ;;
	-h | --help) usage; exit 0 ;;
	*) echo "unknown argument $1 (see --help)" >&2; exit 2 ;;
	esac
	shift
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "-j takes a number of jobs, not $jobs"
case "$plasma" in '' | beta | testing | fedora) ;; *) die "--plasma takes beta, testing or fedora, not $plasma" ;; esac

case "$action" in
print-set) set_for "$version"; exit ;;
print-recipe)
	echo "fedora=$(VGD_FEDORA=$release bash -c 'set -eu; . "$1"; echo "$FEDORA"' _ "$versions")"
	echo "copr=$(copr_id)"
	echo "set=$(set_for "$version" || true)"
	echo "key=$(key)"
	exit 0
	;;
print-plasma) plasma_of_host; exit ;;
print-inputs) inputs_lines; exit 0 ;;
print-key) key; exit 0 ;;
esac

# --- what the build needs -------------------------------------------------------
missing=()
packages=()
for c in podman rpm flock; do
	if ! command -v "$c" > /dev/null; then
		missing+=("$c")
		case "$c" in flock) packages+=(util-linux-core) ;; *) packages+=("$c") ;; esac
	fi
done
for m in ${missing[@]+"${missing[@]}"}; do
	say "missing: $m"
done
if [ ${#missing[@]} -gt 0 ]; then
	say "install: sudo dnf install ${packages[*]}"
	[ "$action" = print-deps ] && exit 1
	die "${missing[*]} missing: podman builds KWin in a Fedora container"
fi
[ "$action" = print-deps ] && exit 0

# The container's Fedora must be the host's: VGD_FEDORA chooses it, if the
# recipe's versions.sh takes it
# shellcheck source=/dev/null
fedora=$(. "${OS_RELEASE:-/etc/os-release}" && echo "${VERSION_ID:-}")
[ -n "$fedora" ] || die "no VERSION_ID in ${OS_RELEASE:-/etc/os-release}: not a Fedora host"
recipe_fedora=$(VGD_FEDORA=$fedora bash -c 'set -eu; . "$1"; echo "$FEDORA"' _ "$versions")
[ "$recipe_fedora" = "$fedora" ] ||
	die "the research's recipe builds for Fedora $recipe_fedora, and this host runs Fedora $fedora"
if [ -z "$plasma" ]; then
	plasma=$(plasma_of_host) ||
		die "this host's Plasma comes neither from Fedora's repositories nor from the KDE SIG's beta COPR"
fi

mkdir -p "$out" "$work"
# one build at a time into $out: two would replace each other's
exec 9> "$out/.lock"
flock -n 9 || die "another build of the host's KWin is running into $out"
# the container named after $out: a build into another folder is not this one's
container=vitrine-host-kwin-$(printf '%s' "$out" | sha256sum | cut -c1-8)
podman_args="--name $container --memory=${MEMORY:-10g}"
new=$out/.new
logs=$work/kde-host-build/out/kwin

# A stop (Ctrl+C, the app's Stop, the app gone): podman passes a SIGTERM to
# the build, whose bash, the container's init, ignores it; the container
# goes here, and with it the partial output.  Another signal does not cut
# that short.
pid=
stop() {
	trap '' INT TERM HUP PIPE
	if [ -n "$pid" ]; then
		podman rm -f -t 0 "$container" > /dev/null 2>&1 || true
		for _ in $(seq 30); do
			kill -0 "$pid" 2> /dev/null || break
			sleep 0.1
		done
		# a podman that was still making it as the first rm went
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

# kde-rpms.sh as it is, told through its environment: no paths.conf of the
# research's, Vitrine's folders, the jobs, the Plasma, the host's Fedora,
# podman's options (the cap, the name Stop needs) and its output as it
# comes (builddep's and rpmbuild's, besides their logs)
env=(VGD_CONFIG=/dev/null VGD_BUILD="$work" VGD_JOBS="$jobs" PLASMA="$plasma" VGD_FEDORA="$fedora"
     VGD_PODMAN_ARGS="$podman_args" VGD_VERBOSE=1 VGD_REBUILD=0 VGD_KDE_PACKAGES=kwin)

# what a killed run left
if podman container exists "$container" 2> /dev/null; then
	podman rm -f -t 0 "$container" > /dev/null
fi
rm -rf "$new"
mkdir -p "$new"
say "phase: preparing the container"
env "${env[@]}" bash "$recipe/setup/tools/kde-rpms.sh" host "$new" 9>&- &
pid=$!
# What the container does now, from the logs it starts: builddep's, then
# rpmbuild's (their lines come through kde-rpms.sh)
(
	phase=
	while kill -0 "$pid" 2> /dev/null; do
		now=$phase
		for f in "$logs"/builddep-*.log; do now=deps; done
		for f in "$logs"/build-*.log; do now=build; done
		if [ "$now" != "$phase" ]; then
			phase=$now
			case "$phase" in
			deps) say "phase: installing the build dependencies" ;;
			build) say "phase: building the packages" ;;
			esac
		fi
		sleep 1
	done
) 9>&- &
watch=$!
rc=0
wait "$pid" || rc=$?
pid=
wait "$watch" 2> /dev/null || true
if [ "$rc" != 0 ]; then
	rm -rf "$new"
	die "the research's kde-rpms.sh failed (status $rc)"
fi

# --- what was built -------------------------------------------------------------
say "phase: keeping the packages"
kwin=
for f in "$new"/kwin/*.rpm; do
	[ "$(rpm -qp --qf '%{NAME}' "$f" 2> /dev/null)" != kwin ] || kwin=$f
done
[ -n "$kwin" ] || { rm -rf "$new"; die "no kwin package came out (logs: $logs)"; }
evr=$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$kwin")
kwin_version=$(rpm -qp --qf '%{VERSION}' "$kwin")
read -r series tag _ <<< "$(set_for "$kwin_version")" || true
{
	echo "# What host/kwin/build.sh built, for Vitrine"
	echo "evr=$evr"
	echo "version=$kwin_version"
	echo "set=${series:-}"
	echo "tag=${tag:-}"
	echo "plasma=$plasma"
	echo "fedora=$fedora"
	echo "key=$(key)"
	echo "research=$(sed -n 's/^research=//p' "$here/components.conf" | head -n 1)"
	echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	for f in "$new"/kwin/*.rpm; do
		echo "rpm=$(rpm -qp --qf '%{NAME} %{ARCH} %{SHA256HEADER}' "$f") kwin/$(basename "$f")"
	done
} > "$new/build.conf"
rm -rf "$out/.$evr.old"
[ ! -d "$out/$evr" ] || mv "$out/$evr" "$out/.$evr.old"
mv "$new" "$out/$evr"
rm -rf "$out/.$evr.old"
say "built: $out/$evr"
