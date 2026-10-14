#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs the host's KWin set that build.sh built, or puts the
# distribution's KWin back.  Run as root: Vitrine runs it with pkexec, one
# action per run (one password).  The packages are the KWin ones installed
# now (kwin, kwin-common, kwin-libs; kwin-devel if it is): the set's RPMs of
# other packages are left out, as the research's host-kde.sh does.  It takes
# effect at the next login: nothing here restarts the desktop.
#
#   install.sh install FILE=SHA256...
#       The set's RPMs and their sha256: copied to a folder of root's and
#       checked first (the files are in the user's folders), installed in
#       one dnf transaction (an upgrade, a downgrade, or a reinstall for a
#       rebuild of the same version), checked once installed (rpm's header
#       digests), then version-locked so that updates leave them alone.
#   install.sh revert DIGEST...
#       The version locks of the KWin packages go, and dnf distro-sync puts
#       the repositories' KWin back; only if the installed kwin's header
#       digest (rpm's SHA256HEADER) is one of these: Vitrine's own.
#
# A failure puts back the locks there were.  Lines starting with
# "vitrine-kwin: " are for the app:
#   vitrine-kwin: step: WHAT
#   vitrine-kwin: left out: NAME, not installed
#   vitrine-kwin: installed: NEVRA
#   vitrine-kwin: locked: NAME...
#   vitrine-kwin: warning: MESSAGE
#   vitrine-kwin: error: MESSAGE
#   vitrine-kwin: done
set -eu
shopt -s nullglob
umask 022

say() { echo "vitrine-kwin: $*"; }
die() { say "error: $*" >&2; exit 1; }
usage() { sed -n '/^#   install.sh install/,/^#       digest/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,3\}//'; }

# The KWin packages installed: those made from KWin's source RPM (kwin-x11
# has its own)
kwin_installed() {
	rpm -qa --qf '%{NAME} %{SOURCERPM}\n' | awk '$2 ~ /^kwin-[0-9]/ { print $1 }' | LC_ALL=C sort -u
}

# Of the names given, those dnf's versionlock has an entry for
locked_of() {
	local name n
	dnf versionlock list 2> /dev/null | sed -n 's/^Package name: *//p' | LC_ALL=C sort -u |
		while read -r name; do
			for n; do
				[ "$n" != "$name" ] || echo "$name"
			done
		done
}

