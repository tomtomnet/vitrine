# Patches of Vitrine's QEMU

`host/build.sh` fetches each component at the commit `host/versions.conf`
pins and applies with `git apply` the `*.patch` files of
`patches/<component>/` and of its folders, in the C order of their paths
(`LC_ALL=C sort`): a folder's patches in name order, the folders in name
order. Folders deeper down, hidden files and other names are not applied.
A `patches/<component>/series` file, if there is one, names the patches
instead, one per line in the order they apply (`#` comments).

The Builds window shows the patches of a folder as coming from what
`host/components.conf` names it (`origin.research=Research`), each by
its `Subject:`, else the first line of its description.

## qemu (upstream at `3876503f`)

`host/versions.conf` pins upstream QEMU
(`https://gitlab.com/qemu-project/qemu.git`), and `qemu/series` lists the
patches that go on top, in this order:

1. `qemu/research/`: the research's QEMU series, its `patches/qemu/series`
   at commit `5f7e6af` (2026-10-14), each patch without its
   `Claude-Session:` line and otherwise unchanged. Since 2026-10-14 this
   series is the source of the research's QEMU; the qemu-gui fork
   (`https://github.com/tomtomnet/qemu-gui`) it started from is no longer
   used. 0001-0017 are the fork's commits, 0018 the work the fork had not
   committed on 2026-10-12 (one patch for now, the research splits it
   later), and the last three are for `virtio-gpu-rutabaga`, the nested
   desktop's device.
2. `qemu/vitrine/`: Vitrine's own changes, as new files here rather than
   edits of the others.

### `qemu/research/`

