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
# keeps the stock driver.  The patches apply without fuzz for the same
# reason: one that only fits with fuzz could land in the wrong place.
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

fetch() {  # fetch FILE DEST -> 0 on success, else says why
	# kernel.org's cgit answers 429 or 503 now and then, and a guest's
	# network may come up late: three more tries before giving up.  Not an
	# error yet: the vendored copy may do (the summary below says ERROR
	# when nothing does)
	curl -sS -f --connect-timeout 10 --max-time 60 --retry 3 --retry-all-errors \
		--retry-delay 2 -o "$2" "$url/$1?h=$tag" && return 0
	rc=$?
	say "could not fetch $1 of $tag from kernel.org (curl exit status $rc, 4 tries)" >&2
	return 1
}

# The driver's source files, from its Makefile (a file added upstream is
# picked up without editing this script)
sources_of() {
	awk '/^virtio-gpu-y/ { sub(/^[^=]*=/, ""); c = 1 }
	     c { cont = ($0 ~ /\\$/); gsub(/\\$/, ""); printf "%s ", $0; if (!cont) c = 0 }' "$1" |
		tr ' \t' '\n\n' | sed -n 's/\.o$/.c/p'
}

# A kernel source file, not the page a proxy or kernel.org's bot check
# answers with status 200: curl -f takes that for the file
kernel_source() {
	grep -q '^#include' "$1" && ! head -c 1024 "$1" | grep -qi '<!doctype\|<html'
}

# A cache is whole when every file is there, not empty, and a kernel source:
# a guest that lost power just after writing one has been seen to keep
# empty files, and before 0.1.0-14 such a page could be cached as one
cache_whole() {
	[ -f "$1/.complete" ] && [ -s "$1/Makefile" ] || return 1
	files=$(sources_of "$1/Makefile")
	[ -n "$files" ] || return 1
	for f in $files $headers; do
		[ -s "$1/$f" ] && kernel_source "$1/$f" || return 1
	done
}

sources_from_cache() {
	[ -d "$cache" ] || return 1
	if ! cache_whole "$cache"; then
		say "the cached sources of $tag are incomplete or not kernel sources: fetching them again"
		rm -rf "$cache"
		return 1
	fi
	cp "$cache"/*.c "$cache"/*.h "$cache"/Makefile .
	say "using the cached sources of $tag"
}

sources_from_upstream() {
	mkdir -p "$(dirname "$cache")"
	# beside the cache, so that putting it in place is one rename
	tmp=$(mktemp -d "$(dirname "$cache")/.fetch.XXXXXX") && chmod 755 "$tmp"
	fetch Makefile "$tmp/Makefile" || { rm -rf "$tmp"; return 1; }
	files=$(sources_of "$tmp/Makefile")
	if [ -z "$files" ]; then
		say "the Makefile kernel.org sent for $tag names no source of the driver (a proxy's or a bot check's page?)" >&2
		rm -rf "$tmp"
		return 1
	fi
	for f in $files $headers; do
		fetch "$f" "$tmp/$f" || { rm -rf "$tmp"; return 1; }
		if ! kernel_source "$tmp/$f"; then
			say "what kernel.org sent for $f of $tag is not a kernel source file (a proxy's or a bot check's page?)" >&2
			rm -rf "$tmp"
			return 1
		fi
	done
	touch "$tmp/.complete"
	cache_whole "$tmp" || { rm -rf "$tmp"; return 1; }
	# on the disk before it is the cache
	sync "$tmp"/* "$tmp/.complete" 2> /dev/null || sync
	rm -rf "$cache"
	mv "$tmp" "$cache"
	sync "$(dirname "$cache")" 2> /dev/null || true
	cp "$cache"/*.c "$cache"/*.h "$cache"/Makefile .
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
	say "ERROR: the fetch from kernel.org failed (see above), and the vendored copy is of"
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
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-attach-imported-dmabufs.patch
	say "import patch applied"
fi

# Linux 7.2.9 and 7.3-rc5 refuse foreign dma-buf imports on a 3D device again
# (the revert of df4dc947): the import patch above would apply and do nothing.
# Allowed again for native contexts (module parameter import_3d).
if grep -q 'has_resource_blob || vgdev->has_virgl_3d)' virtgpu_prime.c; then
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-import-3d.patch
	say "import-3d patch applied (these sources have the 7.2.9 revert)"
fi

if grep -q 'virtio_gpu_host_vblank' virtgpu_display.c; then
	say "the host-vblank patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-host-vblank.patch
	say "host-vblank patch applied"
fi

if grep -q 'virtio_gpu_flip_in_grace' virtgpu_display.c; then
	say "the flip-grace patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-flip-grace.patch
	say "flip-grace patch applied"
fi

if grep -q 'flush_unchanged' virtgpu_plane.c; then
	say "the unchanged-flush patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-skip-unchanged-flush.patch
	say "unchanged-flush patch applied"
fi

if grep -q 'blob_flush_fence' virtgpu_plane.c; then
	say "the blob-flush-fence patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-blob-flush-fence.patch
	say "blob-flush-fence patch applied"
fi

if grep -q 'blob_damage' virtgpu_plane.c; then
	say "the host-blob-damage patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-host-blob-damage.patch
	say "host-blob-damage patch applied"
fi

if grep -q 'virtio_gpu_flip_gate_poll' virtgpu_display.c; then
	say "the gated-flip-event patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-gated-flip-event.patch
	say "gated-flip-event patch applied"
fi

if grep -q 'linear_scanout' virtgpu_plane.c; then
	say "the linear-scanout patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-linear-scanout.patch
	say "linear-scanout patch applied"
fi

if grep -q 'virtio_gpu_fence_own_context' virtgpu_fence.c; then
	say "the flush-fence-context patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-flush-fence-context.patch
	say "flush-fence-context patch applied"
fi

if grep -q 'blob_flush_fence == 3' virtgpu_plane.c; then
	say "the reader-fence patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-reader-fence.patch
	say "reader-fence patch applied"
fi

if grep -q 'tiled_scanout' virtgpu_plane.c; then
	say "the tiled-scanout patch is already in these sources, nothing to apply"
else
	patch -p5 --forward -F0 --no-backup-if-mismatch < patches/linux-drm-virtio-tiled-scanout.patch
	say "tiled-scanout patch applied"
fi
