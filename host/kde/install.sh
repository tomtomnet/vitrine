#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs the builds of the host's KDE packages that build.sh made (KWin,
# KSvg, Kirigami, Qt base: the package= lines of components.conf), or puts
# the distribution's packages back.  Run as root: Vitrine runs it with
# pkexec, one action per run (one password).  Of each source package, the
# packages installed now are replaced, all of them from the same build:
# they require each other at the same version (Qt base's do, at the same
# release), so if one of them is installed and the build has none, nothing
# is done.  The build's packages not installed are left out, as the
# research's host-kde.sh does.  It takes effect for programs started after
# it, the desktop at the next login: nothing here restarts it.
#
#   install.sh install FILE=SHA256...
#       Builds' RPMs and their sha256: copied to a folder of root's and
#       checked first (the files are in the user's folders), installed in
#       one dnf transaction (an upgrade, a downgrade, or a reinstall for a
#       rebuild of the same version), checked once installed (rpm's header
#       digests), then version-locked by their exact names so that updates
#       leave them alone.  With KWin, its set's run-time switches (below) go
#       on.
#   install.sh revert PACKAGE=DIGEST...
#       Of each source package named, when its package (kwin, qt6-qtbase...)
#       has one of its header digests given (rpm's SHA256HEADER: Vitrine's
#       own): the version locks of its installed packages go, and dnf
#       distro-sync puts the repositories' back, one transaction for all.
#       KWin's switches go too.
#
# KWin's switches: some of the research's KWin patches act only when a file
# of $XDG_RUNTIME_DIR says so, which KWin reads as it starts (research/src/
# host-config/kwin-*.conf, user tmpfiles entries: each one's comment says
# what it does).  Those of kwin_switches go into
# /etc/user-tmpfiles.d/vitrine-host-kwin.conf, which makes the files at each
# user's login, and are made at once for the user who installs (pkexec's
# PKEXEC_UID): a login soon after a logout may find their session manager
# still running, and its tmpfiles not read again.  Another KWin ignores the
# files.
#
# A failure puts back the locks there were.  Lines starting with
# "vitrine-kde: " are for the app:
#   vitrine-kde: step: WHAT
#   vitrine-kde: left out: NAME, not installed
#   vitrine-kde: installed: NEVRA
#   vitrine-kde: locked: NAME...
#   vitrine-kde: warning: MESSAGE
#   vitrine-kde: error: MESSAGE
#   vitrine-kde: done
set -eu
shopt -s nullglob
umask 022

here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
# VITRINE_TMPFILES_DIR and VITRINE_RUN_USER are the tests' (pkexec passes no environment)
switches=${VITRINE_TMPFILES_DIR:-/etc/user-tmpfiles.d}/vitrine-host-kwin.conf
# KWin's run-time switches that an install turns on, by their file in
# research/src/host-config (kwin-NAME.conf, the patch it is for in its
# comment): remove a line to leave one off
kwin_switches=(
	active-window-candidate
	animation-start-step
	empty-render-not-content
	scanout-variance-cap
	skip-empty-layer-render
)

say() { echo "vitrine-kde: $*"; }
die() { say "error: $*" >&2; exit 1; }
usage() { sed -n '/^#   install.sh install/,/^#       KWin.s switches go too/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,3\}//'; }

# The source packages this installs: components.conf's
allowed() {
	sed -n 's/^package=//p' "$here/components.conf"
}

