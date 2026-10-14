#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds Vitrine's QEMU: virglrenderer and upstream QEMU, each fetched at
# the commit versions.conf pins and patched with the *.patch files of
# patches/<component> and of its folders, or those its series file lists
# (see patches_of), each installed into a folder of its own:
#
#   <stack>/virglrenderer/<key>/lib64/libvirglrenderer.so.1
#   <stack>/<stamp>/bin/qemu-system-x86_64, qemu-img and QEMU's other tools
#   <stack>/<stamp>/share/qemu/               firmware, keymaps, qemu-options.hx
#   <stack>/<stamp>/share/vitrine/stack.conf  what was built, written last
#   <stack>/current -> <stamp>                flipped once a build is complete
#
# A component's key sums up its inputs (its source, its patches, this
# script, the keys of the components it is built against: key_lines) and
# names its folder; QEMU's is the stamp.  Inputs built already leave
# nothing to do; other inputs build into another folder, and the VMs
# running keep theirs.  The sources and build trees stay in the work
# folder: a patch changed rewrites only the files whose content changes,
# and a build tree is configured again in place, never made anew, so that
# an update compiles only what it has to (--clean starts them over).
#
#   build.sh [--deps|--print-deps|--print-stamp|--print-keys|--print-inputs COMPONENT]
#            [--stack DIR|--prefix DIR] [--work DIR] [-j N] [--clean] [COMPONENT]
#
#   COMPONENT      virglrenderer or qemu: builds it and what it needs [qemu]
#   --deps         install the build dependencies first (Fedora, sudo dnf)
#   --print-deps   tell the missing build dependencies, then stop (status 1
#                  if something is missing)
#   --print-stamp  print the stamp of the inputs (QEMU's key), then stop
#   --print-keys   print "COMPONENT KEY" for each component, then stop
#   --print-inputs COMPONENT  print the lines its key sums up, then stop
#   --stack DIR    where the builds and `current` go
#                  [${XDG_DATA_HOME:-~/.local/share}/vitrine/stack]
#   --prefix DIR   install into DIR itself instead: no key folders, no `current`
#   --work DIR     sources and build trees [${XDG_CACHE_HOME:-~/.cache}/vitrine/stack-build]
#   -j N           parallel jobs [nproc]
#   --clean        build trees from nothing: the incremental state in doubt
#
# QEMU gets an absolute RUNPATH to virglrenderer's lib64, never $ORIGIN:
# once QEMU has file capabilities (cap_sys_nice), the loader ignores
# LD_LIBRARY_PATH and $ORIGIN, and would load the system's virglrenderer.
# virglrenderer's folder is named after its own key, so that a change of
# QEMU alone keeps that path, and QEMU links as it did.
#
# Lines starting with "vitrine-build: " are for the app:
#   vitrine-build: step N/M: <what>
#   vitrine-build: component: <component>     what follows is that component's
#   vitrine-build: done: <component> built|up to date <folder>
#   vitrine-build: missing: <dependency>
#   vitrine-build: install: <the command installing what is missing>
#   vitrine-build: warning: <message>
#   vitrine-build: up to date: <folder>       the end: nothing was built
#   vitrine-build: built: <folder>            the end: built
#   vitrine-build: error: <message>
set -euo pipefail
shopt -s nullglob

here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
data_home=${XDG_DATA_HOME:-$HOME/.local/share}
cache_home=${XDG_CACHE_HOME:-$HOME/.cache}

usage() { sed -n '/^#   build.sh/,/^#   --clean/p' "$here/build.sh" | sed 's/^# \{0,3\}//'; }
say() { echo "vitrine-build: $*"; }
died=0
die() { say "error: $*"; died=1; exit 1; }

# The components, in build order, and those each is built against
components=(virglrenderer qemu)
deps_of() {
	case "$1" in
	qemu) echo virglrenderer ;;
	esac
}

