#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# vitrine's Mesa for Flatpak apps, checked in a guest, by hand: whether one build (guest/build-flatpak.sh) loads
# and draws in each Flatpak runtime it is for, before a runtime joins serves= in guest/flatpak/branches.conf.
# As root, with this folder somewhere the test user can read (/var/tmp/vitrine-kit, or their home), and the
# build's repository beside it (repo/: a copy of ~/.local/share/vitrine/guest-tools/flatpak/repo):
#   kit.sh flathub         Flathub and the runtimes 25.08 and 26.08, with Flathub's GL extension (needs the network)
#   kit.sh check [LABEL]   for each runtime: its GL extension (whose, which commit), then in a sandbox of that runtime
#                          every shared object of the extension loaded, a triangle drawn through EGL, a Vulkan device
#                          made, VA-API's codecs; and the installed apps with the runtime each one is on
#   kit.sh vitrine         vitrine's Mesa (repo/ beside this script: ONE build, of 25.08's recipe, for both runtimes)
#                          in place of Flathub's GL.default//25.08, //25.08-extra, //26.08 and //26.08-extra
#   kit.sh back            each GL.default from the remote it came from before `vitrine` (recorded in
#                          /var/tmp/vitrine-kit-origins; Flathub when that remote no longer has it), and no
#                          remote vitrine-gl
# It needs no vitrine guest tools, and leaves them alone: with tools 0.1.0-46 or later in the guest, their own
# remote has the same name, vitrine-gl; `back` then leaves the guest without vitrine's Mesa until the tools are
# installed again.
# TEST_USER: whom the sandboxes run as (default: the user of the first session, else the first one in /home).
set -u
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
gl=org.freedesktop.Platform.GL.default
remote=vitrine-gl
branches="25.08 26.08"
[ "$(id -u)" = 0 ] || { echo "run it as root" >&2; exit 1; }
user=${TEST_USER:-$(loginctl list-users --no-legend 2> /dev/null | awk '$1 >= 1000 { print $2; exit }')}
[ -n "$user" ] || user=$(ls /home | head -n 1)
uid=$(id -u "$user") || exit 1
runuser -u "$user" -- test -r "$here/egl_render.py" || { echo "$user cannot read $here: move the kit" >&2; exit 1; }

origin_of() { flatpak info --system --show-origin "$gl//$1" 2> /dev/null < /dev/null; }
refs() { flatpak list --system --runtime --columns=ref,origin 2> /dev/null < /dev/null | grep -E 'Platform(\.GL(32)?)?\.?[A-Za-z0-9_-]*/' | sed 's/^/   /'; }

check() {
	# a user manager for the sandbox: lingering when the user has no session, for this check only
	local lingered=
	if [ ! -d "/run/user/$uid" ]; then
		loginctl enable-linger "$user" && lingered=1
		sleep 2
	fi
	echo "== $1: $(. /etc/os-release; echo "$PRETTY_NAME"), kernel $(uname -r), flatpak $(flatpak --version | cut -d' ' -f2), as $user"
	echo "   guest Mesa: $(rpm -q mesa-dri-drivers 2> /dev/null | head -n 1)   guest tools: $(rpm -q vitrine-guest-tools 2> /dev/null | head -n 1)"
	grep -sH FLATPAK_GL_DRIVERS /etc/environment /etc/environment.d/*.conf /etc/profile.d/*.sh | sed 's/^/   /' || true
	for b in $branches; do
		echo "-- $1, runtime $b"
		if ! flatpak info --system "org.freedesktop.Platform//$b" > /dev/null 2>&1 < /dev/null; then
			echo "   org.freedesktop.Platform//$b is not installed"
			continue
		fi
		for v in "$b" "$b-extra"; do
			o=$(origin_of "$v")
			if [ -n "$o" ]; then
				echo "   $gl//$v: from $o, commit $(flatpak info --system --show-commit "$gl//$v" < /dev/null | cut -c1-12), $(flatpak info --system "$gl//$v" < /dev/null | sed -n 's/^ *Subject: *//p')"
			else
				echo "   $gl//$v: not installed"
			fi
		done
		runuser -u "$user" -- env XDG_RUNTIME_DIR="/run/user/$uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" \
			flatpak run --device=dri --filesystem="$here:ro" --command=sh "org.freedesktop.Platform//$b" -c "
				echo \"FLATPAK_GL_DRIVERS in the sandbox: \${FLATPAK_GL_DRIVERS:-not set}\"
				python3 $here/gl_load.py
				LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug python3 $here/egl_render.py 2> /tmp/egl.err
				grep -E 'MESA-LOADER|libEGL (debug|warning|error|fatal)|libGL' /tmp/egl.err | awk '!seen[\$0]++' | head -n 14 | sed 's/^/loader: /'
				python3 $here/vk_device.py 2>&1
				python3 $here/va_info.py 2>&1" < /dev/null 2>&1 | sed 's/^/   /'
	done
	echo "-- $1, the apps and the runtime each is on"
	flatpak list --system --app --columns=application,runtime 2> /dev/null < /dev/null | sed 's/^/   /'
	flatpak list --user --app --columns=application,runtime 2> /dev/null < /dev/null | sed 's/^/   (user) /' || true
	[ -z "$lingered" ] || loginctl disable-linger "$user"
}

