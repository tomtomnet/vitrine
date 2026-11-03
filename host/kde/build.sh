#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds one of the host's KDE packages with the research's host set:
# KWin (frame pacing for VMs, the pointer grab inside Vitrine's Desktop
# view...), KSvg, Kirigami or Qt base (components.conf).  It runs the
# research's recipe as it is: research/ is a pinned copy of
# virtio-gpu-desktop's setup/tools/kde-rpms.sh, of what it sources and of
# the patch sets it reads (README.md).  kde-rpms.sh takes the source RPM of
# the package's newest version in the host's repositories (Fedora's, plus
# the KDE SIG's beta COPR when the host's Plasma comes from it), the set its
# version maps to (setup/versions.sh: kde_set_for), and builds the RPMs in a
# Fedora container (rootless podman).  This only builds: install.sh
# installs them, as root.
#
#   host/kde/build.sh --package PACKAGE [--plasma beta|testing|fedora] [--out DIR] [--work DIR] [-j N]
#   host/kde/build.sh --print-set PACKAGE VERSION | --print-recipe FEDORA PACKAGE=VERSION... |
#                     --print-plasma | --print-inputs PACKAGE | --print-key PACKAGE | --print-deps
#
#   --package PACKAGE  the source package: kwin, kf6-ksvg, kf6-kirigami or
#                  qt6-qtbase (the package= lines of components.conf)
#   --plasma WHERE where the host's Plasma comes from: beta (the COPR that
#                  versions.sh's GUEST_PLASMA_COPR names), testing (Fedora's
#                  updates-testing) or fedora [from the repository
#                  plasma-workspace was installed from].  The container has
#                  the same repositories for every package.
#   --out DIR      the package's builds [${XDG_DATA_HOME:-~/.local/share}/vitrine/ID,
#                  ID its section in components.conf: host-kwin, host-ksvg...]
#   --work DIR     kde-rpms.sh's work folder (its VGD_BUILD) and dnf's cache,
#                  kept for the next build, one for all the packages
#                  [${XDG_CACHE_HOME:-~/.cache}/vitrine/host-kde-build]
#   -j N           parallel jobs [nproc, at most 8]
#   --print-set PACKAGE VERSION  the research's host set for PACKAGE VERSION:
#                  "SERIES TAG"; status 1 if there is none
#   --print-recipe FEDORA PACKAGE=VERSION...  what the recipe makes of a
#                  host on Fedora FEDORA with these versions, for the app:
#                  fedora= the container's Fedora, copr= the COPR's
#                  repository for beta, and per package set.PACKAGE= its set
#                  ("SERIES TAG", empty if none) and key.PACKAGE=
#   --print-plasma beta, testing or fedora for this host's Plasma; status 1
#                  if none of them
#   --print-inputs PACKAGE  the lines its key sums up: this script, the
#                  recipe's scripts (research/setup) and the package's
#                  patches (research/patches/DIR, kde_patch_dir)
#   --print-key PACKAGE  their sha256
#   --print-deps   tell what the build needs that is missing, then stop
#                  (status 1 if something is)
#
# Environment: MEMORY [10g], the container's memory cap (podman's --memory);
# OS_RELEASE [/etc/os-release].
#
# A build goes to DIR/.new, then to DIR/<EVR>/, replacing one of the same
# EVR (the package's version-release, 6.11.2-3.q4.fc44): kde-rpms.sh's
# PACKAGE/ (the RPMs, the container's builddep and rpmbuild logs, and
# buildroot-TAG.txt: the packages they were built against, Qt's among them)
# and build.conf (what it is, and the RPMs' header digests, which tell
# Vitrine's packages from others once installed).  One build at a time
# into the same work folder.
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
# The research's overrides of its sets (VGD_HOST_KWIN_SET=hl39 and the
# like, which versions.sh reads, and lib.sh passes into the container), its
# patches built in again (VGD_HOST_KWIN_0082=1: KWin's 0082, which the copy
# leaves out), and its switches of the host's packages (HOST_QT_Q4=0 and the
# like), are its own experiments: Vitrine builds the sets the copy names, for
# the package it is asked
while read -r v; do
	unset "$v"
done < <(compgen -e | grep -E '^VGD_[A-Z0-9_]+_(SET|0082)')
while read -r v; do
	unset "$v"
done < <(grep -oE '\$\{HOST_[A-Z0-9_]+:-' "$versions" | sed 's/^\${//; s/:-$//' | sort -u)

