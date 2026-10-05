#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs inside the Fedora container of build-rpms.sh: build one package from
# its spec and the files beside it in /src/PACKAGE, put the binary RPMs in /out.
# Its "vitrine-build: phase: " lines tell vitrine what it does (build-rpms.sh).
set -eu
t=$1
say() { echo "vitrine-build: $*"; }
case "$t" in
mesa) dir=/src/mesa; spec=mesa.spec ;;
kwin) dir=/src/kwin; spec=kwin.spec ;;
tools) dir=/src/tools; spec=vitrine-guest-tools.spec ;;
*) echo "unknown package $t" >&2; exit 2 ;;
esac
dnf=(dnf -y --setopt=keepcache=True --setopt=install_weak_deps=False)
say "phase: installing rpmbuild"
"${dnf[@]}" install rpm-build rpmdevtools 'dnf5-command(builddep)'

top=/root/rpmbuild
mkdir -p "$top/SOURCES" "$top/SPECS"
cp "$dir/$spec" "$top/SPECS/"
spec=$top/SPECS/$spec
if [ "$t" = tools ]; then
	# the whole directory is the one source tarball
	version=$(rpmspec -q --srpm --qf '%{VERSION}' "$spec")
	tar -C /src --transform "s|^tools|vitrine-guest-tools-$version|" \
		-czf "$top/SOURCES/vitrine-guest-tools-$version.tar.gz" tools
else
	# the patches, then the tarballs the spec names (kept in /sources)
	find "$dir" -maxdepth 1 -type f ! -name '*.spec' -exec cp {} "$top/SOURCES/" \;
fi
# the build requirements first: they bring the macros some Source URLs use (KWin's kf6-rpm-macros)
say "phase: installing the build dependencies"
"${dnf[@]}" builddep "$spec"
if [ "$t" != tools ]; then
	say "phase: downloading the sources"
	spectool -g -C /sources "$spec"
	cp -n /sources/* "$top/SOURCES/" 2> /dev/null || true
fi

defs=(--define "_smp_build_ncpus ${JOBS:-$(nproc)}"
      # SELinux is not available in a container, and Fedora's linkdupes step then fails
      --define '__brp_linkdupes /usr/bin/true')
[ "${DEBUGINFO:-0}" = 1 ] || defs+=(--define 'debug_package %{nil}')
# the build requirements the spec generates (Mesa's Rust crates): rpmbuild -br
# writes them to a .buildreqs.nosrc.rpm (and exits 11 while some are missing);
# it unpacks and patches the sources first
say "phase: preparing the sources"
for _ in 1 2 3; do
	rm -f "$top"/SRPMS/*.buildreqs.nosrc.rpm
	rc=0; rpmbuild -br "${defs[@]}" "$spec" > /dev/null || rc=$?
	[ "$rc" = 11 ] || break
	say "phase: installing the build dependencies"
	"${dnf[@]}" builddep "$top"/SRPMS/*.buildreqs.nosrc.rpm
done
# rpmbuild's "Executing(%build)" and such tell the parts of this one
say "phase: building the packages"
rpmbuild -bb "${defs[@]}" "$spec"
find "$top/RPMS" -name '*.rpm' -exec cp {} /out/ \;
echo "built: $(find "$top/RPMS" -name '*.rpm' | wc -l) packages"
