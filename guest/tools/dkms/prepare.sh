#!/bin/sh
# DKMS PRE_BUILD: put the virtio-gpu sources of the kernel being built into
# the build directory, then apply the patches.
#
# The module is the in-tree driver plus vitrine's patches, so it must follow
# the kernel it is built for: taking the sources from the matching stable tag
# means every fix that landed since keeps working.  The sources are cached
# under /var/cache.  Offline, the vendored copy is used only for the kernel
# version it comes from (vendor/VERSION): built for another kernel it could
# load and misbehave, so the build fails instead, loudly, and that kernel
# keeps the stock driver.
#
# Usage (DKMS calls it): prepare.sh KERNELVERSION
set -eu

kver=${1:-$(uname -r)}
base=${kver%%-*}                       # 7.2.7-200.fc44.x86_64 -> 7.2.7
tag="v${base%.0}"                      # upstream tags a .0 release as vX.Y
cache=${VITRINE_VIRTIO_GPU_CACHE:-/var/cache/vitrine-virtio-gpu}/$tag
url=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/gpu/drm/virtio
headers="virtgpu_drv.h virtgpu_trace.h"

say() { echo "vitrine-virtio-gpu: $*"; }

fetch() {  # fetch FILE DEST -> 0 on success
	curl -sS -f --connect-timeout 10 --max-time 60 -o "$2" "$url/$1?h=$tag"
}

sources_from_cache() {
	[ -f "$cache/.complete" ] || return 1
	cp "$cache"/*.c "$cache"/*.h "$cache"/Makefile .
	say "using the cached sources of $tag"
}

sources_from_upstream() {
	tmp=$(mktemp -d)
	fetch Makefile "$tmp/Makefile" || { rm -rf "$tmp"; return 1; }
	# The list of source files comes from the kernel's own Makefile, so a
	# file added upstream is picked up without editing this script.
	objs=$(awk '/^virtio-gpu-y/ { sub(/^[^=]*=/, ""); c = 1 }
	            c { cont = ($0 ~ /\\$/); gsub(/\\$/, ""); printf "%s ", $0; if (!cont) c = 0 }' "$tmp/Makefile")
	[ -n "$objs" ] || { rm -rf "$tmp"; return 1; }
	for o in $objs; do
		case "$o" in *.o) ;; *) continue ;; esac
		fetch "${o%.o}.c" "$tmp/${o%.o}.c" || { rm -rf "$tmp"; return 1; }
	done
	for h in $headers; do
		fetch "$h" "$tmp/$h" || { rm -rf "$tmp"; return 1; }
	done
	# Only mark the cache complete once every file is there, so an
	# interrupted download is not reused.
	for o in $objs; do
		case "$o" in *.o) [ -s "$tmp/${o%.o}.c" ] || { rm -rf "$tmp"; return 1; } ;; esac
	done
	mkdir -p "$cache"
	cp "$tmp"/* "$cache"/ && touch "$cache/.complete"
	cp "$tmp"/* .
	rm -rf "$tmp"
	say "fetched the sources of $tag from kernel.org"
}

sources_from_vendor() {
	vendored=$(cat vendor/VERSION 2> /dev/null || true)
	[ -d vendor ] && [ "$vendored" = "$base" ] || return 1
	cp vendor/*.c vendor/*.h vendor/Makefile .
	say "offline: using the vendored sources of $vendored"
}

sources_from_cache || sources_from_upstream || sources_from_vendor || {
	say "ERROR: no driver sources for kernel $kver (tag $tag): none cached in $cache,"
	say "ERROR: kernel.org could not be reached, and the vendored copy is of"
	say "ERROR: $(cat vendor/VERSION 2> /dev/null || echo 'no version'), not $base. This kernel keeps the stock virtio-gpu"
	say "ERROR: driver. Connect the guest to the network, then: sudo dkms autoinstall -k $kver"
	exit 1
}

# Build out of tree: no Kconfig symbol, and the trace header is included
# relative to this directory.
sed -i 's/^obj-\$(CONFIG_DRM_VIRTIO_GPU)/obj-m/' Makefile
grep -q 'CFLAGS_virtgpu_trace_points.o' Makefile ||
	printf '\nCFLAGS_virtgpu_trace_points.o := -I$(src)\n' >> Makefile
sed -i 's|#define TRACE_INCLUDE_PATH .*|#define TRACE_INCLUDE_PATH .|' virtgpu_trace.h

if grep -q '\.open = virtio_gpu_gem_object_open' virtgpu_prime.c; then
	say "the import patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-attach-imported-dmabufs.patch
	say "import patch applied"
fi

if grep -q 'virtio_gpu_host_vblank' virtgpu_display.c; then
	say "the host-vblank patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-host-vblank.patch
	say "host-vblank patch applied"
fi

if grep -q 'virtio_gpu_flip_in_grace' virtgpu_display.c; then
	say "the flip-grace patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-flip-grace.patch
	say "flip-grace patch applied"
fi

if grep -q 'flush_unchanged' virtgpu_plane.c; then
	say "the unchanged-flush patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-skip-unchanged-flush.patch
	say "unchanged-flush patch applied"
fi

if grep -q 'blob_flush_fence' virtgpu_plane.c; then
	say "the blob-flush-fence patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-blob-flush-fence.patch
	say "blob-flush-fence patch applied"
fi

if grep -q 'blob_damage' virtgpu_plane.c; then
	say "the host-blob-damage patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-host-blob-damage.patch
	say "host-blob-damage patch applied"
fi

if grep -q 'virtio_gpu_flip_gate_poll' virtgpu_display.c; then
	say "the gated-flip-event patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-gated-flip-event.patch
	say "gated-flip-event patch applied"
fi

if grep -q 'linear_scanout' virtgpu_plane.c; then
	say "the linear-scanout patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-linear-scanout.patch
	say "linear-scanout patch applied"
fi

if grep -q 'virtio_gpu_fence_own_context' virtgpu_fence.c; then
	say "the flush-fence-context patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-flush-fence-context.patch
	say "flush-fence-context patch applied"
fi

if grep -q 'blob_flush_fence == 3' virtgpu_plane.c; then
	say "the reader-fence patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-reader-fence.patch
	say "reader-fence patch applied"
fi

if grep -q 'tiled_scanout' virtgpu_plane.c; then
	say "the tiled-scanout patch is already in these sources, nothing to apply"
else
	patch -p5 --forward --silent < patches/linux-drm-virtio-tiled-scanout.patch
	say "tiled-scanout patch applied"
fi