say() { echo "vitrine-build: $*"; }
die() { say "error: $*" >&2; exit 1; }
usage() { sed -n '/^#   host\/kde\/build.sh --package/,/^#   --print-deps/p' "$here/build.sh" | sed 's/^# \{0,3\}//'; }

# The section of components.conf whose package= is $1: host-kwin, host-ksvg...
section_of() {
	awk -v p="$1" '/^\[.*\]$/ { s = substr($0, 2, length($0) - 2) } $0 == "package=" p { print s; exit }' \
		"$here/components.conf"
}

# The folder of the recipe's patches for package $1 (versions.sh's kde_patch_dir)
patch_dir() {
	bash -c 'set -eu; . "$1"; kde_patch_dir "$2"' _ "$versions" "$1"
}

# What the key of package $1 sums up: this script, the recipe's scripts and
# the package's patches, in the C locale's order
inputs_lines() {
	local dir
	dir=$(patch_dir "$1")
	(cd "$here" && find build.sh research/setup "research/patches/$dir" -type f -print0 | LC_ALL=C sort -z |
		xargs -0 sha256sum)
}
key() { inputs_lines "$1" | sha256sum | cut -d' ' -f1; }

# The research's host set for package $1, version $2: "SERIES TAG [EXCLUDED...]",
# as kde-rpms.sh takes it
set_for() {
	bash -c 'set -eu; . "$1"; kde_set_for host "$2" "$3"' _ "$versions" "$1" "$2"
}

# Whether the recipe builds package $1 for hosts (versions.sh's HOST_KDE_PACKAGES)
is_host_package() {
	bash -c 'set -eu; . "$1"; case " $HOST_KDE_PACKAGES " in *" $2 "*) ;; *) exit 1 ;; esac' _ "$versions" "$1"
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

# A package name the recipe may be asked about: letters, digits, . + - _
check_package() {
	[[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9.+_-]*$ ]] || die "not a package name: $1"
	[ -n "$(section_of "$1")" ] || die "components.conf has no package $1"
}

# --- arguments ----------------------------------------------------------------
action=build
package=
plasma=
out=
work=${XDG_CACHE_HOME:-$HOME/.cache}/vitrine/host-kde-build
jobs=$(nproc)
[ "$jobs" -le 8 ] || jobs=8
version=
release=
asked=()
while [ $# -gt 0 ]; do
	case "$1" in
	--package) package=${2:?--package needs a package}; shift ;;
	--package=*) package=${1#*=} ;;
	--plasma) plasma=${2:?--plasma needs beta, testing or fedora}; shift ;;
	--plasma=*) plasma=${1#*=} ;;
	--out) out=${2:?--out needs a folder}; shift ;;
	--out=*) out=${1#*=} ;;
	--work) work=${2:?--work needs a folder}; shift ;;
	--work=*) work=${1#*=} ;;
	-j) jobs=${2:?-j needs a number}; shift ;;
	-j*) jobs=${1#-j} ;;
	--print-set)
		action=print-set
		package=${2:?--print-set needs a package and a version}
		version=${3:?--print-set needs a package and a version}
		shift 2
		;;
	--print-recipe)
		action=print-recipe
		release=${2:?--print-recipe needs a Fedora release}
		shift
		while [ $# -gt 1 ] && [[ $2 == *=* ]]; do
			asked+=("$2")
			shift
		done
		;;
	--print-plasma) action=print-plasma ;;
	--print-inputs) action=print-inputs; package=${2:?--print-inputs needs a package}; shift ;;
	--print-key) action=print-key; package=${2:?--print-key needs a package}; shift ;;
	--print-deps) action=print-deps ;;
	-h | --help) usage; exit 0 ;;
	*) echo "unknown argument $1 (see --help)" >&2; exit 2 ;;
	esac
	shift
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "-j takes a number of jobs, not $jobs"
case "$plasma" in '' | beta | testing | fedora) ;; *) die "--plasma takes beta, testing or fedora, not $plasma" ;; esac
[ -z "$package" ] || check_package "$package"

