#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Our KDE builds for one machine, in a fedora container with that machine's repositories (the KDE SIG's
# beta COPR for PLASMA=beta): for each package of the role (setup/versions.sh), the source RPM of its
# newest version there + our patch set for that version (kde_set_for), built with build-patched-rpm.sh.
# RPMs in OUTROOT/PACKAGE/; a package built already is kept (VGD_REBUILD=1 builds it again).
# The build's logs and its buildroot's package list (builddep-TAG.log, build-TAG.log, buildroot-TAG.txt) go
# next to the RPMs; while it runs they grow in $VGD_BUILD/kde-ROLE-build/out/PACKAGE/.
#   kde-rpms.sh host|guest OUTROOT        PLASMA=beta|testing|fedora (default beta; testing: Fedora's
#                                         updates-testing), VGD_KDE_PACKAGES: some of the role's packages only
. "$(dirname -- "${BASH_SOURCE[0]}")/../lib.sh"
role=${1:?host or guest} outroot=${2:?output folder}
plasma=${PLASMA:-beta}
case $role in host) all=$HOST_KDE_PACKAGES ;; guest) all=$GUEST_KDE_PACKAGES ;; *) die "host or guest" ;; esac
if [ -n "${VGD_KDE_PACKAGES:-}" ]; then
	for p in $VGD_KDE_PACKAGES; do case " $all " in *" $p "*) ;; *) die "$p is not a $role package ($all)" ;; esac; done
	all=$VGD_KDE_PACKAGES
fi
pkgs=
for p in $all; do
	if [ "${VGD_REBUILD:-0}" != 1 ] && ls "$outroot/$p"/*.rpm > /dev/null 2>&1; then
		say "$role $p is built: $(cd "$outroot/$p" && ls "$p"-[0-9]*.rpm | head -1) (VGD_REBUILD=1 builds it again)"
	else
		pkgs+=" $p"
	fi
done
[ -n "$pkgs" ] || exit 0
need podman
work=$VGD_BUILD/kde-$role-build
rm -rf "$work"
mkdir -p "$work/out"
case $plasma in
beta) copr="dnf -y -q copr enable $GUEST_PLASMA_COPR > /work/dnf.log 2>&1 || { tail /work/dnf.log; exit 1; }" ;;
testing) copr="dnf -y -q config-manager setopt updates-testing.enabled=1 > /work/dnf.log 2>&1 || { tail /work/dnf.log; exit 1; }" ;;
fedora) copr= ;;
*) die "PLASMA: beta, testing or fedora" ;;
esac
say "$role KDE builds ($pkgs ) in a fedora:$FEDORA container, Plasma: $plasma (logs: $work)"
fedora_container "kde-$role" "
	dnf -y -q install rpm-build dnf5-plugins cpio python3 > /work/dnf.log 2>&1 || { tail -20 /work/dnf.log; exit 1; }
	$copr
	. /repo/setup/versions.sh
	for pkg in $pkgs; do
		mkdir -p /work/\$pkg /work/out/\$pkg && cd /work/\$pkg
		dnf -y -q download --srpm \$pkg > /work/\$pkg/dnf.log 2>&1 || { tail -20 /work/\$pkg/dnf.log; exit 1; }
		srpm=\$(ls \$pkg-[0-9]*.src.rpm | sort -V | tail -1)
		ver=\$(rpm -qp --qf '%{VERSION}' \"\$srpm\" 2> /dev/null) || { echo \"cannot read \$srpm\"; exit 1; }
		sel=\$(kde_set_for $role \$pkg \$ver) || { echo \"no $role patch set for \$pkg \$ver (setup/versions.sh)\"; exit 1; }
		read -r series tag excluded <<< \"\$sel\"
		echo \"\$srpm -> \$series as \$tag\${excluded:+ without \$excluded}\"
		mkdir -p set
		i=0
		while read -r p; do
			[ -n \"\$p\" ] || continue
			i=\$((i + 1))
			for x in \$excluded; do case \$p in \"\$x\"*) echo \"  left out: \$p\"; continue 2 ;; esac; done
			cp \"/repo/patches/\$(kde_patch_dir \$pkg)/\$p\" \"set/\$(printf %03d \$i)-\$p\"
		done < \"/repo/patches/\$(kde_patch_dir \$pkg)/sets/\$series.series\"
		bash /repo/setup/tools/build-patched-rpm.sh \"\$srpm\" set \"\$tag\" /work/out/\$pkg
	done
" -v "$VGD_REPO:/repo:ro" -v "$work:/work"
for p in $pkgs; do
	ls "$work/out/$p"/*.rpm > /dev/null 2>&1 || die "no $p RPM came out (logs: $work/out/$p)"
	rm -rf "${outroot:?}/${p:?}"
	mkdir -p "$outroot/$p"
	mv "$work/out/$p"/*.rpm "$outroot/$p/"
	for f in "$work/out/$p"/buildroot-*.txt "$work/out/$p"/builddep-*.log "$work/out/$p"/build-*.log; do
		[ -e "$f" ] && mv "$f" "$outroot/$p/"
	done
	say "$role $p: $(cd "$outroot/$p" && ls | tr '\n' ' ')"
done
