#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds Vitrine's QEMU: virglrenderer and the qemu-gui fork, each fetched
# at the commit versions.conf pins and patched with patches/<component>/*.patch
# in name order, installed into a prefix of their own:
#
#   <stack>/<stamp>/bin/qemu-system-x86_64, qemu-img and QEMU's other tools
#   <stack>/<stamp>/lib64/libvirglrenderer.so.1
#   <stack>/<stamp>/share/qemu/               firmware, keymaps, qemu-options.hx
#   <stack>/<stamp>/share/vitrine/stack.conf  what was built, written last
#   <stack>/current -> <stamp>                flipped once a build is complete
#
# <stamp> sums up the inputs (versions.conf, the patches, this script): other
# inputs build into another prefix, and the VMs running keep theirs.  Inputs
# built already leave nothing to do.  The sources and build trees stay in
# the work folder, and are patched and configured again only when their
# inputs change, so that an update compiles only what it has to.
#
#   build.sh [--deps|--print-deps|--print-stamp] [--stack DIR|--prefix DIR] [--work DIR] [-j N]
#
#   --deps         install the build dependencies first (Fedora, sudo dnf)
#   --print-deps   tell the missing build dependencies, then stop (status 1
#                  if something is missing)
#   --print-stamp  print the stamp of the inputs, then stop
#   --stack DIR    where the prefixes and `current` go
#                  [${XDG_DATA_HOME:-~/.local/share}/vitrine/stack]
#   --prefix DIR   install into DIR itself instead: no stamp folder, no `current`
#   --work DIR     sources and build trees [${XDG_CACHE_HOME:-~/.cache}/vitrine/stack-build]
#   -j N           parallel jobs [nproc]
#
# QEMU gets an absolute RUNPATH to <prefix>/lib64, never $ORIGIN: once QEMU
# has file capabilities (cap_sys_nice), the loader ignores LD_LIBRARY_PATH
# and $ORIGIN, and would load the system's virglrenderer.
#
# Lines starting with "vitrine-build: " are for the app:
#   vitrine-build: step N/M: <what>
#   vitrine-build: missing: <dependency>
#   vitrine-build: install: <the command installing what is missing>
#   vitrine-build: warning: <message>
#   vitrine-build: up to date: <prefix>
#   vitrine-build: built: <prefix>
#   vitrine-build: error: <message>
set -euo pipefail
shopt -s nullglob

here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
data_home=${XDG_DATA_HOME:-$HOME/.local/share}
cache_home=${XDG_CACHE_HOME:-$HOME/.cache}

usage() { sed -n '/^#   build.sh/,/^#   -j N/p' "$here/build.sh" | sed 's/^# \{0,3\}//'; }
say() { echo "vitrine-build: $*"; }
died=0
die() { say "error: $*"; died=1; exit 1; }

# --- arguments ----------------------------------------------------------------
action=build
stack=$data_home/vitrine/stack
prefix=
work=$cache_home/vitrine/stack-build
jobs=$(nproc)
while [ $# -gt 0 ]; do
	case "$1" in
	--deps) action=deps ;;
	--print-deps) action=print-deps ;;
	--print-stamp) action=print-stamp ;;
	--stack) stack=${2:?--stack needs a folder}; shift ;;
	--stack=*) stack=${1#*=} ;;
	--prefix) prefix=${2:?--prefix needs a folder}; shift ;;
	--prefix=*) prefix=${1#*=} ;;
	--work) work=${2:?--work needs a folder}; shift ;;
	--work=*) work=${1#*=} ;;
	-j) jobs=${2:?-j needs a number}; shift ;;
	-j*) jobs=${1#-j} ;;
	-h|--help) usage; exit 0 ;;
	*) echo "unknown argument $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "-j takes a number of jobs, not $jobs"

# --- inputs -------------------------------------------------------------------
# versions.conf: NAME=value lines, read rather than sourced
conf() { sed -n "s/^[[:space:]]*$1=\(.*\)$/\1/p" "$here/versions.conf" | tail -n 1 | sed 's/[[:space:]]*$//'; }
QEMU_URL=$(conf QEMU_URL)
QEMU_COMMIT=$(conf QEMU_COMMIT)
VIRGL_URL=$(conf VIRGL_URL)
VIRGL_COMMIT=$(conf VIRGL_COMMIT)
for v in QEMU_URL QEMU_COMMIT VIRGL_URL VIRGL_COMMIT; do
	[ -n "${!v}" ] || die "versions.conf has no $v"
done

