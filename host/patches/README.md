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

## qemu (upstream at `c61ea024`)

`host/versions.conf` pins upstream QEMU
(`https://gitlab.com/qemu-project/qemu.git`), and `qemu/series` lists the
patches that go on top, in this order:

1. `qemu/research/`: the research's QEMU series, its `patches/qemu/series`
   at commit `2710b05` (2026-10-27), each patch without its
   `Claude-Session:` line and otherwise unchanged. Since 2026-10-14 this
   series is the source of the research's QEMU; the qemu-gui fork
   (`https://github.com/tomtomnet/qemu-gui`) it started from is no longer
   used. 0001-0017 are the fork's commits, 0018-0024 the work the fork had
   not committed on 2026-10-12, split by topic (0020 left when upstream
   took it), and the last eleven are for `virtio-gpu-rutabaga`, the nested
   desktop's device.
2. `qemu/vitrine/`: Vitrine's own changes, as new files here rather than
   edits of the others.

### `qemu/research/`

| sha256 | patch |
|---|---|
| `521d7c619f6836b90f3b2113d6b82e3db3ec5ea9e2397c333fc9db86da3b2d5b` | `0001-ui-sdl2-follow-the-host-refresh-rate-tolerate-grab-b.patch` |
| `4c52aae7e7f078e5dd4af793cc9f30926ba4a73ae5db3809da981683220b3a05` | `0002-ui-sdl2-add-an-in-window-control-menu-Dear-ImGui.patch`: the menu 0022 removes again |
| `b1b927381170bf2056e012903a31804cf07c90956db41c1d5a5836ab20578e9a` | `0003-Add-a-README-for-this-fork.patch` |
| `183f9f34b0f27d783d27787df5d32d96889429e6fbc7792c214e939d978c8109` | `0004-ui-sdl2-draw-the-menu-bar-without-a-border.patch` |
| `1cd67882a955602fa9145bf59746ea0659a258d20a2e99289b194c4a386e496f` | `0005-ui-sdl2-share-the-clipboard-with-the-guest.patch` |
| `b3b1b9fdd9b403b30c13f1051de170743fcc25ae15e886c94eb1c3a40b4b6102` | `0006-contrib-add-vdagent-clipboard-bridge-for-Wayland-gue.patch` |
| `6bc641b70437d45e8ea2aedbdf48ddf8511a9dd78edb5774a83bf70b7cd14494` | `0007-contrib-vdagent-clipboard-bridge-wait-for-XWayland-f.patch` |
| `c8b652f45275697272651d4357fa4b5193781b491fdc54acb205fd6000fe2520` | `0008-hw-display-virtio-gpu-gl-tell-whether-the-guest-uses.patch`: `x-drm-offered`, `x-drm-contexts`, `x-virgl-contexts` |
| `bc15cbbdc77dd5f1c962773afc90286dcd8b53cd6d09695393d7f8772052907c` | `0009-virtio-gpu-gl-disable-the-scanouts-on-reset-in-the-m.patch` |
| `6fd23105ea52f009efc16ff6eb31a3b6fceb1c4473c76736ff52a3c51f362600` | `0010-virtio-gpu-virgl-give-guest-memory-blobs-a-udmabuf-f.patch` |
| `1625a761dd0bd1a4531ed44b067c5b898e80ea341db7dbfc6ada4779cbdec4bb` | `0011-zero-copy-the-virglrenderer-patch-this-branch-needs-.patch` |
| `c273fa5830838d7f718fb9438aed78631df6948b19fa63afa6d3938cd37aacf7` | `0012-ui-sdl2-frame-and-input-latency-stats-x-query-displa.patch` |
| `1fefe2f2fc934aa83bb735871df79e59ec0b2abb46f0dde204aa339e4a545e06` | `0013-README-zero-copy-and-frame-statistics-are-in-master.patch` |
| `1f46a94fe8f10f82f93aa5300dd746eee8025e850bc02d811e21dda7e2c73ae9` | `0014-virtio-gpu-guest-vblank-locked-to-the-host-s-present.patch`: `x-host-vblank` and the vblank lead and swap targets |
| `cb72a5baecf1626211e79d6a054e8e8077a059a43c7f84ce8a259cebf898aefa` | `0015-machine-x-vcpu-priority-QMP-command.patch` |
| `edd1d14cb1c7a6ac931706559183a120b5b0c282a45edc7760d5a26491acca63` | `0016-ui-sdl2-own-Wayland-swapchain-for-the-guest-s-frames.patch` |
| `a61e1259f19b9f137246bcb17b9484274d0c54adcea1e2548c7e3762dd4b8326` | `0017-ui-dbus-presentation-feedback-from-the-D-Bus-display.patch` |
| `466ce2bbf2384ba523954e6c27967d9871adeca3905516605a7ab709b541b00f` | `0018-kvm-per-VM-halt-poll-ns-the-x86-slot-zap-all-quirk-a.patch`: KVM's per-VM `halt-poll-ns`, the x86 slot zap knob (`slot-zap-all`), a `KVM_PRE_FAULT_MEMORY` helper |
| `38981beefd0be259725583447abf779c7eb57d0ea360ab59e629c7d43562ddd9` | `0019-physmem-align-memfd-backed-RAM-so-that-shmem-gives-i.patch`: memfd guest RAM aligned for shmem's huge pages |
| `de4390b20499720bf05fc1c372cfe19b998192c41054b46c1fa2b46eb77598c5` | `0021-ui-dmabuf-layout-a-guest-dma-buf-s-layout-from-its-a.patch`: tiled buffers, a guest dma-buf's layout from its amdgpu metadata (`ui/dmabuf-layout.c`) |
| `f57d7526717381b79a859aa603a61070ba6e21e44e954217cd09215092848045` | `0022-ui-virtio-gpu-zero-copy-with-held-flush-fences-and-h.patch`: zero copy that holds the guest's flushes, the D-Bus display's zero copy, the host's latching deadline (`x-vblank-deadline`), virtio-gpu-rutabaga's 512-entry control queue; and the SDL window's ImGui menu removed |
| `f51477e5cfcc1fbaab6869a837b0020fca4b39362f5bf8f6fa1912c973101164` | `0023-virtio-gpu-rutabaga-native-context-capsets-fixed-blo.patch`: virtio-gpu-rutabaga's additions: drm and virgl2 capsets, `x-fixed-blob-mapping`, `x-guest-dmabuf`, `x-prefault-blobs`, rutabaga's poll descriptor, the fence ring index as a byte |
| `85e119375bc2692e2359f0d2c17d683a99a8d1ab482756f38d31caf387adb8ab` | `0024-README-the-fork-as-of-2026-10-12.patch` |
| `f2ca5e6090a29bfadb29e940368227811cc618aa1b34a5c5c30007c6efb0be8d` | `fix-rutabaga-scanout-image-leak.patch` |
| `6572397833517b8e19675ac556935458bed10be5208f36150ec28e5573cf01d3` | `virtio-gpu-rutabaga-scanout-blob.patch`: `SET_SCANOUT_BLOB` on displays with GL: the guest's GPU-rendered login screen on its own console |
| `7241c150b19b49f6c598f3b3a527b48f5bcb8ad66e43123e29bb7f0fa8c1bce6` | `virtio-gpu-rutabaga-gl-context-scope.patch`: virglrenderer's GL context kept apart from the display's, so that the VGA and virtio-gpu consoles show with `-display dbus,gl=on` |
| `b31e726f02fd8ec2955092744453b7b802bfd51bd9720a60844eccaddf36a429` | `virtio-gpu-rutabaga-reset-contexts.patch`: a reset the guest did not see coming (QMP `system_reset`, Machine > Reset) destroys the guest's rutabaga contexts, so that the next boot's contexts do not meet stale ones of the same ids; the errors a guest can cause logged at most 20 a second |
| `4886a52e553e50b3e40f0a3f3169b0a674281435c41e0fb36e7cd2a24e1be09b` | `virtio-gpu-rutabaga-prefault-async.patch`: `x-prefault-blobs` queues the blob pre-fault on a vCPU (`async_run_on_cpu()`) instead of waiting for it in the command bottom half, which holds the device's re-entrancy guard: the guest's MSI-X table writes meanwhile (an interrupt moved to another CPU) were dropped ("Blocked re-entrant IO") |
| `4fe7c201777c4eb794dcf2f1751f5ce1e21e4b5c554d4793c31e8c66bf3ffc5c` | `virtio-gpu-rutabaga-gl-scope-blob-maps.patch`: blob map, unmap and export, and `GET_CAPSET`, also run in virglrenderer's GL context: with a GL display (`-display dbus,gl=on`, Vitrine's window) every virgl blob map failed (-22), and a guest whose Mesa uses virgl, such as the stock Fedora 44 KDE live ISO, stayed black |
| `45d3af19c0978edc7f573496234c2700c4385e94dced2e2c9e679eb2a0f9780e` | `virtio-gpu-rutabaga-render-node.patch`: `x-render-node=PATH`, the host GPU the contexts render on: the render node goes to rutabaga as its GPU channel, and virglrenderer renders on it, vrend and the native context alike (unset: virglrenderer's first, amdgpu before i915 and xe); checked at realize: a path that is not a `/dev/dri/renderD*` QEMU can open read-write stops QEMU with the reason, where virglrenderer would take its first GPU without a word |
| `028021d21483538f8a570aca800ae9cec0d69408fb9bb8cd3a6b380caac987ce` | `virtio-gpu-rutabaga-2d-partial-updates.patch`: `TRANSFER_TO_HOST_2D` passes the box's offset in the guest's buffer, which virglrenderer copies from: every partial update came from the buffer's start (no '*' at a LUKS prompt, garbled boot text); `RESOURCE_FLUSH` reads back only the flushed rectangle, after virtio-gpu's bounds check, where it read back the whole resource (33 MB at 3840x2160 for a line of text or a firmware blit) |
| `b454904909a38abc62b766235dcfedfb95d1017a86585cf10ec781241c2210c7` | `virtio-gpu-rutabaga-2d-shadow.patch`: 2D resources (dumb buffers: the guest's console, a boot splash, a cursor) stay in QEMU's own image until a context or a 3D transfer uses them, as virtio-gpu's 2D device keeps them, where every update went into virglrenderer's texture and came back at the flush (a 4K console scrolling 3,000 lines: 11 uploads and read-backs of 33 MB before, none after); the first 3D use hands rutabaga the whole backing |
| `31c73b86c2f4c4753b4e674b230cc2ea9562f07db5b8bb8403c5918536f38536` | `virtio-gpu-rutabaga-2d-shadow-budget.patch`: the 2D shadow's images count against `max_hostmem` (256 MiB by default), as virtio-gpu's 2D device does, where a guest could have QEMU allocate one per 2D resource: a resource past the budget goes to rutabaga, whose flushes read back as before; without virgl QEMU keeps the 2D resources alone, so that such a device has a console (a guest session on the virtio output still needs `x-virgl2`: blob scanouts and context-0 fences go through vrend) |
| `9d575192c1885c56faf07cc405392a41d04b43394de92a287359f391ea920516` | `virtio-gpu-rutabaga-2d-resource-id.patch`: `RESOURCE_CREATE_2D` refuses an id of 0 or one in use itself, as virtio-gpu's 2D device does: a 2D resource that QEMU keeps alone (virgl off) never reached rutabaga's own check, and a guest could put two resources with one id in QEMU's list |

When the research's series moves (`r` its repository, `c` the commit,
`v` this folder):

```sh
r=~/Documents/virtio-gpu-desktop c=2710b05 v=~/Documents/vitrine/host/patches/qemu
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
research's static review. 0018-0024 hold all of them in their later form
but two things, both on purpose: the SDL window's ImGui menu, with the zero
copy below its docked bar on a subsurface (`viewporter`,
`fractional-scale-v1`), which the fork removed (and with it QEMU's
`sdl-gui` option, which build.sh no longer passes); and 0014's blob page
array leak fix, which the unref leak fix v2 (`virtio_gpu_cleanup_mapping()`)
covers.

Until the research split it (its commit `a381e87`), 0018-0024 were one
patch, `0018-The-fork-s-uncommitted-work-as-of-2026-10-12-to-be-s.patch`
(sha256 `81ceed52...`). The split series gives the same tree: checked when
the reset patch came, the new series giving the tree of the old one plus
`virtio-gpu-rutabaga-reset-contexts.patch` alone.

Until the research's `e06b62f` (2026-10-15), 0012 ended `ui/sdl2-stats.c`
with a blank line, git am's only whitespace warning (sha256 `efd7be38...`):
the series' tree differs from the one before by that line alone.

Until the research's `a4eb00b` (2026-10-15) the series applied to upstream
`3876503f` (master of 2026-09-24) and had
`0020-virtio-gpu-free-a-resource-s-guest-mapping-when-it-i.patch`, the
user's unref leak fix (sha256 `d7729e8e...`), which upstream merged as
`ca5d368f11` ("virtio-gpu: fix memory leaks in virgl and rutabaga"). The
research made five patches again for the new base: 0014 no longer frees
`res->base.addrs` itself (upstream's fix covers it), 0023 keeps comparing
the fence's ring index as a byte on top of upstream's context check, and
0012, 0015 and 0018 changed in their context only. `git am` of its 29
files on `c61ea024` gives tree `991689a4`, the research's, and so do
Vitrine's copies; Vitrine's own two apply on top unchanged.

### `qemu/vitrine/`

Vitrine's own, on top, for the research side to take over:

| sha256 | patch |
|---|---|
| `c477e012d64d05e0652e366bebc597e919af6ec64a02e794d471cd5188b0aafd` | `0001-ui-sdl2-QEMU_SDL_POLL_FOCUSED-0-is-off.patch`: `QEMU_SDL_POLL_FOCUSED=0` is off, as `QEMU_SDL_ZERO_COPY=0` is |
| `491a23d8114ae5a517d39c2070c1302c902ff887a734d450306fe518507d6be6` | `0002-virtio-vga-forward-the-display-s-frame-feedback.patch`: virtio-vga and virtio-vga-gl pass the display's frame feedback (the research series' `gl_presented`, `gl_flush_pending`, `gl_flush_done`, `gl_flush_fenced`, `gl_zero_copy`) to the virtio-gpu inside, as they pass the other hooks: without it the guest's vblank is not locked to the host's and its frames are never shown as they are (zero copy) with virtio-vga-gl, the card of new Linux VMs; virtio-vga-rutabaga, the nested desktop's card, shares `virtio-vga.c` and gets them too |

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

## rutabaga_gfx (upstream at `4fb9f320`)

`https://github.com/magma-gpu/rutabaga_gfx.git` at release 0.1.85, the
research's base, for QEMU's `virtio-gpu-rutabaga` (cross-domain: the
nested desktop). build.sh builds it with meson as the research does
(`-Dffi=true -Dfeatures=virgl_renderer`, rustc and bindgen, its crates as
meson subprojects downloaded in the fetch step) against Vitrine's
virglrenderer, into a folder of its own that QEMU's RUNPATH names.

| sha256 | patch |
|---|---|
| `4b2abee923eba4061fc8c05e369b4fcfacd227b3249721bfbc050108095f954c` | `changes.patch`: the research's `patches/rutabaga_gfx/changes.diff` (commit `b4400b1`) under a description of Vitrine's: native contexts through the FFI, `struct rutabaga_fence`'s ring index as a byte, polling for virglrenderer's fences, a logger, guest dma-bufs, and cross-domain hang-ups: the host end of the guest's Wayland connection going away (the app serving QEMU's socket quit, crashed or was never there) reaches the guest once, as an empty receive, instead of empty receives in a busy loop or no answer at all; a reset of the connection too (an app that died with data it had not read) |

Below its description the file is the research's diff, unchanged:
`sed -n '/^diff --git/,$p' rutabaga_gfx/changes.patch` gives it back.

Until the research's `b4400b1` (2026-10-15), a reset of the connection
halted the cross-domain worker instead ("Worker halting due to: ...
Connection reset by peer"), and the guest never heard of it: an app that
crashed with data it had not read left the session running without a
window (sha256 `59fd9f15...`, their `8ecffaf`).

Check: `cd host/patches && sha256sum qemu/*/*.patch virglrenderer/*.patch virglrenderer/*/*.patch rutabaga_gfx/*.patch`.
