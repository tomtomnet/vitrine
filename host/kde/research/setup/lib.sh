# shellcheck shell=bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by the setup steps: locations, pinned versions and helpers.
set -euo pipefail
VGD_REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
. "$VGD_REPO/setup/paths.sh"
. "$VGD_REPO/setup/versions.sh"
VGD_STAMPS=$VGD_BUILD/setup-stamps
SUDO=(sudo)
[ "$(id -u)" = 0 ] && SUDO=()

say() { printf '== %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
need() {
	local c
	for c; do command -v "$c" > /dev/null || die "$c not found (setup/setup.sh host-packages installs it)"; done
}
# compile jobs: VGD_JOBS, else the CPUs but at most one per 2 GiB of available memory (2026-10-19: 16 KWin compile
# jobs beside the laptop's 8 GiB macOS guest left 1.4 GiB free and the swap full)
jobs_n() {
	if [ -n "${VGD_JOBS:-}" ]; then echo "$VGD_JOBS"; return; fi
	local cpus mem
	cpus=$(nproc)
	mem=$(awk '/^MemAvailable:/ { print int($2 / 2097152) }' /proc/meminfo)
	[ "${mem:-0}" -ge 1 ] || mem=1
	echo $(( cpus < mem ? cpus : mem ))
}
# long builds at low priority: the machine stays usable while they run
low() {
	if command -v ionice > /dev/null; then nice -n 10 ionice -c 2 -n 7 "$@"; else nice -n 10 "$@"; fi
}
# git that never asks for credentials (a wrong URL fails instead of opening a password dialog)
git_() { GIT_TERMINAL_PROMPT=0 GIT_ASKPASS='' SSH_ASKPASS='' git -c credential.helper= "$@"; }
# git am needs a committer; use the user's identity, or a placeholder where git has none
git_am() {
	local dir=$1; shift
	if git -C "$dir" config user.email > /dev/null; then
		git_ -C "$dir" am -q "$@"
	else
		git_ -C "$dir" -c user.name="virtio-gpu-desktop setup" -c user.email=setup@localhost am -q "$@"
	fi
}
# clone_at URL DIR COMMIT [REFERENCE]: a clone of URL in DIR that has COMMIT (an existing clone is
# fetched if it lacks it). REFERENCE: a local clone of the same project to take objects from.
clone_at() {
	local url=$1 dir=$2 commit=$3 ref=${4:-}
	if [ ! -d "$dir/.git" ]; then
		say "cloning $url"
		rm -rf "$dir.part"
		if [ -n "$ref" ] && [ -d "$ref/.git" ]; then
			git_ clone -q --reference-if-able "$ref" --dissociate "$url" "$dir.part"
		else
			git_ clone -q "$url" "$dir.part"
		fi
		mv "$dir.part" "$dir"
	fi
	git_ -C "$dir" cat-file -e "$commit^{commit}" 2> /dev/null || git_ -C "$dir" fetch -q origin
	git_ -C "$dir" cat-file -e "$commit^{commit}" 2> /dev/null || die "$url has no commit $commit"
}
# stamps: what a step last built, so that running it again skips what is done
stamp_get() { cat "$VGD_STAMPS/$1" 2> /dev/null || true; }
stamp_set() { mkdir -p "$VGD_STAMPS"; printf '%s\n' "$2" > "$VGD_STAMPS/$1"; }
hash_of() { cat "$@" | sha256sum | cut -c1-16; }
# fedora_container NAME SCRIPT [PODMAN OPTIONS...]: SCRIPT (bash) as root in a fresh fedora container;
# its dnf cache (metadata and packages) is kept in $VGD_BUILD/dnf-cache/NAME between runs
# VGD_PODMAN_ARGS: more podman run options from the caller, split on spaces (e.g. --name NAME, so that the
# container can be removed - its bash is PID 1 and ignores SIGTERM - and --memory=10g: a rootless container
# has a cgroup of its own, outside any scope around this script). VGD_VERBOSE goes into the container.
fedora_container() {
	local name=$1 script=$2 extra=() v; shift 2
	read -r -a extra <<< "${VGD_PODMAN_ARGS:-}"
	# the patch set overrides kde_set_for reads (VGD_GUEST_KWIN_SET, VGD_GUEST_KSVG_SET, VGD_GUEST_PW_SET...) go in too:
	# before 2026-10-17 they stayed outside unless passed through VGD_PODMAN_ARGS
	for v in $(compgen -e | grep -E '^VGD_(GUEST|HOST)_[A-Z0-9]+_SET$'); do extra+=(-e "$v"); done
	mkdir -p "$VGD_BUILD/dnf-cache/$name"
	low podman run --rm --security-opt label=disable \
		-v "$VGD_BUILD/dnf-cache/$name:/var/cache/libdnf5" -e JOBS="$(jobs_n)" -e VGD_VERBOSE="${VGD_VERBOSE:-0}" \
		"${extra[@]}" "$@" \
		"registry.fedoraproject.org/fedora:$FEDORA" bash -euo pipefail -c "
			echo keepcache=True >> /etc/dnf/dnf.conf
			$script"
}