# the patches of a component, in name order whatever the locale
patches_of() { (cd "$here" && printf '%s\n' patches/"$1"/*.patch | sed '/^$/d' | LC_ALL=C sort); }
mapfile -t qemu_patches < <(patches_of qemu)
mapfile -t virgl_patches < <(patches_of virglrenderer)

# The stamp: the sha256 of sha256sum's lines for the inputs, in this order.
# The app computes the same to tell when the build is out of date.
stamp=$(cd "$here" && sha256sum versions.conf build.sh "${qemu_patches[@]}" "${virgl_patches[@]}" |
	sha256sum | cut -c1-64)
if [ "$action" = print-stamp ]; then
	echo "$stamp"
	exit 0
fi

# --- dependencies -------------------------------------------------------------
# Fedora's package names; what the build checks for is below (commands and
# pkg-config modules), so that other distributions can map them
fedora=(git gcc gcc-c++ make meson ninja-build pkgconf-pkg-config python3 python3-pyyaml binutils
	util-linux glib2-devel pixman-devel zlib-devel libslirp-devel SDL2-devel libepoxy-devel
	mesa-libgbm-devel mesa-libEGL-devel libdrm-devel libva-devel libusb1-devel
	pulseaudio-libs-devel pipewire-devel spice-protocol libzstd-devel libpng-devel
	libcap-ng-devel libattr-devel wayland-devel wayland-protocols-devel bzip2)
missing=()
need_cmd() { local c; for c in "$@"; do command -v "$c" > /dev/null 2>&1 || missing+=("command $c"); done; }
need_pc() { local m; for m in "$@"; do pkg-config --exists "$m" 2> /dev/null || missing+=("pkg-config $m"); done; }
check_deps() {
	missing=()
	need_cmd git gcc g++ make meson ninja python3 pkg-config readelf ldd flock sha256sum
	# virglrenderer
	need_pc epoxy gbm egl libdrm libva
	python3 -c 'import yaml' 2> /dev/null || missing+=("python3 module yaml")
	# QEMU
	need_pc glib-2.0 gio-unix-2.0 pixman-1 zlib slirp sdl2 libusb-1.0 libpulse libpipewire-0.3 \
		spice-protocol libzstd libpng libcap-ng wayland-client
	need_cmd wayland-scanner gdbus-codegen
	# QEMU's configure requires it (install_blobs, on by default): the EDK2
	# firmware it installs comes compressed
	need_cmd bzip2
}
tell_missing() {
	local m
	[ ${#missing[@]} -gt 0 ] || return 0
	for m in "${missing[@]}"; do say "missing: $m"; done
	if command -v dnf > /dev/null 2>&1; then
		say "install: sudo dnf install ${fedora[*]}"
	fi
}
check_deps
case "$action" in
print-deps)
	tell_missing
	exit $((${#missing[@]} > 0))
	;;
deps)
	command -v dnf > /dev/null 2>&1 || die "--deps knows Fedora's dnf only: install the equivalents of what --print-deps lists"
	echo "== installing the build dependencies (sudo dnf install)"
	sudo dnf install -y "${fedora[@]}"
	check_deps
	;;
esac

# --- where --------------------------------------------------------------------
# a stamp folder of the stack, or the prefix given
if [ -n "$prefix" ]; then
	mkdir -p "$prefix"
	prefix=$(cd "$prefix" && pwd)
	name=
else
	mkdir -p "$stack"
	stack=$(cd "$stack" && pwd)
	name=${stamp:0:16}
	prefix=$stack/$name
fi
mkdir -p "$work"
work=$(cd "$work" && pwd)
manifest=$prefix/share/vitrine/stack.conf

# one build at a time in a work folder, and in a stack
exec 8> "$work/.lock"
flock -n 8 || die "another build is running in $work"
if [ -n "$name" ]; then
	exec 9> "$stack/.lock"
	flock -n 9 || die "another build is running for $stack"
fi

flip_current() {
	[ -n "$name" ] || return 0
	[ "$(readlink "$stack/current" 2> /dev/null)" = "$name" ] && return 0
	ln -sfn "$name" "$stack/.current.new"
	mv -Tf "$stack/.current.new" "$stack/current"
}

if [ -f "$manifest" ] && grep -qx "STAMP=$stamp" "$manifest"; then
	flip_current
	say "up to date: $prefix"
	exit 0
fi

# A stamp folder without its manifest is what a build that failed or was
# stopped left: it starts again from nothing.  A prefix given is the
# caller's: installed over, never removed.
partial=
if [ -n "$name" ]; then
	rm -rf "${prefix:?}"
	mkdir -p "$prefix"
	partial=$prefix
fi
step=
on_exit() {
	local rc=$?
	if [ "$rc" -ne 0 ]; then
		[ -z "$partial" ] || rm -rf "$partial"
		[ "$died" = 1 ] || say "error: ${step:-the build} failed (status $rc)"
	fi
}
trap on_exit EXIT
# stopped (the app's Stop signals the whole process group): the exit above runs
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
steps=8
n=0
begin() { n=$((n + 1)); step=$1; say "step $n/$steps: $1"; }

# --- 1. dependencies ----------------------------------------------------------
begin "checking the build dependencies"
if [ ${#missing[@]} -gt 0 ]; then
	tell_missing
	die "build dependencies are missing (build.sh --deps installs them on Fedora)"
fi
command -v passt > /dev/null 2>&1 ||
	say "warning: no passt on this host: VMs fall back to QEMU's user-mode network (Fedora: dnf install passt)"

# --- sources ------------------------------------------------------------------
# no credential prompts (a desktop's askpass dialog) for public repositories
export GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=
git() { command git -c credential.helper= -c advice.detachedHead=false "$@"; }

# checkout DIR URL COMMIT PATCH...: DIR at COMMIT with the patches applied,
# left alone when it is already (DIR.stamp: the commit and the patches'
# sha256): rewriting the files would make the build compile them again
checkout() {
	local d=$1 url=$2 commit=$3 p sums
	shift 3
	sums=$(cd "$here" && cat "$@" < /dev/null | sha256sum | cut -c1-64)
	if [ -d "$d/.git" ] && [ "$(cat "$d.stamp" 2> /dev/null)" = "$commit $sums" ]; then
		echo "$(basename "$d") at ${commit:0:12} with $# patches already"
		return 0
	fi
	rm -f "$d.stamp"
	echo "$(basename "$d"): $url at ${commit:0:12}, $# patches"
	if [ ! -d "$d/.git" ]; then
		rm -rf "$d"
		mkdir -p "$d"
		git -C "$d" init -q
		git -C "$d" remote add origin "$url"
	fi
	git -C "$d" remote set-url origin "$url"
	git -C "$d" cat-file -e "$commit^{commit}" 2> /dev/null ||
		git -C "$d" fetch --depth 1 origin "$commit" ||
		die "cannot fetch $commit from $url (no network, or the commit is gone from the repository)"
	git -C "$d" checkout -q -f --detach "$commit"
	git -C "$d" clean -q -fdx
	for p in "$@"; do
		echo "applying $p"
		git -C "$d" apply --whitespace=nowarn "$here/$p" || die "$p does not apply to ${commit:0:12}"
	done
	echo "$commit $sums" > "$d.stamp"
}

# configured DIR LINE: whether build tree DIR was configured with LINE
configured() { [ -f "$1/.vitrine-configure" ] && [ "$(cat "$1/.vitrine-configure")" = "$2" ]; }

src=$work/src
mkdir -p "$src"

# --- 2-3. virglrenderer -------------------------------------------------------
begin "fetching virglrenderer"
checkout "$src/virglrenderer" "$VIRGL_URL" "$VIRGL_COMMIT" "${virgl_patches[@]}"

begin "building virglrenderer"
vbuild=$work/virglrenderer-build
vargs=(--prefix="$prefix" --libdir=lib64 --buildtype=debugoptimized
	-Ddrm-renderers=amdgpu-experimental,xe-experimental -Dvideo=true -Dvenus=false)
vline="$src/virglrenderer ${vargs[*]}"
# configured afresh when the options change: the prefix does with each new stamp
if [ ! -f "$vbuild/build.ninja" ] || ! configured "$vbuild" "$vline"; then
	rm -rf "$vbuild"
	meson setup "$vbuild" "$src/virglrenderer" "${vargs[@]}"
	echo "$vline" > "$vbuild/.vitrine-configure"
fi
ninja -C "$vbuild" -j "$jobs"
meson install -C "$vbuild" --no-rebuild --quiet
[ -f "$prefix/lib64/pkgconfig/virglrenderer.pc" ] || die "virglrenderer installed no virglrenderer.pc"

# --- 4-7. QEMU ----------------------------------------------------------------
begin "fetching QEMU"
checkout "$src/qemu" "$QEMU_URL" "$QEMU_COMMIT" "${qemu_patches[@]}"

begin "configuring QEMU"
qbuild=$work/qemu-build
qargs=(--prefix="$prefix" --target-list=x86_64-softmmu --without-default-features --disable-werror
	--enable-kvm --enable-tcg --enable-pixman --enable-attr --enable-virtfs --enable-hmp
	--enable-malloc-trim --enable-sdl --enable-sdl-gui --enable-opengl --enable-virglrenderer
	--enable-libusb --enable-pa --enable-pipewire --enable-spice-protocol --enable-passt --enable-gio
	--enable-dbus-display --enable-slirp --enable-tpm --enable-vhost-kernel --enable-vhost-net
	--enable-vhost-user --enable-zstd --enable-png --enable-tools --enable-fdt=internal --disable-docs
	--disable-plugins --disable-containers --container-command=no
	"--extra-ldflags=-Wl,-rpath,$prefix/lib64")
# (no containers: configure would otherwise run podman to see if it works, which
# sets up podman's storage in the user's home, for cross-builds we never do)
qline="$src/qemu ${qargs[*]} PKG_CONFIG_PATH=$prefix/lib64/pkgconfig"
if [ -f "$qbuild/build.ninja" ] && configured "$qbuild" "$qline"; then
	echo "configured already"
else
	rm -rf "$qbuild"
	mkdir -p "$qbuild"
	(cd "$qbuild" && PKG_CONFIG_PATH="$prefix/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
		"$src/qemu/configure" "${qargs[@]}")
	echo "$qline" > "$qbuild/.vitrine-configure"
fi
grep -q "define HAVE_VIRGL_RESOURCE_SET_GUEST_DMABUF" "$qbuild/config-host.h" ||
	say "warning: QEMU is built without virglrenderer's guest dma-buf call: no zero copy of guest dma-bufs"

begin "compiling QEMU"
make -C "$qbuild" -j "$jobs"

begin "installing QEMU"
make -C "$qbuild" install
# QEMU installs the UEFI firmware of every architecture it knows, some 200 MB:
# the VMs are x86_64 ones
rm -f "$prefix"/share/qemu/edk2-{aarch64,arm,riscv64,loongarch64}-* \
	"$prefix"/share/qemu/firmware/*-edk2-{aarch64,arm,riscv64,loongarch64}*.json
# the full option descriptions, for the app's QEMU reference
install -m 0644 "$src/qemu/qemu-options.hx" "$prefix/share/qemu/qemu-options.hx"

# --- 8. check -----------------------------------------------------------------
begin "checking the installation"
qemu=$prefix/bin/qemu-system-x86_64
[ -x "$qemu" ] && [ -x "$prefix/bin/qemu-img" ] || die "qemu-system-x86_64 or qemu-img was not installed"
# meson removes the run paths it adds for the build when it installs; ours must stay
runpath=$(readelf -d "$qemu" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]$/\1/p')
case ":$runpath:" in
*'$ORIGIN'*) die "the RUNPATH of qemu-system-x86_64 uses \$ORIGIN: $runpath" ;;
*":$prefix/lib64:"*) echo "RUNPATH $runpath" ;;
*) die "qemu-system-x86_64 has no RUNPATH to $prefix/lib64 (it has: ${runpath:-none})" ;;
esac
# what the loader finds, without what the environment adds; and with
# modules, the virtio-gpu-gl one is what loads virglrenderer
for f in "$qemu" "$prefix"/lib*/qemu/hw-display-virtio-gpu-gl.so; do
	lib=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD ldd "$f" | sed -n 's/^[[:space:]]*libvirglrenderer\.so[.0-9]* => \([^ ]*\).*/\1/p')
	[ "$lib" = "$prefix/lib64/libvirglrenderer.so.1" ] ||
		die "$(basename "$f") loads ${lib:-no virglrenderer} instead of $prefix/lib64/libvirglrenderer.so.1"
	echo "$(basename "$f") loads $lib"
done
version=$("$qemu" -version | sed -n 's/^QEMU emulator version \([^ ]*\).*/\1/p')
[ -n "$version" ] || die "qemu-system-x86_64 -version failed"
"$prefix/bin/qemu-img" --version > /dev/null || die "qemu-img --version failed"
props=$("$qemu" -device virtio-gpu-gl-pci,help)
for p in drm_native_context x-host-vblank; do
	grep -q "^ *$p=" <<< "$props" || die "virtio-gpu-gl-pci has no $p property: not the patched fork"
done
echo "QEMU $version, virtio-gpu-gl-pci with drm_native_context and x-host-vblank"

# what was built, written last: a prefix with it is complete
mkdir -p "$(dirname "$manifest")"
patch_names() { local p; for p in "$@"; do printf '%s ' "$(basename "$p")"; done; }
cat > "$manifest.new" << EOF
# What host/build.sh built here
STAMP=$stamp
BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
QEMU_URL=$QEMU_URL
QEMU_COMMIT=$QEMU_COMMIT
QEMU_VERSION=$version
QEMU_PATCHES=$(patch_names "${qemu_patches[@]}")
VIRGL_URL=$VIRGL_URL
VIRGL_COMMIT=$VIRGL_COMMIT
VIRGL_VERSION=$(PKG_CONFIG_PATH=$prefix/lib64/pkgconfig pkg-config --modversion virglrenderer)
VIRGL_PATCHES=$(patch_names "${virgl_patches[@]}")
EOF
mv -f "$manifest.new" "$manifest"
partial=
flip_current
say "built: $prefix"
