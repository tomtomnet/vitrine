#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Make the guest tools medium: a FAT image labelled VITRINETOOL (FAT labels
# hold 11 characters) with a dnf repository of the packages build-rpms.sh
# made and the installer.  vitrine attaches it to a VM read-only, and the
# guest installs from it: at boot (the bootstrap vitrine hands its systemd),
# through the vitrine agent, or by hand:
#   sudo bash /run/media/$USER/VITRINETOOL/install
#
#   guest/build-medium.sh [--print-deps]
#
# In:  $RPMS_DIR [~/.local/share/vitrine/guest-tools/rpms/fcRELEASE]/PACKAGE/
#      *.rpm, for the packages of the medium's deps in components.conf (tools
#      needed; without Mesa, KWin, KPipeWire, Qt Base, KSvg, Kirigami or
#      Mutter the medium keeps the guest's own, without the proxy there is no
#      nested desktop).  A package built against another (its deps=: KWin
#      against KWayland) goes on the medium only with that one's packages of
#      the build it was built against: else no medium.
#      $FLATPAK_DIR [~/.local/share/vitrine/guest-tools/flatpak]: the Mesa for
#      Flatpak apps of build-flatpak.sh (a dep of kind=flatpak), every
#      release's: without it Flatpak apps keep Flathub's
# Out: $OUT [~/.local/share/vitrine/guest-tools/vitrine-guest-tools-fcRELEASE.img]
#      and its manifest beside it (.json: versions, packages, and the inputs
#      of the packages and of this script, which tell vitrine whether the
#      medium is out of date), replaced once complete
# Needs mkfs.fat and mcopy (dosfstools, mtools), rpm, and createrepo_c or
# podman (then it runs in the Fedora container); --print-deps tells what is
# missing (status 1 if something is).
# Environment: FEDORA_RELEASE [44].
#
# Lines starting with "vitrine-build: " are for vitrine (see build-rpms.sh):
# phase, warning, missing, install, error, and "built: IMAGE" at the end.
set -eu
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
release=${FEDORA_RELEASE:-44}
data=${XDG_DATA_HOME:-$HOME/.local/share}/vitrine/guest-tools
rpms=${RPMS_DIR:-$data/rpms/fc$release}
flatpak=${FLATPAK_DIR:-$data/flatpak}
out=${OUT:-$data/vitrine-guest-tools-fc$release.img}
label=VITRINETOOL

say() { echo "vitrine-build: $*"; }
die() { say "error: $*" >&2; exit 1; }
action=build
case "${1:-}" in
-h|--help) sed -n '3,/^set /{/^#/p}' "$here/build-medium.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
--print-deps) action=print-deps ;;
"") ;;
*) echo "unknown argument $1 (see --help)" >&2; exit 2 ;;
esac
missing=()
packages=()
for c in mkfs.fat:dosfstools mcopy:mtools rpm:rpm flock:util-linux-core; do
	command -v "${c%%:*}" > /dev/null || { missing+=("${c%%:*}"); packages+=("${c#*:}"); }
done
command -v createrepo_c > /dev/null || command -v podman > /dev/null ||
	{ missing+=(createrepo_c); packages+=(createrepo_c); }
for m in ${missing[@]+"${missing[@]}"}; do
	say "missing: $m"
