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
# In:  $RPMS_DIR [~/.local/share/vitrine/guest-tools/rpms/fcRELEASE]/
#      {tools,mesa,kwin}/*.rpm (tools needed; without mesa or kwin the medium
#      installs the guest's own)
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
# not while build-rpms.sh replaces the packages
exec 9> "$rpms/.lock"
flock -n 9 || die "a build of the guest tools is running into $rpms"
for t in mesa kwin; do
	ls "$rpms/$t"/*.rpm > /dev/null 2>&1 ||
		say "warning: no $t packages in $rpms/$t (guest/build-rpms.sh $t): the medium keeps the guest's own"
done

stage=$(mktemp -d)
img=$out.new
manifest=${out%.img}.json.new
# stopped (Ctrl+C, vitrine's Stop): no half-made medium; the last one stays
trap 'rm -rf "$stage"; rm -f "$img" "$manifest"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
mkdir -p "$stage/repo"
# what a guest may install: no debug info, no documentation
find "$rpms"/tools "$rpms"/mesa "$rpms"/kwin -maxdepth 1 -name '*.rpm' 2> /dev/null |
	grep -Ev -- '-(debuginfo|debugsource|doc)-' | while read -r f; do cp "$f" "$stage/repo/"; done

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
mesa_evr=$(evr_of mesa-dri-drivers)

say "phase: adding the installer"
for f in install bootstrap; do
	cp "$here/tools/$f" "$stage/"
done
sed "s/@VERSION@/$tools_version/" "$here/tools/agent/vitrine-agent" > "$stage/vitrine-agent"
# what it changes and why, for whoever opens the medium
cat > "$stage/README.txt" <<EOF
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
- Mesa ${mesa_evr:-(none on this medium)} with native context, in place of the Mesa installed
- KWin ${kwin_evr:-(none on this medium)}, only on Plasma ${kwin_evr%%-*} guests
Mesa and KWin are kept at those versions (dnf versionlock, 64-bit only).
Secure Boot must be off: the driver is not signed.
EOF

# the medium's identity: what it would install, and how
medium_id=$(cat "$stage/packages.txt" "$stage/install" "$stage/bootstrap" "$stage/vitrine-agent" |
	sha256sum | cut -c1-16)
cat > "$stage/medium.env" <<EOF
TOOLS_VERSION=$tools_version
TOOLS_EVR=$tools_evr
FEDORA_RELEASE=$release
MEDIUM_ID=$medium_id
KWIN_VERSION=${kwin_evr%%-*}
MESA_VERSION=${mesa_evr%%-*}
EOF
packages_json=$(awk '{ printf "%s{\"name\": \"%s\", \"evr\": \"%s\", \"arch\": \"%s\"}", (NR > 1 ? ", " : ""), $1, $2, $3 }' "$stage/packages.txt")
# what it is made from: the inputs build-rpms.sh recorded for each package
# on it, and this script's; other inputs in guest/ make it out of date
inputs_json=
for t in tools mesa kwin; do
	if ls "$rpms/$t"/*.rpm > /dev/null 2>&1 && [ -s "$rpms/$t/.inputs" ]; then
		inputs_json+="\"$t\": \"$(head -c 64 "$rpms/$t/.inputs")\", "
	fi
done
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
echo "  Mesa: ${mesa_evr:-none}   KWin: ${kwin_evr:-none}"
say "built: $out"