| sha256 | patch |
|---|---|
| `521d7c619f6836b90f3b2113d6b82e3db3ec5ea9e2397c333fc9db86da3b2d5b` | `0001-ui-sdl2-follow-the-host-refresh-rate-tolerate-grab-b.patch` |
| `4c52aae7e7f078e5dd4af793cc9f30926ba4a73ae5db3809da981683220b3a05` | `0002-ui-sdl2-add-an-in-window-control-menu-Dear-ImGui.patch`: the menu 0018 removes again |
| `b1b927381170bf2056e012903a31804cf07c90956db41c1d5a5836ab20578e9a` | `0003-Add-a-README-for-this-fork.patch` |
| `183f9f34b0f27d783d27787df5d32d96889429e6fbc7792c214e939d978c8109` | `0004-ui-sdl2-draw-the-menu-bar-without-a-border.patch` |
| `1cd67882a955602fa9145bf59746ea0659a258d20a2e99289b194c4a386e496f` | `0005-ui-sdl2-share-the-clipboard-with-the-guest.patch` |
| `b3b1b9fdd9b403b30c13f1051de170743fcc25ae15e886c94eb1c3a40b4b6102` | `0006-contrib-add-vdagent-clipboard-bridge-for-Wayland-gue.patch` |
| `6bc641b70437d45e8ea2aedbdf48ddf8511a9dd78edb5774a83bf70b7cd14494` | `0007-contrib-vdagent-clipboard-bridge-wait-for-XWayland-f.patch` |
| `c8b652f45275697272651d4357fa4b5193781b491fdc54acb205fd6000fe2520` | `0008-hw-display-virtio-gpu-gl-tell-whether-the-guest-uses.patch`: `x-drm-offered`, `x-drm-contexts`, `x-virgl-contexts` |
| `bc15cbbdc77dd5f1c962773afc90286dcd8b53cd6d09695393d7f8772052907c` | `0009-virtio-gpu-gl-disable-the-scanouts-on-reset-in-the-m.patch` |
| `6fd23105ea52f009efc16ff6eb31a3b6fceb1c4473c76736ff52a3c51f362600` | `0010-virtio-gpu-virgl-give-guest-memory-blobs-a-udmabuf-f.patch` |
| `1625a761dd0bd1a4531ed44b067c5b898e80ea341db7dbfc6ada4779cbdec4bb` | `0011-zero-copy-the-virglrenderer-patch-this-branch-needs-.patch` |
| `efd7be38ca303d991acf34ddb33422d6d63d4be211978df713bf44541bc12a68` | `0012-ui-sdl2-frame-and-input-latency-stats-x-query-displa.patch` |
| `1fefe2f2fc934aa83bb735871df79e59ec0b2abb46f0dde204aa339e4a545e06` | `0013-README-zero-copy-and-frame-statistics-are-in-master.patch` |
| `b28eb6124a4e0e576be61abd095dd6a36c0d677bb5fe18f2f416d1bd4a648c47` | `0014-virtio-gpu-guest-vblank-locked-to-the-host-s-present.patch`: `x-host-vblank` and the vblank lead and swap targets |
| `54db2036e019926c2a8e7fdb679f70241bef9da14204562d9675785b1cda5ec1` | `0015-machine-x-vcpu-priority-QMP-command.patch` |
| `edd1d14cb1c7a6ac931706559183a120b5b0c282a45edc7760d5a26491acca63` | `0016-ui-sdl2-own-Wayland-swapchain-for-the-guest-s-frames.patch` |
| `a61e1259f19b9f137246bcb17b9484274d0c54adcea1e2548c7e3762dd4b8326` | `0017-ui-dbus-presentation-feedback-from-the-D-Bus-display.patch` |
| `81ceed5214069bb20523461c1ba957b719d12c1c1d7fba4bb47879fef39abb59` | `0018-The-fork-s-uncommitted-work-as-of-2026-10-12-to-be-s.patch`: zero copy that holds the guest's flushes, by default in full screen, tiled buffers (`ui/dmabuf-layout.c`), the D-Bus display's zero copy, the fixes of the research's static review of 2026-10-04, the host's latching deadline (`x-vblank-deadline`), virtio-gpu-rutabaga's additions (drm and virgl2 capsets, a 512-entry control queue, `x-fixed-blob-mapping`, `x-guest-dmabuf`, `x-prefault-blobs`, the fence ring index as a byte), KVM's slot zap knob, the virtio-gpu unref leak fix (v2); and the SDL window's ImGui menu removed |
| `f2ca5e6090a29bfadb29e940368227811cc618aa1b34a5c5c30007c6efb0be8d` | `fix-rutabaga-scanout-image-leak.patch` |
| `6572397833517b8e19675ac556935458bed10be5208f36150ec28e5573cf01d3` | `virtio-gpu-rutabaga-scanout-blob.patch`: `SET_SCANOUT_BLOB` on displays with GL: the guest's GPU-rendered login screen on its own console |
| `7241c150b19b49f6c598f3b3a527b48f5bcb8ad66e43123e29bb7f0fa8c1bce6` | `virtio-gpu-rutabaga-gl-context-scope.patch`: virglrenderer's GL context kept apart from the display's, so that the VGA and virtio-gpu consoles show with `-display dbus,gl=on` |

When the research's series moves (`r` its repository, `c` the commit,
`v` this folder):

```sh
r=~/Documents/virtio-gpu-desktop c=5f7e6af v=~/Documents/vitrine/host/patches/qemu
rm -f "$v"/research/*.patch
for p in $(git -C "$r" show "$c:patches/qemu/series" | sed 's/#.*//'); do
    git -C "$r" show "$c:patches/qemu/$p" | grep -v '^Claude-Session:' > "$v/research/$p"
done
```