done
if [ ${#missing[@]} -gt 0 ]; then
	say "install: sudo dnf install ${packages[*]}"
	[ "$action" = print-deps ] && exit 1
	die "${missing[*]} missing"
fi
[ "$action" = print-deps ] && exit 0
ls "$rpms"/tools/vitrine-guest-tools-*.noarch.rpm > /dev/null 2>&1 ||
	die "no vitrine-guest-tools package in $rpms/tools: run guest/build-rpms.sh tools first"
# The package @1 is built for Fedora $release: its section of components.conf
# has no releases= line, or one that names it (as build-rpms.sh's)
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
# What the section @1 is: its kind= ("" for a package)
kind_of() {
	[ -f "$here/components.conf" ] || return 0
	awk -v p="$1" '
		/^[[:space:]]*\[/ { s = $0; gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", s); next }
		s == p && /^kind=/ { sub(/^kind=/, ""); print; exit }' "$here/components.conf"
}
# The packages @1 is built against: its section's deps= (as build-rpms.sh's)
deps_of() {
	[ -f "$here/components.conf" ] || return 0
	awk -v p="$1" '
		/^[[:space:]]*\[/ { s = $0; gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", s); next }
		s == p && /^deps=/ { sub(/^deps=/, ""); print; exit }' "$here/components.conf"
}
# the packages on the medium: its deps in components.conf (those of an older
# guest/ without the file) that are built for this release
deps=()
read -r -a deps < <(awk '/^[[:space:]]*\[/ { s = $0 }
	s ~ /^[[:space:]]*\[medium\]/ && /^deps=/ { sub(/^deps=/, ""); print; exit }' "$here/components.conf" 2> /dev/null) || true
[ ${#deps[@]} -gt 0 ] || deps=(tools mesa kwin)
medium_packages=()
flatpak_id=
for t in "${deps[@]}"; do
	if [ "$(kind_of "$t")" = flatpak ]; then
		flatpak_id=$t
	elif for_release "$t"; then
		medium_packages+=("$t")
	fi
done
# not while build-rpms.sh replaces the packages, nor build-flatpak.sh its own
exec 9> "$rpms/.lock"
flock -n 9 || die "a build of the guest tools is running into $rpms"
if [ -n "$flatpak_id" ]; then
	if [ -d "$(dirname "$flatpak")" ]; then
		exec 8> "$flatpak.lock"
		flock -s -n 8 || die "a build of the Mesa for Flatpak apps is running into $flatpak"
	fi
	[ -s "$flatpak/.inputs" ] && [ -d "$flatpak/repo" ] ||
		say "warning: no Mesa for Flatpak apps in $flatpak (guest/build-flatpak.sh): Flatpak apps keep Flathub's"
fi
for t in "${medium_packages[@]}"; do
	[ "$t" = tools ] || ls "$rpms/$t"/*.rpm > /dev/null 2>&1 ||
		say "warning: no $t packages in $rpms/$t (guest/build-rpms.sh $t): the medium keeps the guest's own"
done
# a package built against another, with that one's packages of the build it
# was built against (the "dep" line its build recorded): vitrine's KWin does
# not even start without the KWayland it is built against
for t in "${medium_packages[@]}"; do
	ls "$rpms/$t"/*.rpm > /dev/null 2>&1 || continue
	for d in $(deps_of "$t"); do
		[[ " ${medium_packages[*]} " == *" $d "* ]] ||
			die "$t is built against $d, which this medium does not have (the medium's deps= in $here/components.conf)"
		key=$(head -c 64 "$rpms/$d/.inputs" 2> /dev/null || true)
		ls "$rpms/$d"/*.rpm > /dev/null 2>&1 && [ ${#key} = 64 ] &&
			grep -qx "dep $d $key" "$rpms/$t/.inputs.list" 2> /dev/null ||
			die "$t is built against $d, and $rpms/$d has not the build of $d it was built against: guest/build-rpms.sh $d $t"
	done
done

stage=$(mktemp -d)
# the Mesa for Flatpak apps: its OSTree repository, its branches
flatpak_key=
flatpak_branches=()
if [ -n "$flatpak_id" ] && [ -s "$flatpak/.inputs" ] && [ -d "$flatpak/repo" ]; then
	flatpak_key=$(head -c 64 "$flatpak/.inputs")
	while read -r b m _; do
		[ -z "$b" ] || flatpak_branches+=("$b:$m")
	done < "$flatpak/branches"
fi
img=$out.new
manifest=${out%.img}.json.new
# stopped (Ctrl+C, vitrine's Stop): no half-made medium; the last one stays
trap 'rm -rf "$stage"; rm -f "$img" "$manifest"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
mkdir -p "$stage/repo"
# what a guest may install: no debug info, no documentation
for t in "${medium_packages[@]}"; do
	# (set -e: a package not built would end the loop)
	find "$rpms/$t" -maxdepth 1 -name '*.rpm' 2> /dev/null || true
done |
	grep -Ev -- '-(debuginfo|debugsource|doc)-' | while read -r f; do cp "$f" "$stage/repo/"; done
if [ -n "$flatpak_key" ]; then
	say "phase: adding the Mesa for Flatpak apps"
	mkdir -p "$stage/flatpak"
	# not its temporary files, nor its lock
	cp -r "$flatpak/repo" "$stage/flatpak/repo"
	rm -rf "$stage/flatpak/repo/tmp" "$stage/flatpak/repo/.lock"
	cp "$flatpak/branches" "$stage/flatpak/branches"
fi

say "phase: making the repository"
if command -v createrepo_c > /dev/null; then
	createrepo_c --quiet "$stage/repo"
else
	podman run --rm --security-opt label=disable -v "$stage/repo:/repo" 9>&- \
		"registry.fedoraproject.org/fedora:$release" \
		bash -c 'dnf -y -q install createrepo_c > /dev/null && createrepo_c --quiet /repo'
fi
# NAME VERSION-RELEASE ARCH FILE, for the installer
for f in "$stage"/repo/*.rpm; do
	rpm -qp --nosignature --qf "%{NAME} %{VERSION}-%{RELEASE} %{ARCH} $(basename "$f")\n" "$f"
done | sort > "$stage/packages.txt"
evr_of() { awk -v n="$1" '$1 == n { print $2; exit }' "$stage/packages.txt"; }
tools_evr=$(evr_of vitrine-guest-tools)
tools_version=${tools_evr%-*}
kwin_evr=$(evr_of kwin)
kwayland_evr=$(evr_of kwayland)
mesa_evr=$(evr_of mesa-dri-drivers)
kpipewire_evr=$(evr_of kpipewire)
proxy_evr=$(evr_of wl-cross-domain-proxy)
qtbase_evr=$(evr_of qt6-qtbase)
ksvg_evr=$(evr_of kf6-ksvg)
kirigami_evr=$(evr_of kf6-kirigami)
mutter_evr=$(evr_of mutter)

say "phase: adding the installer"
for f in install bootstrap; do
	cp "$here/tools/$f" "$stage/"
done
sed "s/@VERSION@/$tools_version/" "$here/tools/agent/vitrine-agent" > "$stage/vitrine-agent"
# what it changes and why, for whoever opens the medium
# (the packages this medium has: those of another release or not built are
# left out)
join() {  # join WORD...: "a", "a and b", "a, b and c"
	local out=$1
	[ $# -lt 2 ] || { out=$(printf '%s, ' "${@:1:$#-1}"); out="${out%, } and ${!#}"; }
	printf '%s' "$out"
}
locked=() unlocked=()
[ -z "$mesa_evr" ] || locked+=(Mesa)
[ -z "$kwin_evr" ] || locked+=(KWin)
[ -z "$kwayland_evr" ] || locked+=(KWayland)
[ -z "$kpipewire_evr" ] || locked+=(KPipeWire)
[ -z "$proxy_evr" ] || locked+=("the proxy")
{
	cat <<EOF
vitrine guest tools $tools_evr for Fedora $release (x86_64)

vitrine installs them by itself at the guest's next start when you ask for
them. To install them by hand, in a terminal of the guest:

    sudo bash /run/media/\$USER/$label/install

(the path of this medium in the guest's file manager). Remove them with
--uninstall. They need the network: dkms and the kernel headers come from
Fedora's repositories.

What they install:
- vitrine-guest-tools: the virtio-gpu driver with vitrine's patches as a DKMS
  module (rebuilt for each new kernel), its options in
  /usr/lib/modprobe.d/vitrine-virtio-gpu.conf, the KWin settings in
  /usr/lib/environment.d/60-vitrine-kwin.conf, boot-time settings (cacheable
  GPU memory, full preemption, vblank interrupts kept on, a check of the
  driver) and the vitrine agent
EOF
	if [ -n "$mesa_evr" ]; then
		echo "- Mesa $mesa_evr with native context and every video codec (H.264,"
		echo "  H.265 and VC-1 too), in place of the Mesa installed and of RPM Fusion's"
		echo "  freeworld drivers"
	fi
	if [ -n "$kwin_evr" ] && [ -n "$kwayland_evr" ]; then
		echo "- KWin $kwin_evr, only on Plasma ${kwin_evr%%-*} guests, with KWayland $kwayland_evr,"
		echo "  which it is built against (the nested desktop's scrolling)"
	elif [ -n "$kwin_evr" ]; then
		echo "- KWin $kwin_evr, only on Plasma ${kwin_evr%%-*} guests"
	fi
	[ -z "$kpipewire_evr" ] || echo "- KPipeWire $kpipewire_evr, only on Plasma ${kpipewire_evr%%-*} guests"
	if [ -n "$proxy_evr" ]; then
		echo "- the nested desktop: its session, offered in the boots of a VM with the"
		echo "  Nested desktop box, and its proxy, wl-cross-domain-proxy $proxy_evr"
	fi
	if [ -n "$qtbase_evr" ]; then
		echo "- Qt Base $qtbase_evr: Wayland windows being resized reuse their shared"
		echo "  memory"
	fi
	[ -z "$ksvg_evr" ] || echo "- KSvg $ksvg_evr: Plasma's popups no longer upload their frames again at each opening"
	[ -z "$kirigami_evr" ] || echo "- Kirigami $kirigami_evr: the same for the popups of Kirigami's apps and Plasma"
	if [ -n "$mutter_evr" ]; then
		echo "- Mutter $mutter_evr: the first seconds of motion after a new screen mode (the"
		echo "  VM's screen resized, or out of full screen) no longer skip refreshes"
	fi
	if [ ${#flatpak_branches[@]} -gt 0 ]; then
		parts=()
		for e in "${flatpak_branches[@]}"; do
			parts+=("${e#*:} for ${e%%:*}")
		done
		echo "- Mesa for Flatpak apps: vitrine's Mesa with native context in place of"
		echo "  Flathub's GL extension (org.freedesktop.Platform.GL.default) of the runtimes"
		echo "  the guest has, Mesa $(join "${parts[@]}"); for runtimes installed later"
		echo "  too. Flatpak updates keep it; --uninstall brings Flathub's back"
	fi
	following=()
	for e in "$qtbase_evr" "$ksvg_evr" "$kirigami_evr" "$mutter_evr"; do
		[ -z "$e" ] || following+=("$e")
	done
	if [ ${#following[@]} -gt 1 ]; then
		echo "  Each of these in place of the guest's, all its packages, when the guest"
		echo "  has the version it is built from"
	elif [ ${#following[@]} = 1 ]; then
		echo "  In place of the guest's, all its packages, when the guest has the"
		echo "  version it is built from"
	fi
	# with vitrine's KWin, Qt Base, KSvg and Kirigami are locked too, and
	# Plasma and the KDE Frameworks held at their versions (install)
	with=()
	if [ -n "$kwin_evr" ]; then
		[ -z "$qtbase_evr" ] || with+=("Qt Base (which that KWin is built against)")
		[ -z "$ksvg_evr" ] || with+=(KSvg)
		[ -z "$kirigami_evr" ] || with+=(Kirigami)
		with+=("the guest's Plasma")
		[ -z "$ksvg_evr$kirigami_evr" ] || with+=("its KDE Frameworks")
	fi
	if [ ${#locked[@]} -gt 0 ]; then
		kept="are kept at those versions"
		[ ${#locked[@]} -gt 1 ] || kept="is kept at that version"
		if [ ${#with[@]} -gt 0 ]; then
			echo "$(join "${locked[@]}") $kept (dnf versionlock, 64-bit only), and with" \
				"vitrine's KWin so are $(join "${with[@]}"): Fedora's updates of them" \
				"wait for the next guest tools." | fmt -w 78
		else
			echo "$(join "${locked[@]}") $kept (dnf versionlock, 64-bit only)."
		fi
	fi
	[ -z "$qtbase_evr" ] || unlocked+=("Qt Base")
	[ -z "$ksvg_evr" ] || unlocked+=(KSvg)
	[ -z "$kirigami_evr" ] || unlocked+=(Kirigami)
	if [ ${#unlocked[@]} -gt 0 ]; then
		way="give way to Fedora's next build of them"
		[ ${#unlocked[@]} -gt 1 ] || way="gives way to Fedora's next build of it"
		echo "Without vitrine's KWin, $(join "${unlocked[@]}") $way: the next" \
			"guest tools bring vitrine's back." | fmt -w 78
	fi
	if [ -n "$mutter_evr" ]; then
		echo "Mutter gives way to Fedora's next build of it: the next guest tools bring"
		echo "vitrine's back."
	fi
	echo "Secure Boot must be off: the driver is not signed."
} > "$stage/README.txt"

# the medium's identity: what it would install, and how
medium_id=$({ cat "$stage/packages.txt" "$stage/install" "$stage/bootstrap" "$stage/vitrine-agent"
	[ -z "$flatpak_key" ] || echo "flatpak $flatpak_key"; } | sha256sum | cut -c1-16)
cat > "$stage/medium.env" <<EOF
TOOLS_VERSION=$tools_version
TOOLS_EVR=$tools_evr
FEDORA_RELEASE=$release
MEDIUM_ID=$medium_id
KWIN_VERSION=${kwin_evr%%-*}
KWAYLAND_VERSION=${kwayland_evr%%-*}
MESA_VERSION=${mesa_evr%%-*}
KPIPEWIRE_VERSION=${kpipewire_evr%%-*}
QTBASE_VERSION=${qtbase_evr%%-*}
KSVG_VERSION=${ksvg_evr%%-*}
KIRIGAMI_VERSION=${kirigami_evr%%-*}
MUTTER_VERSION=${mutter_evr%%-*}
FLATPAK_GL="${flatpak_branches[*]}"
EOF
packages_json=$(awk '{ printf "%s{\"name\": \"%s\", \"evr\": \"%s\", \"arch\": \"%s\"}", (NR > 1 ? ", " : ""), $1, $2, $3 }' "$stage/packages.txt")
# what it is made from: the inputs build-rpms.sh recorded for each package
# on it, and this script's; other inputs in guest/ make it out of date
inputs_json=
for t in "${medium_packages[@]}"; do
	if ls "$rpms/$t"/*.rpm > /dev/null 2>&1 && [ -s "$rpms/$t/.inputs" ]; then
		inputs_json+="\"$t\": \"$(head -c 64 "$rpms/$t/.inputs")\", "
	fi
done
[ -z "$flatpak_key" ] || inputs_json+="\"$flatpak_id\": \"$flatpak_key\", "
inputs_json+="\"medium\": \"$(sha256sum < "$here/build-medium.sh" | cut -d' ' -f1)\""
cat > "$stage/manifest.json" <<EOF
{
  "label": "$label",
  "mediumId": "$medium_id",
  "fedora": "$release",
  "tools": "$tools_evr",
  "toolsVersion": "$tools_version",
  "mesa": "$mesa_evr",
  "kwin": "$kwin_evr",
  "kwayland": "$kwayland_evr",
  "kpipewire": "$kpipewire_evr",
  "proxy": "$proxy_evr",
  "qtbase": "$qtbase_evr",
  "ksvg": "$ksvg_evr",
  "kirigami": "$kirigami_evr",
  "mutter": "$mutter_evr",
  "flatpak": "${flatpak_branches[*]}",
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "inputs": {$inputs_json},
  "packages": [$packages_json]
}
EOF

say "phase: writing the image"
# the payload, plus room for the FAT and the directories
kib=$(( $(du -sk "$stage" | cut -f1) * 11 / 10 + 8192 ))
mkdir -p "$(dirname "$out")"
rm -f "$img"
mkfs.fat -C -n "$label" -i "${medium_id:0:8}" "$img" "$kib" > /dev/null
mcopy -s -m -i "$img" "$stage"/* ::/
# replaced in one step: a running VM keeps the image it opened; the
# manifest right after it
cp "$stage/manifest.json" "$manifest"
mv -f "$img" "$out"
mv -f "$manifest" "${out%.img}.json"
echo
echo "$out: $((kib / 1024)) MiB, tools $tools_evr, medium $medium_id"
echo "  Mesa: ${mesa_evr:-none}   KWin: ${kwin_evr:-none}   KWayland: ${kwayland_evr:-none}"
echo "  KPipeWire: ${kpipewire_evr:-none}   proxy: ${proxy_evr:-none}"
echo "  Qt Base: ${qtbase_evr:-none}   KSvg: ${ksvg_evr:-none}   Kirigami: ${kirigami_evr:-none}"
echo "  Mutter: ${mutter_evr:-none}   Flatpak: ${flatpak_branches[*]:-none}"
say "built: $out"