case ${1:-} in
flathub)
	flatpak remote-add --system --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo < /dev/null
	for b in $branches; do
		flatpak install --system -y --noninteractive flathub "org.freedesktop.Platform//$b" < /dev/null > /dev/null ||
			echo "org.freedesktop.Platform//$b could not be installed"
	done
	refs
	;;
check) check "${2:-now}" ;;
vitrine)
	[ -d "$here/repo" ] || { echo "no repo/ beside this script" >&2; exit 1; }
	# a copy flatpak's helper can read whatever the kit's folder allows
	rm -rf /var/tmp/vitrine-kit-repo && cp -r "$here/repo" /var/tmp/vitrine-kit-repo
	if flatpak remote-list --system --columns=name < /dev/null | grep -qx "$remote"; then
		echo "   the remote $remote is there already (the guest tools'): pointed at this kit's repository"
		flatpak remote-modify --system --no-gpg-verify --url=file:///var/tmp/vitrine-kit-repo "$remote" < /dev/null
	else
		flatpak remote-add --system --no-gpg-verify --prio=0 "$remote" file:///var/tmp/vitrine-kit-repo < /dev/null
	fi
	for b in $branches; do
		for v in "$b" "$b-extra"; do
			o=$(origin_of "$v")
			[ -n "$o" ] || flatpak info --system "org.freedesktop.Platform//$b" > /dev/null 2>&1 < /dev/null || continue
			# where it came from, for `back` (the first run's: a second `vitrine` sees vitrine-gl)
			if [ -n "$o" ] && [ "$o" != "$remote" ] && ! grep -q "^$v " /var/tmp/vitrine-kit-origins 2> /dev/null; then
				echo "$v $o" >> /var/tmp/vitrine-kit-origins
			fi
			# another remote's goes first: flatpak takes no commit over a newer one of another remote
			[ -z "$o" ] || flatpak uninstall --system -y --noninteractive "$gl//$v" < /dev/null > /dev/null
			was=
			[ -z "$o" ] || was=", in place of the one from $o"
			if flatpak install --system -y --noninteractive "$remote" "$gl//$v" < /dev/null > /dev/null; then
				echo "   $gl//$v: vitrine's$was"
			else
				echo "   $gl//$v: vitrine's COULD NOT BE INSTALLED"
				[ -z "$o" ] || [ "$o" = "$remote" ] || flatpak install --system -y --noninteractive "$o" "$gl//$v" < /dev/null > /dev/null
			fi
		done
	done
	refs
	;;
back)
	for b in $branches; do
		for v in "$b" "$b-extra"; do
			[ "$(origin_of "$v")" = "$remote" ] || continue
			# the remote it came from, if it still has it; else Flathub
			r=$(awk -v v="$v" '$1 == v { print $2; exit }' /var/tmp/vitrine-kit-origins 2> /dev/null)
			if [ -z "$r" ] || ! flatpak remote-info --system "$r" "$gl//$v" > /dev/null 2>&1 < /dev/null; then
				[ -z "$r" ] || echo "   $gl//$v came from $r, which no longer has it: Flathub's instead"
				r=flathub
			fi
			flatpak uninstall --system -y --noninteractive "$gl//$v" < /dev/null > /dev/null &&
				flatpak install --system -y --noninteractive "$r" "$gl//$v" < /dev/null > /dev/null &&
				echo "   $gl//$v: $r's again" || echo "   $gl//$v: COULD NOT GO BACK (flatpak install $r $gl//$v)"
		done
	done
	flatpak remote-delete --system --force "$remote" < /dev/null
	rm -rf /var/tmp/vitrine-kit-repo /var/tmp/vitrine-kit-origins
	refs
	;;
*) sed -n '2,/^set /{/^#/p}' "$here/kit.sh" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