Then `qemu/series` gets their names in the research's order, the
`vitrine/` lines after them; the table above its new checksums; and
build.sh stops at the first patch that does not apply (`... does not
apply`), the place to look at Vitrine's own patches again. A line that
starts with `Claude-Session:` can only be in a patch's message, never in
its diff, whose lines start with a space, `+`, `-`, `@` or `\`.

#### From the series before (2026-10-14)

Until then Vitrine applied the fork's 13 commits (`qemu/fork/`, exported
from its master at `49fd5067`) and the research export's 0001-0008
(2026-10-04): the same 0001-0017 (the same tree), then 0005-0007, the
fork's uncommitted zero copy of that day, and 0008, the fixes of the
research's static review. 0018 holds all of them in their later form but
two things, both on purpose: the SDL window's ImGui menu, with the zero
copy below its docked bar on a subsurface (`viewporter`,
`fractional-scale-v1`), which the fork removed (and with it QEMU's
`sdl-gui` option, which build.sh no longer passes); and 0014's blob page
array leak fix, which the unref leak fix v2 (`virtio_gpu_cleanup_mapping()`)
covers.

### `qemu/vitrine/`

Vitrine's own, on top, for the research side to take over:

| sha256 | patch |
|---|---|
| `c477e012d64d05e0652e366bebc597e919af6ec64a02e794d471cd5188b0aafd` | `0001-ui-sdl2-QEMU_SDL_POLL_FOCUSED-0-is-off.patch`: `QEMU_SDL_POLL_FOCUSED=0` is off, as `QEMU_SDL_ZERO_COPY=0` is |
| `aaa485f67e3477d0b2fe29eb4ce8cc2ca52802ab659c4cc2d2b2d304398ebd5b` | `0002-virtio-vga-forward-the-display-s-frame-feedback.patch`: virtio-vga and virtio-vga-gl pass the display's frame feedback (the research series' `gl_presented`, `gl_flush_pending`, `gl_flush_done`, `gl_flush_fenced`, `gl_zero_copy`) to the virtio-gpu inside, as they pass the other hooks: without it the guest's vblank is not locked to the host's and its frames are never shown as they are (zero copy) with virtio-vga-gl, the card of new Linux VMs |

## virglrenderer (upstream at `aafa9bd2`)

The research's virglrenderer: the commit and the patches at the top of
`virglrenderer/` are those of its `patches/virglrenderer/main/` (the
`virglrenderer.spec` there, commit `5f7e6af`), byte-identical;
`virglrenderer/vitrine/` holds Vitrine's own, applied after them. Until
2026-10-14 Vitrine had `cf6c62da` (main of 2026-09-06) with the research
export's copies, whose 0003 called a function `aafa9bd2` renamed
(`vrend_get_iovec_size`, now `virgl_get_iovec_size`); the commits between
are fixes (i915 native context's checks of the guest's input, fences
exported before the sync thread takes them).

| sha256 | patch |
|---|---|
| `515120f1b7ddd08c8101acbf8d02c4fc52c147505f1010894ae99246992a98ed` | `0001-xe-native-context.patch`: cmspam's Intel Xe native context, rebased on main; single-batch exec only (2026-10-04: with more batches the kernel reads the guest's `address` as a pointer into QEMU) |
| `0f629fef2724244d1900c707e24218eb91bd97dabd3d42dcfc3658a64fb6bcc5` | `0002-amdgpu-force-wc.patch`: write-combined mappings for host-visible amdgpu blobs |
| `57bcf33419a68ae0523a12c7d66c8e84e9d4e8f87fc1d3ed3356fe75e896beaa` | `0003-guest-dmabuf-api.patch`: `virgl_renderer_resource_set_guest_dmabuf()`, for QEMU's zero copy of guest dma-bufs |

### `virglrenderer/vitrine/`

Vitrine's own, on top, for the research side to take over:

| sha256 | patch |
|---|---|
| `88c80b43864f8b875f83b631e68092d34b462762c5e713fa092f2353aa8e0323` | `0001-xe-sync-only-execs-pass-again.patch`: Xe execs without a batch buffer pass again (0001's check refused them too): Mesa sends them to wait for a queue to go idle, and ANV loses its device when they fail |

Check: `cd host/patches && sha256sum qemu/*/*.patch virglrenderer/*.patch virglrenderer/*/*.patch`.