# "NAME ARCH" of the packages installed of source package $1, but its debug
# information, which an install leaves alone and whose repository is seldom
# enabled
of_source() {
	rpm -qa --qf '%{NAME} %{ARCH} %{SOURCERPM}\n' |
		awk -v s="$1" '{ n = $3; sub(/\.(no)?src\.rpm$/, "", n); sub(/-[^-]+-[^-]+$/, "", n) }
			n == s && $1 !~ /-debug(info|source)$/ { print $1, $2 }' | LC_ALL=C sort -u
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

# The switches on: the entries of every user, and the files of the user who
# installs.  A failure to make those is only a warning: the next login makes
# them.
switches_on() {
	local -a entries=()
	local uid=${PKEXEC_UID:-} run user name
	for name in "${kwin_switches[@]}"; do
		[ -f "$here/research/src/host-config/kwin-$name.conf" ] ||
			{ say "warning: no switch kwin-$name in research/src/host-config"; continue; }
		entries+=("$here/research/src/host-config/kwin-$name.conf")
	done
	[ ${#entries[@]} -gt 0 ] || return 0
	say "step: turning the set's run-time switches on ($switches)"
	mkdir -p "${switches%/*}"
	{
		echo "# Vitrine's host KWin (host/kde/install.sh): the run-time switches of the research's"
		echo "# patches, a file each in \$XDG_RUNTIME_DIR that KWin reads as it starts. Reverting"
		echo "# Vitrine's KWin removes this file; without the patches, KWin ignores them."
		cat "${entries[@]}"
	} > "$switches.new"
	mv -f "$switches.new" "$switches"
	run=${VITRINE_RUN_USER:-/run/user}/$uid
	if [[ $uid =~ ^[1-9][0-9]*$ ]] && [ -d "$run" ] && user=$(id -nu "$uid" 2> /dev/null); then
		runuser -u "$user" -- env XDG_RUNTIME_DIR="$run" systemd-tmpfiles --user --create "$switches" ||
			say "warning: the switches' files were not made now: they come at the next login"
	fi
}

switches_off() {
	if [ -e "$switches" ]; then
		say "step: removing the set's run-time switches ($switches)"
		rm -f "$switches"
	fi
}

install_sets() {
	local arg file sum name arch digest evr have have_digest have_evr c src n a found
	local -a names=() files=() up=() down=() re=() was=() actions=() sources=() rpms=()
	[ $# -gt 0 ] || die "no packages given"
	# root's own: pkexec gives no TMPDIR, so /var/tmp, on disk rather than in memory
	tmp=$(mktemp -d "${TMPDIR:-/var/tmp}/vitrine-kde.XXXXXX")
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
	rpms=("$tmp"/*.rpm)
	# each file's source package, one of those this installs
	for file in "${rpms[@]}"; do
		src=$(rpm -qp --qf '%{SOURCERPM}' "$file" | sed -E 's/\.(no)?src\.rpm$//; s/-[^-]+-[^-]+$//')
		allowed | grep -qxF -- "$src" || die "${file##*/} is not a package of $(allowed | tr '\n' ' ')"
		[[ " ${sources[*]-} " == *" $src "* ]] || sources+=("$src")
	done
	# of each, all its packages installed or none: they need each other at one version
	for src in "${sources[@]}"; do
		rpm -q "$src" > /dev/null 2>&1 || die "$src is not installed here: there is nothing to replace"
		while read -r n a; do
			found=0
			for file in "${rpms[@]}"; do
				[ "$(rpm -qp --qf '%{NAME} %{ARCH}' "$file")" != "$n $a" ] || found=1
			done
			[ "$found" = 1 ] ||
				die "$n ($a) is installed and the build of $src has none: Vitrine replaces all of $src's packages or none"
		done < <(of_source "$src")
	done
	for file in "${rpms[@]}"; do
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
		# this very build already
		[ "$have_digest" != "$digest" ] || continue
		c=$(vercmp "$evr" "$have_evr")
		case "$c" in
		1) up+=("$file") ;;
		-1) down+=("$file") ;;
		*) re+=("$file") ;;     # a rebuild of the same version
		esac
	done
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
	# entries at the versions installed now, of these exact names
	mapfile -t was < <(locked_of "${names[@]}")
	[ ${#was[@]} = 0 ] || dnf versionlock delete "${was[@]}" > /dev/null
	dnf versionlock add "${names[@]}"
	[[ " ${sources[*]} " != *" kwin "* ]] || switches_on
	for name in "${names[@]}"; do
		say "installed: $(rpm -q --qf '%{NEVRA}\n' "$name" | head -n 1)"
	done
	say "locked: ${names[*]}"
	say "done"
}

revert() {
	local arg src d digest mine n a kwin=0
	local -a sources=() names=() was=() again=() stayed=()
	local -A given=() before=()
	[ $# -gt 0 ] || die "no packages given"
	for arg; do
		src=${arg%%=*}
		d=${arg#*=}
		[[ $arg == *=* && $d =~ ^[0-9a-f]{64}$ ]] || die "not a package and a digest (PACKAGE=SHA256): $arg"
		allowed | grep -qxF -- "$src" || die "$src is not a package of $(allowed | tr '\n' ' ')"
		[[ " ${sources[*]-} " == *" $src "* ]] || sources+=("$src")
		given[$src]="${given[$src]:-} $d"
	done
	for src in "${sources[@]}"; do
		digest=$(rpm -q --qf '%{SHA256HEADER}\n' "$src" 2> /dev/null | head -n 1)
		[ -n "$digest" ] || die "$src is not installed here"
		mine=0
		for d in ${given[$src]}; do
			[ "$d" != "$digest" ] || mine=1
		done
		[ "$mine" = 1 ] || die "the $src installed is not one Vitrine installed: Vitrine leaves it alone"
		before[$src]=$digest
		while read -r n a; do
			[[ " ${names[*]-} " == *" $n "* ]] || names+=("$n")
		done < <(of_source "$src")
		[ "$src" != kwin ] || kwin=1
	done
	mapfile -t was < <(locked_of "${names[@]}")
	if [ ${#was[@]} -gt 0 ]; then
		say "step: removing the version locks of ${was[*]}"
		dnf versionlock delete "${was[@]}"
	fi
	say "step: installing the distribution's packages"
	dnf -y --setopt=install_weak_deps=False distro-sync "${names[@]}" ||
		{ relock "${was[@]}"; die "dnf could not install the distribution's packages"; }
	# the same build still: the repositories have no other; its locks back
	for src in "${sources[@]}"; do
		if [ "$(rpm -q --qf '%{SHA256HEADER}\n' "$src" | head -n 1)" = "${before[$src]}" ]; then
			stayed+=("$src")
			while read -r n a; do
				for d in "${was[@]}"; do
					[ "$d" != "$n" ] || again+=("$n")
				done
			done < <(of_source "$src")
		fi
	done
	if [ ${#stayed[@]} -gt 0 ]; then
		relock ${again[@]+"${again[@]}"}
		die "the repositories have no other ${stayed[*]}: Vitrine's stays"
	fi
	[ "$kwin" = 0 ] || switches_off
	for n in "${names[@]}"; do
		say "installed: $(rpm -q --qf '%{NEVRA}\n' "$n" | head -n 1)"
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
	die "dnf has no versionlock command: without it an update would replace these packages"
case "$action" in
install) install_sets "$@" ;;
revert) revert "$@" ;;
esac