# --- arguments ----------------------------------------------------------------
action=build
stack=$data_home/vitrine/stack
prefix=
work=$cache_home/vitrine/stack-build
jobs=$(nproc)
target=qemu
inputs_of=
clean=0
while [ $# -gt 0 ]; do
	case "$1" in
	--deps) action=deps ;;
	--print-deps) action=print-deps ;;
	--print-stamp) action=print-stamp ;;
	--print-keys) action=print-keys ;;
	--print-inputs) action=print-inputs; inputs_of=${2:?--print-inputs needs a component}; shift ;;
	--print-inputs=*) action=print-inputs; inputs_of=${1#*=} ;;
	--stack) stack=${2:?--stack needs a folder}; shift ;;
	--stack=*) stack=${1#*=} ;;
	--prefix) prefix=${2:?--prefix needs a folder}; shift ;;
	--prefix=*) prefix=${1#*=} ;;
	--work) work=${2:?--work needs a folder}; shift ;;
	--work=*) work=${1#*=} ;;
	-j) jobs=${2:?-j needs a number}; shift ;;
	-j*) jobs=${1#-j} ;;
	--clean) clean=1 ;;
	-h|--help) usage; exit 0 ;;
	-*) echo "unknown argument $1" >&2; usage >&2; exit 2 ;;
	*)
		case " ${components[*]} " in
		*" $1 "*) target=$1 ;;
		*) echo "unknown component $1 (${components[*]})" >&2; exit 2 ;;
		esac
		;;
	esac
	shift
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "-j takes a number of jobs, not $jobs"
if [ -n "$inputs_of" ]; then
	case " ${components[*]} " in
	*" $inputs_of "*) ;;
	*) echo "unknown component $inputs_of (${components[*]})" >&2; exit 2 ;;
	esac
fi

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
source_of() {
	case "$1" in
	virglrenderer) echo "$VIRGL_URL $VIRGL_COMMIT" ;;
	qemu) echo "$QEMU_URL $QEMU_COMMIT" ;;
	esac
}

# The patches of a component, as paths in this folder, in the order they
# apply: those its series file names (patches/<component>/series: one
# name per line, # comments), else its *.patch files and those of its
# folders, in the C order of their paths whatever the locale: a folder's
# patches one after the other in name order, the folders in name order
patches_of() {
	local dir=patches/$1 line
	if [ -f "$here/$dir/series" ]; then
		while IFS= read -r line || [ -n "$line" ]; do
			line=${line%%#*}
			line=$(printf '%s' "$line" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
			[ -z "$line" ] || printf '%s\n' "$dir/$line"
		done < "$here/$dir/series"
		return
	fi
	(cd "$here" && printf '%s\n' "$dir"/*.patch "$dir"/*/*.patch | sed '/^$/d' | LC_ALL=C sort)
}

# A component's key: the sha256 of these lines, which its build records
# beside what it installs, for the app to tell what changed since.  This
# script, as it holds the configure options; versions.conf's values, not
# the file (its comments change nothing)
key_lines() {
	local c=$1 d
	local -a patches
	mapfile -t patches < <(patches_of "$c")
	echo "source $(source_of "$c")"
	(cd "$here" && sha256sum build.sh ${patches[@]+"${patches[@]}"})
	for d in $(deps_of "$c"); do
		echo "dep $d $(key_of "$d")"
	done
}
key_of() { key_lines "$1" | sha256sum | cut -c1-64; }

case "$action" in
print-stamp)
	key_of qemu
	exit 0
	;;
print-keys)
	for c in "${components[@]}"; do
		echo "$c $(key_of "$c")"
	done
	exit 0
	;;
print-inputs)
	key_lines "$inputs_of"
	exit 0
	;;
esac
mapfile -t qemu_patches < <(patches_of qemu)
mapfile -t virgl_patches < <(patches_of virglrenderer)
for p in ${qemu_patches[@]+"${qemu_patches[@]}"} ${virgl_patches[@]+"${virgl_patches[@]}"}; do
	[ -f "$here/$p" ] || die "$p, which a series file names, is not there"
done
vkey=$(key_of virglrenderer)
stamp=$(key_of qemu)