# Locks again what was locked before a failure: the versions installed,
# which a failed transaction left as they were
relock() {
	[ $# = 0 ] || dnf versionlock add "$@" > /dev/null 2>&1 || say "warning: could not lock $* again"
}

# rpm's comparison of two EPOCH:VERSION-RELEASE: 1, 0 or -1
vercmp() {
	local a=$1 b=$2
	[[ $a =~ ^[A-Za-z0-9._+~^:-]+$ && $b =~ ^[A-Za-z0-9._+~^:-]+$ ]] || die "not a version: $a, $b"
	rpm --eval "%{lua: print(rpm.vercmp('$a', '$b'))}"
}

# The header digest and EPOCH:VERSION-RELEASE of package $1 of arch $2, installed
installed() {
	rpm -q --qf '%{ARCH} %{SHA256HEADER} %{EPOCHNUM}:%{VERSION}-%{RELEASE}\n' "$1" 2> /dev/null |
		awk -v arch="$2" '$1 == arch { print $2, $3; exit }'
}

install_set() {
	local arg file sum name arch digest evr have have_digest have_evr c kwin=0
	local -a names=() files=() up=() down=() re=() was=() actions=()
	[ $# -gt 0 ] || die "no packages given"
	tmp=$(mktemp -d /var/tmp/vitrine-kwin.XXXXXX)
	trap 'rm -rf "$tmp"' EXIT
	say "step: checking the packages"
	for arg; do
		file=${arg%=*}
		sum=${arg##*=}
		[[ $arg == *=* && $sum =~ ^[0-9a-f]{64}$ && $file == *.rpm && -f $file ]] ||
			die "not an RPM file and its sha256 (FILE.rpm=SHA256): $arg"
		cp -- "$file" "$tmp/"
		[ "$(sha256sum < "$tmp/${file##*/}" | cut -d' ' -f1)" = "$sum" ] ||
			die "${file##*/} is not the file that was built: its sha256 differs"
	done
	for file in "$tmp"/*.rpm; do
		[[ $(rpm -qp --qf '%{SOURCERPM}' "$file") == kwin-[0-9]* ]] || die "${file##*/} is not a KWin package"
		name=$(rpm -qp --qf '%{NAME}' "$file")
		arch=$(rpm -qp --qf '%{ARCH}' "$file")
		digest=$(rpm -qp --qf '%{SHA256HEADER}' "$file")
		evr=$(rpm -qp --qf '%{EPOCHNUM}:%{VERSION}-%{RELEASE}' "$file")
		have=$(installed "$name" "$arch")
		if [ -z "$have" ]; then
			say "left out: $name, not installed"
			continue
		fi
		have_digest=${have%% *}
		have_evr=${have#* }
		names+=("$name")
		files+=("$file")
		[ "$name" != kwin ] || kwin=1
		# this very build already
		[ "$have_digest" != "$digest" ] || continue
		c=$(vercmp "$evr" "$have_evr")
		case "$c" in
		1) up+=("$file") ;;
		-1) down+=("$file") ;;
		*) re+=("$file") ;;     # a rebuild of the same version
		esac
	done
	[ "$kwin" = 1 ] || die "KWin is not installed here: there is nothing to replace"
	mapfile -t was < <(locked_of "${names[@]}")

	if [ $(( ${#up[@]} + ${#down[@]} + ${#re[@]} )) -gt 0 ]; then
		if [ ${#was[@]} -gt 0 ]; then
			say "step: removing the version locks of ${was[*]}"
			dnf versionlock delete "${was[@]}"
		fi
		say "step: installing"
		if dnf "do" --help > /dev/null 2>&1; then
			# one transaction
			[ ${#up[@]} = 0 ] || actions+=(--action=upgrade "${up[@]}")
			[ ${#down[@]} = 0 ] || actions+=(--action=downgrade "${down[@]}")
			[ ${#re[@]} = 0 ] || actions+=(--action=reinstall "${re[@]}")
			dnf -y --setopt=install_weak_deps=False "do" "${actions[@]}" ||
				{ relock "${was[@]}"; die "dnf could not install them"; }
		else
			{ [ ${#up[@]} = 0 ] || dnf -y --setopt=install_weak_deps=False upgrade "${up[@]}"; } &&
				{ [ ${#down[@]} = 0 ] || dnf -y --setopt=install_weak_deps=False downgrade "${down[@]}"; } &&
				{ [ ${#re[@]} = 0 ] || dnf -y --setopt=install_weak_deps=False reinstall "${re[@]}"; } ||
				{ relock "${was[@]}"; die "dnf could not install them"; }
		fi
	fi

	# what is installed now is what was built
	for i in "${!names[@]}"; do
		arch=$(rpm -qp --qf '%{ARCH}' "${files[$i]}")
		have=$(installed "${names[$i]}" "$arch")
		[ "${have%% *}" = "$(rpm -qp --qf '%{SHA256HEADER}' "${files[$i]}")" ] ||
			{ relock "${was[@]}"; die "${names[$i]} is not the package built once installed"; }
	done
	say "step: locking ${names[*]}"
	# entries at the versions installed now
	mapfile -t was < <(locked_of "${names[@]}")
	[ ${#was[@]} = 0 ] || dnf versionlock delete "${was[@]}" > /dev/null
	dnf versionlock add "${names[@]}"
	for name in "${names[@]}"; do
		say "installed: $(rpm -q --qf '%{NEVRA}\n' "$name" | head -n 1)"
	done
	say "locked: ${names[*]}"
	say "done"
}

revert() {
	local digest d mine=0
	local -a names=() was=()
	[ $# -gt 0 ] || die "no digests given"
	mapfile -t names < <(kwin_installed)
	[ ${#names[@]} -gt 0 ] || die "KWin is not installed here"
	digest=$(rpm -q --qf '%{SHA256HEADER}\n' kwin 2> /dev/null | head -n 1)
	for d; do
		[ "$d" != "$digest" ] || mine=1
	done
	[ "$mine" = 1 ] || die "the KWin installed is not one Vitrine installed: Vitrine leaves it alone"
	mapfile -t was < <(locked_of "${names[@]}")
	if [ ${#was[@]} -gt 0 ]; then
		say "step: removing the version locks of ${was[*]}"
		dnf versionlock delete "${was[@]}"
	fi
	say "step: installing the distribution's KWin"
	dnf -y --setopt=install_weak_deps=False distro-sync "${names[@]}" ||
		{ relock "${was[@]}"; die "dnf could not install the distribution's KWin"; }
	# the same build still: the repositories have no other KWin
	[ "$(rpm -q --qf '%{SHA256HEADER}\n' kwin | head -n 1)" != "$digest" ] ||
		{ relock "${was[@]}"; die "the repositories have no other KWin: this one stays"; }
	for name in "${names[@]}"; do
		say "installed: $(rpm -q --qf '%{NEVRA}\n' "$name" | head -n 1)"
	done
	say "done"
}

[ $# -ge 1 ] || { usage >&2; exit 2; }
action=$1
shift
case "$action" in
install | revert) ;;
-h | --help) usage; exit 0 ;;
*) usage >&2; exit 2 ;;
esac
[ "$(id -u)" = 0 ] || die "run it as root (pkexec)"
command -v rpm > /dev/null || die "rpm not found"
dnf --version 2> /dev/null | grep -q '^dnf5' || die "this needs dnf5 (Fedora 41 or later)"
dnf versionlock list > /dev/null 2>&1 ||
	die "dnf has no versionlock command: without it an update would replace this KWin"
case "$action" in
install) install_set "$@" ;;
revert) revert "$@" ;;
esac
