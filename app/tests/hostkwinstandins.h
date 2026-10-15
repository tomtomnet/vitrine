// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * Stand-ins for rpm and dnf, shared by the tests of the host's KWin and of
 * its banner, on a state folder STATE: installed/NAME and repo/NAME (what
 * distro-sync brings) hold NAME=value lines as the "RPM files" do (NAME,
 * VERSION, RELEASE, ARCH, DIGEST, SOURCERPM, EPOCHNUM), locks/NAME the EVR
 * locked.  Enough of both for install.sh and for System::read(); rpm
 * --eval goes to the real rpm (vercmp).  (No line of them starts with a #,
 * nor has a \": moc would take them for a directive or a string's end.)
 */
inline constexpr char kFakeRpm[] = R"sh(#!/bin/bash
state=${STATE:?}
field() { sed -n "s/^$2=//p" "$1" | head -n 1; }
format() {
	local f=$1 fmt=$2 tag
	for tag in NAME VERSION RELEASE ARCH SOURCERPM EPOCHNUM; do
		fmt=${fmt//%\{$tag\}/$(field "$f" "$tag")}
	done
	fmt=${fmt//%\{SHA256HEADER\}/$(field "$f" DIGEST)}
	fmt=${fmt//%\{VENDOR\}/$(field "$f" VENDOR)}
	fmt=${fmt//%\{INSTALLTIME\}/$(field "$f" INSTALLTIME)}
	fmt=${fmt//%\{NEVRA\}/$(field "$f" NAME)-$(field "$f" VERSION)-$(field "$f" RELEASE).$(field "$f" ARCH)}
	printf '%b' "$fmt"
}
echo "rpm $*" >> "$state/calls"
case "$1" in
--eval) exec /usr/bin/rpm "$@" ;;
-qp) shift; [ "$1" = --qf ] && { fmt=$2; shift 2; }; format "$1" "$fmt"; exit 0 ;;
-qa) shift; [ "$1" = --qf ] && fmt=$2; for f in "$state"/installed/*; do [ -e "$f" ] && format "$f" "$fmt"; done; exit 0 ;;
-q)
	shift
	[ "${1:-}" = --qf ] && { fmt=$2; shift 2; }
	rc=0
	[ -n "${fmt:-}" ] || fmt='%{NAME}\n'
	for n; do
		if [ -e "$state/installed/$n" ]; then format "$state/installed/$n" "$fmt"; else echo "package $n is not installed"; rc=1; fi
	done
	exit $rc
	;;
esac
exit 2
)sh";

inline constexpr char kFakeDnf[] = R"sh(#!/bin/bash
state=${STATE:?}
echo "dnf $*" >> "$state/calls"
field() { sed -n "s/^$2=//p" "$1" | head -n 1; }
args=()
for a; do case "$a" in -y | -q | -C | --setopt=*) ;; *) args+=("$a") ;; esac; done
set -- "${args[@]}"
locked() { [ -e "$state/locks/$1" ] && [ "$(cat "$state/locks/$1")" != "$2" ]; }
take() {
	local f=$1 n
	n=$(field "$f" NAME)
	if locked "$n" "$(field "$f" VERSION)-$(field "$f" RELEASE)"; then echo "$n is version-locked" >&2; return 1; fi
	cp "$f" "$state/installed/$n"
}
case "$1" in
--version) echo "dnf5 version 5.4.6.0"; echo "libdnf5 version 5.4.6.0" ;;
versionlock)
	case "$2" in
	list) for f in "$state"/locks/*; do [ -e "$f" ] && printf 'Package name: %s\nevr = %s\n\n' "${f##*/}" "$(cat "$f")"; done ;;
	add) shift 2; for n; do echo "$(field "$state/installed/$n" VERSION)-$(field "$state/installed/$n" RELEASE)" > "$state/locks/$n"; done ;;
	delete) shift 2; for n; do rm -f "$state/locks/$n"; done ;;
	esac
	;;
do)
	[ "${2:-}" = --help ] && { [ -e "$state/no-do" ] && exit 2; exit 0; }
	[ -e "$state/fail" ] && { echo "Transaction failed" >&2; exit 1; }
	shift
	for a; do case "$a" in --action=*) echo "${a#--action=}" >> "$state/actions" ;; *) take "$a" || exit 1 ;; esac; done
	;;
upgrade | downgrade | reinstall)
	[ -e "$state/fail" ] && exit 1
	echo "$1" >> "$state/actions"
	shift
	for f; do take "$f" || exit 1; done
	;;
distro-sync)
	[ -e "$state/fail" ] && exit 1
	shift
	for n; do [ -e "$state/repo/$n" ] && { take "$state/repo/$n" || exit 1; }; done
	;;
repoquery)
	shift
	for n; do [ -e "$state/installed/$n" ] && echo "$n $(field "$state/installed/$n" REPO)"; done
	;;
esac
exit 0
)sh";