# --- dependencies -------------------------------------------------------------
# Fedora's package names; what the build checks for is below (commands and
# pkg-config modules), so that other distributions can map them
fedora=(git gcc gcc-c++ make meson ninja-build pkgconf-pkg-config python3 python3-pyyaml
	python3-wheel python3-setuptools python3-pip binutils
	util-linux glib2-devel pixman-devel zlib-devel libslirp-devel SDL2-devel gtk3-devel libepoxy-devel
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
	need_pc glib-2.0 gio-unix-2.0 pixman-1 zlib slirp sdl2 gtk+-3.0 libusb-1.0 libpulse libpipewire-0.3 \
		spice-protocol libzstd libpng libcap-ng wayland-client
	need_cmd wayland-scanner gdbus-codegen
	# QEMU's configure requires it (install_blobs, on by default): the x86_64
	# UEFI firmware it installs with its other blobs comes compressed
	need_cmd bzip2
	# configure's Python tooling, which it takes from the system or QEMU's
	# own wheels since it may not download (--disable-download)
	local m
	for m in wheel setuptools pip; do
		python3 -c "import $m" 2> /dev/null || missing+=("python3 module $m")
	done
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
# The folders of the stack, named after the keys, or the prefix given for
# all.  QEMU is configured with a prefix that stays the same from build to
# build ($qconf: the stack's `current`), since it compiles its prefix into
# every file; a build installs it with DESTDIR into its stamp folder, where
# QEMU finds its data and modules beside its binary (its install is
# relocatable, meson's default).  virglrenderer is configured with its own
# folder, which changes no command line of its compiles.
if [ -n "$prefix" ]; then
	mkdir -p "$prefix"
	prefix=$(cd "$prefix" && pwd)
	name=
	vprefix=$prefix
	qconf=$prefix
else
	mkdir -p "$stack"
	stack=$(cd "$stack" && pwd)
	name=${stamp:0:16}
	prefix=$stack/$name
	vprefix=$stack/virglrenderer/${vkey:0:16}
	qconf=$stack/current
fi
mkdir -p "$work"
work=$(cd "$work" && pwd)
manifest=$prefix/share/vitrine/stack.conf
vmanifest=$vprefix/share/vitrine/virglrenderer.conf
# QEMU's configure installs Python packages with pip, whose cache would go
# to the home folder; and so does a configure that make runs again
export PIP_CACHE_DIR=$work/pip-cache

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

# what is built already: the manifests, written last, with these keys.
# QEMU's key has virglrenderer's: a QEMU built already names this
# virglrenderer folder, built again if it went
virgl_built() { [ -f "$vmanifest" ] && grep -qx "KEY=$vkey" "$vmanifest"; }
qemu_built() { [ -f "$manifest" ] && grep -qx "STAMP=$stamp" "$manifest"; }
build_virgl=1
! virgl_built || build_virgl=0
build_qemu=0
[ "$target" != qemu ] || qemu_built || build_qemu=1

if [ "$build_virgl" = 0 ] && [ "$build_qemu" = 0 ]; then
	# no Stop between switching `current` and saying so (see the end)
	trap '' TERM INT HUP
	say "done: virglrenderer up to date $vprefix"
	if [ "$target" = qemu ]; then
		flip_current
		say "done: qemu up to date $prefix"
		say "up to date: $prefix"
	else
		say "up to date: $vprefix"
	fi
	exit 0
fi

# A folder of the stack without its manifest is what a build that failed
# or was stopped left: it starts again from nothing.  A prefix given is the
# caller's: installed over, never removed, but without its manifests from
# the first file installed until the build is complete, so that one that
# failed or was stopped half-way is never taken for complete.
partial=
vpartial=
stage=
on_exit() {
	local rc=$?
	if [ "$rc" -ne 0 ]; then
		local d
		for d in "$partial" "$vpartial" "$stage"; do
			[ -z "$d" ] || rm -rf "$d"
		done
		[ "$died" = 1 ] || say "error: ${step:-the build} failed (status $rc)"
	fi
}
trap on_exit EXIT
# stopped (the app's Stop signals the whole process group): the exit above runs
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
steps=$((1 + 2 * build_virgl + 5 * build_qemu))
n=0
step=
begin() { n=$((n + 1)); step=$1; say "step $n/$steps: $1"; }

# --- 1. dependencies ----------------------------------------------------------
begin "checking the build dependencies"
if [ ${#missing[@]} -gt 0 ]; then
	tell_missing
	die "build dependencies are missing (build.sh --deps installs them on Fedora)"
fi
command -v passt > /dev/null 2>&1 ||
	say "warning: no passt on this host: VMs fall back to QEMU's user-mode network (Fedora: dnf install passt)"

src=$work/src
vbuild=$work/virglrenderer-build
qbuild=$work/qemu-build
mkdir -p "$src"
if [ "$clean" = 1 ]; then
	echo "build trees from nothing (--clean)"
	rm -rf "$vbuild" "$qbuild"
	rm -f "$src"/*.stamp
fi

# --- sources ------------------------------------------------------------------
# no credential prompts (a desktop's askpass dialog) for public repositories
export GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=
git() { command git -c credential.helper= -c advice.detachedHead=false "$@"; }

# subprojects DIR NAME...: meson's subprojects NAME of DIR, downloaded as
# its wraps and their patch files say now, so that configure, which may not
# download (--disable-download), finds them: all the network use is in the
# fetch steps.  Downloaded again when those change (meson would build a
# stale one, its folder having a meson.build), or when a download was cut
# short: DIR.subprojects says what they come from, once all are there.
subprojects() {
	local d=$1 s pd key
	shift
	key=$(cd "$d/subprojects" && for s in "$@"; do
		sha256sum "$s.wrap"
		pd=$(sed -n 's/^[[:space:]]*patch_directory[[:space:]]*=[[:space:]]*\([^[:space:]]*\).*/\1/p' "$s.wrap")
		[ -z "$pd" ] || [ ! -d "packagefiles/$pd" ] ||
			find "packagefiles/$pd" -type f -print0 | LC_ALL=C sort -z | xargs -0r sha256sum
	done | sha256sum | cut -c1-64)
	if [ "$(cat "$d.subprojects" 2> /dev/null)" != "$key" ]; then
		rm -f "$d.subprojects"
		for s in "$@"; do
			rm -rf "${d:?}/subprojects/$s"
		done
	fi
	# meson's progress overwrites its line (\r, then an escape that erases
	# it): as lines of their own, lest the app read the next line of build.sh
	# as part of one
	meson subprojects download --sourcedir "$d" "$@" 2>&1 |
		tr '\r' '\n' | sed 's/\x1b\[K//g; /^[[:space:]]*$/d' ||
		die "cannot download the subprojects $* of $(basename "$d") (no network?)"
	echo "$key" > "$d.subprojects"
}

# checkout DIR URL COMMIT SUBPROJECTS PATCH...: DIR at COMMIT with the
# patches applied, and its meson SUBPROJECTS (a list, of those it has a wrap
# for) downloaded; left alone when it is already (DIR.stamp: the commit and
# the patches' sha256).
#
# The patched tree is made in an index of its own, the working tree left
# alone: a patch that does not apply changes nothing.  Then git takes the
# working tree there from what the last build left, writing only the files
# whose content differs: the others keep their time, and nothing that
# depends on them only compiles again.
checkout() {
	local d=$1 url=$2 commit=$3 p sums s tree index
	local -a subs keep=()
	read -ra subs <<< "$4"
	shift 4
	sums=$(cd "$here" && cat "$@" < /dev/null | sha256sum | cut -c1-64)
	if [ -d "$d/.git" ] && [ "$(cat "$d.stamp" 2> /dev/null)" = "$commit $sums" ]; then
		echo "$(basename "$d") at ${commit:0:12} with $# patches already"
		# a subproject gone since is downloaded again; the others stay as they are
		subprojects_of "$d" "${subs[@]}"
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
	index=$d/.git/vitrine-patched-index
	rm -f "$index"
	GIT_INDEX_FILE=$index git -C "$d" read-tree "$commit"
	for p in "$@"; do
		echo "applying $p"
		GIT_INDEX_FILE=$index git -C "$d" apply --cached --whitespace=nowarn "$here/$p" ||
			die "$p does not apply to ${commit:0:12}"
	done
	tree=$(GIT_INDEX_FILE=$index git -C "$d" write-tree)
	rm -f "$index"
	# all the last build left goes, nested repositories too (-ff): meson's
	# downloads are, and meson would build a stale one.  The downloads of
	# SUBPROJECTS stay, for subprojects() to tell whether they still hold
	for s in "${subs[@]}"; do
		keep+=(-e "/subprojects/$s/")
	done
	git -C "$d" clean -q -ffdx ${keep[@]+"${keep[@]}"}
	# the files as they are, changed by hand too, which go back
	git -C "$d" add -u
	git -C "$d" read-tree -m -u "$tree"
	# a commit of it, for `git diff COMMIT` to show the patches
	git -C "$d" update-ref --no-deref HEAD "$(git -C "$d" -c user.name=vitrine -c user.email=vitrine@localhost \
		commit-tree "$tree" -p "$commit" -m "${commit:0:12} with $# patches")"
	subprojects_of "$d" "${subs[@]}"
	echo "$commit $sums" > "$d.stamp"
}

# subprojects_of DIR NAME...: subprojects() for those of NAME DIR has a wrap for
subprojects_of() {
	local d=$1 s
	local -a have=()
	shift
	for s in "$@"; do
		[ ! -f "$d/subprojects/$s.wrap" ] || have+=("$s")
	done
	[ ${#have[@]} -eq 0 ] || subprojects "$d" "${have[@]}"
}

# configured DIR LINE: whether build tree DIR was configured with LINE
configured() { [ -f "$1/.vitrine-configure" ] && [ "$(cat "$1/.vitrine-configure")" = "$2" ]; }

# put FILE DEST: DEST with FILE's content, written only if it differs: what
# depends on it does not compile again
put() {
	cmp -s "$1" "$2" && return 0
	mkdir -p "$(dirname "$2")"
	cp "$1" "$2.new"
	mv -f "$2.new" "$2"
}

# devel FOLDER DEV: in DEV, what QEMU is compiled and linked against of the
# virglrenderer in FOLDER: its headers and pkg-config file, its library
# linked to.  At a path that stays from build to build: its include
# folder is in most of QEMU's compile commands, which a new path would
# all change (QEMU's RUNPATH names FOLDER itself)
devel() {
	local from=$1 dev=$2 f pc
	[ ! -d "$from/include" ] || (cd "$from/include" && find . -type f) | while read -r f; do
		put "$from/include/$f" "$dev/include/$f"
	done
	[ ! -d "$dev/include" ] || (cd "$dev/include" && find . -type f) | while read -r f; do
		[ -e "$from/include/$f" ] || rm -f "$dev/include/$f"
	done
	pc=$dev/virglrenderer.pc.new
	mkdir -p "$dev/lib64/pkgconfig"
	sed "s|^prefix=.*|prefix=$dev|" "$from/lib64/pkgconfig/virglrenderer.pc" > "$pc"
	put "$pc" "$dev/lib64/pkgconfig/virglrenderer.pc"
	rm -f "$pc"
	ln -sfn "$from/lib64/libvirglrenderer.so" "$dev/lib64/libvirglrenderer.so"
}

# their paths in patches/<component>: research/0001-x.patch
patch_names() { local p; for p in "$@"; do printf '%s ' "${p#patches/*/}"; done; }

# --- virglrenderer ------------------------------------------------------------
say "component: virglrenderer"
if [ "$build_virgl" = 0 ]; then
	echo "virglrenderer built already: $vprefix"
	say "done: virglrenderer up to date $vprefix"
else
	[ -z "$name" ] || vpartial=$vprefix
	if [ -n "$name" ]; then
		rm -rf "$vprefix"
	fi
	rm -f "$vmanifest"
	begin "fetching virglrenderer"
	checkout "$src/virglrenderer" "$VIRGL_URL" "$VIRGL_COMMIT" "" ${virgl_patches[@]+"${virgl_patches[@]}"}

	begin "building virglrenderer"
	# native context for AMD GPUs, and Intel ones on the i915 or the Xe driver;
	# the msm and asahi renderers serve ARM hosts, which this x86_64 QEMU is not for
	vargs=(--prefix="$vprefix" --libdir=lib64 --buildtype=debugoptimized
		-Ddrm-renderers=amdgpu-experimental,i915-experimental,xe-experimental -Dvideo=true -Dvenus=false)
	vline="$src/virglrenderer ${vargs[*]}"
	# configured again in place when the options change (a new folder with
	# each new key): meson keeps what its compiles' command lines do not change
	if [ ! -f "$vbuild/build.ninja" ]; then
		rm -rf "$vbuild"
		meson setup "$vbuild" "$src/virglrenderer" "${vargs[@]}"
	elif ! configured "$vbuild" "$vline"; then
		rm -f "$vbuild/.vitrine-configure"
		meson setup --reconfigure "$vbuild" "$src/virglrenderer" "${vargs[@]}" || {
			echo "configuring again in place failed: from nothing"
			rm -rf "$vbuild"
			meson setup "$vbuild" "$src/virglrenderer" "${vargs[@]}"
		}
	fi
	echo "$vline" > "$vbuild/.vitrine-configure"
	ninja -C "$vbuild" -j "$jobs"
	meson install -C "$vbuild" --no-rebuild --quiet
	[ -f "$vprefix/lib64/pkgconfig/virglrenderer.pc" ] || die "virglrenderer installed no virglrenderer.pc"
	mkdir -p "$(dirname "$vmanifest")"
	key_lines virglrenderer > "$vprefix/share/vitrine/virglrenderer.inputs"
	# what was built, written last: a folder with it is complete
	cat > "$vmanifest.new" << EOF
# What host/build.sh built here
KEY=$vkey
BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
URL=$VIRGL_URL
COMMIT=$VIRGL_COMMIT
VERSION=$(PKG_CONFIG_PATH=$vprefix/lib64/pkgconfig pkg-config --modversion virglrenderer)
PATCHES=$(patch_names ${virgl_patches[@]+"${virgl_patches[@]}"})
EOF
	mv -f "$vmanifest.new" "$vmanifest"
	vpartial=
	say "done: virglrenderer built $vprefix"
fi
if [ "$target" = virglrenderer ]; then
	trap '' TERM INT HUP
	say "built: $vprefix"
	exit 0
fi

# --- QEMU ---------------------------------------------------------------------
say "component: qemu"
if [ "$build_qemu" = 0 ]; then
	# built already, with the virglrenderer made again above
	trap '' TERM INT HUP
	flip_current
	say "done: qemu up to date $prefix"
	say "built: $prefix"
	exit 0
fi
if [ -n "$name" ]; then
	rm -rf "$prefix"
	partial=$prefix
fi
rm -f "$manifest"

begin "fetching QEMU"
# the subprojects this configuration builds: keycodemapdb, which configure
# checks for; dtc for --enable-fdt=internal
checkout "$src/qemu" "$QEMU_URL" "$QEMU_COMMIT" "dtc keycodemapdb" ${qemu_patches[@]+"${qemu_patches[@]}"}

begin "configuring QEMU"
qargs=(--prefix="$qconf" --target-list=x86_64-softmmu --without-default-features --disable-werror
	--enable-kvm --enable-tcg --enable-pixman --enable-attr --enable-virtfs --enable-hmp
	--enable-malloc-trim --enable-sdl --enable-gtk --enable-opengl --enable-virglrenderer
	--enable-libusb --enable-pa --enable-pipewire --enable-spice-protocol --enable-passt --enable-gio
	--enable-dbus-display --enable-slirp --enable-tpm --enable-vhost-kernel --enable-vhost-net
	--enable-vhost-user --enable-zstd --enable-png --enable-tools --enable-fdt=internal --disable-docs
	--disable-plugins --disable-containers --container-command=no --disable-download
	"--extra-ldflags=-Wl,-rpath,$vprefix/lib64")
# (no containers: configure would otherwise run podman to see if it works, which
# sets up podman's storage in the user's home, for cross-builds we never do;
# no download: the subprojects come with the sources, and Python's tooling
# from the system and the wheels QEMU carries, rather than from PyPI)
#
# Configured again in place when this line changes: QEMU's configure
# clears meson's state (its probes start over) and keeps what was
# compiled, which compiles again only where a command line changed.  The
# line names the virglrenderer QEMU is built against: configure probes
# what that offers (virgl_renderer_resource_set_guest_dmabuf, its version),
# and meson keeps what it found, which nothing else would make it look
# again at
vdev=$work/virglrenderer-devel
devel "$vprefix" "$vdev"
qline="$src/qemu ${qargs[*]} PKG_CONFIG_PATH=$vdev/lib64/pkgconfig virglrenderer=$vkey"
if [ -f "$qbuild/build.ninja" ] && configured "$qbuild" "$qline"; then
	echo "configured already"
else
	rm -f "$qbuild/.vitrine-configure"
	mkdir -p "$qbuild"
	(cd "$qbuild" && PKG_CONFIG_PATH="$vdev/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
		"$src/qemu/configure" "${qargs[@]}")
	echo "$qline" > "$qbuild/.vitrine-configure"
fi
grep -q "define HAVE_VIRGL_RESOURCE_SET_GUEST_DMABUF" "$qbuild/config-host.h" ||
	say "warning: QEMU is built without virglrenderer's guest dma-buf call: no zero copy of guest dma-bufs"

begin "compiling QEMU"
make -C "$qbuild" -j "$jobs"

begin "installing QEMU"
if [ -n "$name" ]; then
	# beside the stack, on its file system: moved into its folder in one step
	stage=$stack/.stage-$name
	rm -rf "$stage"
	make -C "$qbuild" install DESTDIR="$stage"
	[ -d "$stage$qconf" ] || die "QEMU installed nothing under $stage$qconf"
	mv -T "$stage$qconf" "$prefix"
	rm -rf "$stage"
	stage=
	# the firmware descriptors name the firmware by its configured path
	for f in "$prefix"/share/qemu/firmware/*.json; do
		json=$(< "$f")
		printf '%s\n' "${json//"$qconf/"/"$prefix/"}" > "$f"
	done
else
	make -C "$qbuild" install
fi
# QEMU installs the UEFI firmware of every architecture it knows, some 200 MB:
# the VMs are x86_64 ones
rm -f "$prefix"/share/qemu/edk2-{aarch64,arm,riscv64,loongarch64}-* \
	"$prefix"/share/qemu/firmware/*-edk2-{aarch64,arm,riscv64,loongarch64}*.json
# the full option descriptions, for the app's QEMU reference
install -m 0644 "$src/qemu/qemu-options.hx" "$prefix/share/qemu/qemu-options.hx"

begin "checking the installation"
qemu=$prefix/bin/qemu-system-x86_64
[ -x "$qemu" ] && [ -x "$prefix/bin/qemu-img" ] || die "qemu-system-x86_64 or qemu-img was not installed"
# meson removes the run paths it adds for the build when it installs; ours must stay
runpath=$(readelf -d "$qemu" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]$/\1/p')
case ":$runpath:" in
*'$ORIGIN'*) die "the RUNPATH of qemu-system-x86_64 uses \$ORIGIN: $runpath" ;;
*":$vprefix/lib64:"*) echo "RUNPATH $runpath" ;;
*) die "qemu-system-x86_64 has no RUNPATH to $vprefix/lib64 (it has: ${runpath:-none})" ;;
esac
# what the loader finds, without what the environment adds; and with
# modules, the virtio-gpu-gl one is what loads virglrenderer
for f in "$qemu" "$prefix"/lib*/qemu/hw-display-virtio-gpu-gl.so; do
	lib=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD ldd "$f" | sed -n 's/^[[:space:]]*libvirglrenderer\.so[.0-9]* => \([^ ]*\).*/\1/p')
	[ "$lib" = "$vprefix/lib64/libvirglrenderer.so.1" ] ||
		die "$(basename "$f") loads ${lib:-no virglrenderer} instead of $vprefix/lib64/libvirglrenderer.so.1"
	echo "$(basename "$f") loads $lib"
done
version=$("$qemu" -version | sed -n 's/^QEMU emulator version \([^ ]*\).*/\1/p')
[ -n "$version" ] || die "qemu-system-x86_64 -version failed"
"$prefix/bin/qemu-img" --version > /dev/null || die "qemu-img --version failed"
props=$("$qemu" -device virtio-gpu-gl-pci,help)
for p in drm_native_context x-host-vblank; do
	grep -q "^ *$p=" <<< "$props" || die "virtio-gpu-gl-pci has no $p property: not the patched QEMU"
done
echo "QEMU $version, virtio-gpu-gl-pci with drm_native_context and x-host-vblank"

# what was built, written last: a prefix with it is complete.  From here
# on, a Stop would leave `current` switched to it while the app hears of
# a failure: the few commands left run to their end, and the commands
# they start ignore the signals too
trap '' TERM INT HUP
mkdir -p "$(dirname "$manifest")"
key_lines qemu > "$prefix/share/vitrine/qemu.inputs"
cat > "$manifest.new" << EOF
# What host/build.sh built here
STAMP=$stamp
BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
QEMU_URL=$QEMU_URL
QEMU_COMMIT=$QEMU_COMMIT
QEMU_VERSION=$version
QEMU_PATCHES=$(patch_names ${qemu_patches[@]+"${qemu_patches[@]}"})
VIRGL_URL=$VIRGL_URL
VIRGL_COMMIT=$VIRGL_COMMIT
VIRGL_VERSION=$(PKG_CONFIG_PATH=$vprefix/lib64/pkgconfig pkg-config --modversion virglrenderer)
VIRGL_PATCHES=$(patch_names ${virgl_patches[@]+"${virgl_patches[@]}"})
VIRGL_KEY=$vkey
VIRGL_PREFIX=$vprefix
EOF
mv -f "$manifest.new" "$manifest"
partial=
flip_current
say "done: qemu built $prefix"
say "built: $prefix"
