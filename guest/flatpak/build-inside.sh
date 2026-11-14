#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# In build-flatpak.sh's container (freedesktop-sdk's BuildStream image, as
# root: the user outside), for one runtime branch: freedesktop-sdk at COMMIT
# in /src, Mesa's patches and options, the build, and its export as
# org.freedesktop.Platform.GL.default//BRANCH and //BRANCH-extra into the
# OSTree repository /out/repo.  /guest is guest/ (read-only), /root/.cache
# BuildStream's cache.
#   build-inside.sh BRANCH COMMIT IMAGE-ID MESA JOBS PATCH...
# PATCH: by its path in guest/, in the order they apply.
set -euo pipefail
branch=$1 commit=$2 image_id=$3 mesa=$4 jobs=$5
shift 5
target=flatpak-images/mesa-extra.bst
say() { echo "vitrine-build: $*"; }
die() { say "error: $*" >&2; exit 1; }

say "phase: fetching freedesktop-sdk $branch"
cd /src
if [ ! -d .git ]; then
	git init -q .
	git remote add origin https://gitlab.com/freedesktop-sdk/freedesktop-sdk.git
fi
git fetch -q --depth 1 origin "$commit"
git checkout -q -f FETCH_HEAD
git clean -q -fdx
# the image of its CI: another one may not build it as the cache has it
ci=$(sed -n 's/^ *DOCKER_IMAGE_ID: *"\{0,1\}\([0-9a-f]*\)"\{0,1\} *$/\1/p' .gitlab-ci.yml | head -n 1)
[ "$ci" = "$image_id" ] ||
	die "freedesktop-sdk's CI builds release/$branch at ${commit:0:12} in the image bst2:${ci:-?}, not bst2:$image_id: set image=bst2:${ci:-?} in guest/flatpak/branches.conf"

say "phase: patching Mesa $mesa"
sources=elements/extensions/mesa/mesa-sources.yml
options=elements/extensions/mesa/mesa.yml
have=$(sed -n 's/^  ref: mesa-\([0-9.]*\)-0-g.*/\1/p' "$sources" | head -n 1)
[ "$have" = "$mesa" ] ||
	die "freedesktop-sdk's release/$branch at ${commit:0:12} builds Mesa ${have:-?}, not $mesa: guest/flatpak/branches.conf has patches for $mesa"
mkdir -p patches/mesa
for p in "$@"; do
	name=$(basename "$p")
	[ ! -e "patches/mesa/$name" ] || die "two patches named $name"
	cp "/guest/$p" "patches/mesa/$name"
	printf -- '- kind: patch\n  path: patches/mesa/%s\n' "$name" >> "$sources"
done
# the native contexts: Intel's (Xe, i915) and AMD's, on in place of what the
# recipe says, else after its -Dteflon=true
grep -q -- '^ *-Dteflon=true$' "$options" || die "$options has no -Dteflon=true line to add Mesa's options after"
for opt in intel-virtio-experimental amdgpu-virtio; do
	if grep -q -- "-D$opt=" "$options"; then
		sed -i "s/-D$opt=[^ ]*/-D$opt=true/" "$options"
	else
		sed -i "s/^\( *\)-Dteflon=true$/&\n\1-D$opt=true/" "$options"
	fi
done
git status --short

# all but Mesa from freedesktop-sdk's cache, as its CI built them: else the
# build would be the whole runtime's, hours long
say "phase: looking for freedesktop-sdk's builds of $branch"
show=$(bst --no-colors artifact show --deps all "$target")
missing=$(printf '%s\n' "$show" | awk '/^ *not cached / { print $NF }' |
	grep -v -e '^extensions/mesa/' -e '^flatpak-images/mesa' || true)
if [ -n "$missing" ]; then
	echo "not built in freedesktop-sdk's cache:"
	printf '  %s\n' $missing
	die "freedesktop-sdk's cache no longer has its builds of release/$branch at ${commit:0:12} ($(printf '%s\n' $missing | wc -l) missing, $(printf '%s\n' $missing | head -n 1) first): move guest/flatpak/branches.conf to a newer commit of it"
fi

say "phase: building Mesa $mesa for $branch"
bst --no-colors --max-jobs "$jobs" build --retry-failed "$target"
rm -rf "/out/tree-$branch"
bst --no-colors artifact checkout "$target" --directory "/out/tree-$branch"
cat "/out/tree-$branch/metadata"

say "phase: exporting $branch"
# stamped now: flatpak replaces a commit only with a later one
for b in "$branch" "$branch-extra"; do
	flatpak build-export --runtime --timestamp=NOW \
		--subject="vitrine's Mesa $mesa (Xe and AMD native contexts) as GL.default//$b" \
		/out/repo "/out/tree-$branch" "$b"
done
rm -rf "/out/tree-$branch"