case "$action" in
print-set) set_for "$package" "$version"; exit ;;
print-recipe)
	echo "fedora=$(VGD_FEDORA=$release bash -c 'set -eu; . "$1"; echo "$FEDORA"' _ "$versions")"
	echo "copr=$(copr_id)"
	for a in ${asked[@]+"${asked[@]}"}; do
		p=${a%%=*}
		check_package "$p"
		echo "set.$p=$(set_for "$p" "${a#*=}" || true)"
		echo "key.$p=$(key "$p")"
	done
	exit 0
	;;
print-plasma) plasma_of_host; exit ;;
print-inputs) inputs_lines "$package"; exit 0 ;;
print-key) key "$package"; exit 0 ;;
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
	die "${missing[*]} missing: podman builds the packages in a Fedora container"
fi
[ "$action" = print-deps ] && exit 0

[ -n "$package" ] || die "which package? --package kwin, kf6-ksvg, kf6-kirigami or qt6-qtbase"
is_host_package "$package" || die "the research's recipe does not build $package for hosts"
[ -n "$out" ] || out=${XDG_DATA_HOME:-$HOME/.local/share}/vitrine/$(section_of "$package")

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
# one build at a time in the work folder (kde-rpms.sh starts it afresh), and
# into $out: two would replace each other's
exec 9> "$work/.lock"
flock -n 9 || die "another build of the host's packages is running in $work"
exec 8> "$out/.lock"
flock -n 8 || die "another build of $package is running into $out"
# the container named after the work folder: a build in another one is not this one's
container=vitrine-host-kde-$(printf '%s' "$work" | sha256sum | cut -c1-8)
podman_args="--name $container --memory=${MEMORY:-10g}"
new=$out/.new
logs=$work/kde-host-build/out/$package

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
# podman's options (the cap, the name Stop needs), its output as it comes
# (builddep's and rpmbuild's, besides their logs), and the one package
env=(VGD_CONFIG=/dev/null VGD_BUILD="$work" VGD_JOBS="$jobs" PLASMA="$plasma" VGD_FEDORA="$fedora"
     VGD_PODMAN_ARGS="$podman_args" VGD_VERBOSE=1 VGD_REBUILD=0 VGD_KDE_PACKAGES="$package")

# what a killed run left
if podman container exists "$container" 2> /dev/null; then
	podman rm -f -t 0 "$container" > /dev/null
fi
rm -rf "$new"
mkdir -p "$new"
say "phase: preparing the container"
env "${env[@]}" bash "$recipe/setup/tools/kde-rpms.sh" host "$new" 8>&- 9>&- &
pid=$!
# What the container does now, from the logs it starts: builddep's, then
# rpmbuild's (their lines come through kde-rpms.sh).  A line of its own: the
# container's output may be in the middle of one (dnf's)
(
	phase=
	while kill -0 "$pid" 2> /dev/null; do
		now=$phase
		for f in "$logs"/builddep-*.log; do now=deps; done
		for f in "$logs"/build-*.log; do now=build; done
		if [ "$now" != "$phase" ]; then
			phase=$now
			case "$phase" in
			deps) printf '\nvitrine-build: phase: installing the build dependencies\n' ;;
			build) printf '\nvitrine-build: phase: building the packages\n' ;;
			esac
		fi
		sleep 1
	done
) 8>&- 9>&- &
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
main=
for f in "$new/$package"/*.rpm; do
	[ "$(rpm -qp --qf '%{NAME}' "$f" 2> /dev/null)" != "$package" ] || main=$f
done
[ -n "$main" ] || { rm -rf "$new"; die "no $package package came out (logs: $logs)"; }
evr=$(rpm -qp --qf '%{VERSION}-%{RELEASE}' "$main")
built_version=$(rpm -qp --qf '%{VERSION}' "$main")
read -r series tag _ <<< "$(set_for "$package" "$built_version")" || true
{
	echo "# What host/kde/build.sh built, for Vitrine"
	echo "package=$package"
	echo "evr=$evr"
	echo "version=$built_version"
	echo "set=${series:-}"
	echo "tag=${tag:-}"
	echo "plasma=$plasma"
	echo "fedora=$fedora"
	echo "key=$(key "$package")"
	echo "research=$(sed -n 's/^research=//p' "$here/components.conf" | head -n 1)"
	echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	for f in "$new/$package"/*.rpm; do
		echo "rpm=$(rpm -qp --qf '%{NAME} %{ARCH} %{SHA256HEADER}' "$f") $package/$(basename "$f")"
	done
} > "$new/build.conf"
rm -rf "$out/.$evr.old"
[ ! -d "$out/$evr" ] || mv "$out/$evr" "$out/.$evr.old"
mv "$new" "$out/$evr"
rm -rf "$out/.$evr.old"
say "built: $out/$evr"
