#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs gpu-input-awake, the research's service that wakes the GPU from
# its deep idle at each key or pointer event (README.md), or removes it.
# Run as root: Vitrine runs it with pkexec, one action per run (one
# password).  Its files go where the research's own setup puts them, so that
# a host has one copy of it, whoever installed it:
#
#   install.sh install
#       gpu-input-awake and gpu-input-awake.service of this folder to
#       /usr/local/sbin and /etc/systemd/system, the unit enabled (it
#       starts at boot) and started again now, so that a copy running
#       takes the new script
#   install.sh remove
#       the unit stopped and disabled, both files removed, whichever copy
#       they are
#
# Lines starting with "vitrine-awake: " are for the app:
#   vitrine-awake: step: WHAT
#   vitrine-awake: warning: MESSAGE
#   vitrine-awake: error: MESSAGE
#   vitrine-awake: done
set -eu
umask 022

here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
# VITRINE_AWAKE_ROOT is the tests' fake root (pkexec passes no environment)
root=${VITRINE_AWAKE_ROOT:-}
name=gpu-input-awake.service
script=$root/usr/local/sbin/gpu-input-awake
unit=$root/etc/systemd/system/$name
wants=$root/etc/systemd/system/multi-user.target.wants/$name

say() { echo "vitrine-awake: $*"; }
die() { say "error: $*" >&2; exit 1; }
usage() { sed -n '/^#   install.sh install/,/^#       they are/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,3\}//'; }

# $1 to $2 with mode $3: written beside it, then renamed over it
put() {
	install -D -m "$3" -- "$1" "$2.new" || die "cannot write $2"
	mv -f -- "$2.new" "$2" || die "cannot write $2"
}

install_it() {
	[ -f "$here/gpu-input-awake" ] && [ -f "$here/$name" ] ||
		die "$here has no gpu-input-awake to install"
	# the unit runs it with /usr/bin/python3
	[ -x /usr/bin/python3 ] || die "python3 is not installed (/usr/bin/python3): the service runs on it"
	say "step: installing ${script#"$root"} and ${unit#"$root"}"
	put "$here/gpu-input-awake" "$script" 0755
	put "$here/$name" "$unit" 0644
	say "step: starting it (systemctl daemon-reload, enable, restart)"
	systemctl daemon-reload || die "systemctl daemon-reload failed"
	systemctl enable "$name" || die "systemctl enable $name failed"
	systemctl restart "$name" || die "systemctl restart $name failed"
	say "done"
}

remove_it() {
	say "step: stopping and disabling $name"
	if [ -e "$unit" ]; then
		systemctl disable --now "$name" || say "warning: systemctl could not stop or disable $name"
	else
		# a unit running from a file removed by hand
		systemctl stop "$name" > /dev/null 2>&1 || true
	fi
	# enabled once, its unit removed since: disable cannot find it
	if [ -L "$wants" ]; then
		rm -f -- "$wants"
	fi
	say "step: removing ${script#"$root"} and ${unit#"$root"}"
	rm -f -- "$script" "$unit" || die "cannot remove them"
	systemctl daemon-reload || say "warning: systemctl daemon-reload failed"
	systemctl reset-failed "$name" > /dev/null 2>&1 || true
	say "done"
}

[ $# = 1 ] || { usage >&2; exit 2; }
case "$1" in
install | remove) ;;
-h | --help) usage; exit 0 ;;
*) usage >&2; exit 2 ;;
esac
[ "$(id -u)" = 0 ] || die "run it as root (pkexec)"
command -v systemctl > /dev/null || die "systemctl not found: the service needs systemd"
case "$1" in
install) install_it ;;
remove) remove_it ;;
esac
