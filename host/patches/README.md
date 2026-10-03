# Patches of Vitrine's QEMU

`host/build.sh` fetches each component at the commit `host/versions.conf`
pins and applies `patches/<component>/*.patch` in name order with
`git apply`. The patches come verbatim from the research export
(`~/Documents/qemu-gui-experimental`, exported 2026-10-03: `qemu/patches/`
and `host/virglrenderer/`); keep them byte-identical to it, and add
Vitrine's own changes as new files (`1xxx-vitrine-*.patch`) rather than
editing these.

## qemu (on the qemu-gui fork at `49fd5067`)

0005-0007 are plain diffs (no `diff --git` headers): they were uncommitted
in the fork, so these files are the only copy.

| sha256 | patch |
|---|---|
| `0783af2d0155954962208a1a07b96b1effb8ff84c56523b67b4e939c55c3e783` | `0001-virtio-gpu-guest-vblank-locked-to-the-host-s-present.patch`: guest vblank from the host's presentation (`x-host-vblank`, `x-vblank-lead`, `x-vblank-lead-auto`, `x-vblank-swap-target`), flush fences for blob scanouts, a blob page-array leak fix |
| `d164d36e16e31e7aa2372383238852f1098f97b4f0add80158299a04b33ff776` | `0002-machine-x-vcpu-priority-QMP-command.patch`: QMP `x-vcpu-priority` |
| `a368d85f3edc4e71f2944ad17f4d1881e4d447b3d66cf5b845b72aaf21e979d0` | `0003-ui-sdl2-own-Wayland-swapchain-for-the-guest-s-frames.patch`: the SDL window's own Wayland dma-buf swapchain, frame statistics |
| `5f21c5dfa40b362de6948b39c069bb2867f4dd9b1118b0c2d43df2e7d0edd22f` | `0004-ui-dbus-presentation-feedback-from-the-D-Bus-display.patch`: D-Bus `Presented` feedback, `Listener.Unix.AsyncUpdate` |
| `db9294a8d7f3d3b7f58577d00be0c5b2d7cd945f429c07d7a69e84c29998ec34` | `0005-zero-copy-that-holds.patch`: zero copy with held flush fences |
| `b720bb1a5400594737462850853c00a2df148a170ccef9ebce94d1f79d55d683` | `0006-zero-copy-default-full-screen-tiled.patch`: zero copy by default in full screen, tiled buffers with explicit modifiers, `x-vblank-swap-target-zc` |
| `ca271b7d73b18402c826de59b28125c518e2ec574881a9370b1b70c956d4ee8e` | `0007-ui-dbus-zero-copy.patch`: zero copy for the D-Bus display (`Listener.Unix.ZeroCopy`, `ScanoutDMABUF2`) |

Vitrine's own, on top, for the research side to take over:

| sha256 | patch |
|---|---|
| `c477e012d64d05e0652e366bebc597e919af6ec64a02e794d471cd5188b0aafd` | `1001-vitrine-ui-sdl2-QEMU_SDL_POLL_FOCUSED-0-is-off.patch`: `QEMU_SDL_POLL_FOCUSED=0` is off, as `QEMU_SDL_ZERO_COPY=0` is |

## virglrenderer (upstream at `cf6c62da`)

| sha256 | patch |
|---|---|
| `936e47b0becee1151a0a170003a06b9932c3afd9454134f8568c498d4b53e85d` | `0001-xe-native-context.patch`: cmspam's Intel Xe native context, rebased on main |
| `0f629fef2724244d1900c707e24218eb91bd97dabd3d42dcfc3658a64fb6bcc5` | `0002-amdgpu-force-wc.patch`: write-combined mappings for host-visible amdgpu blobs |
| `b2c05277f960fa91ffcd2fd90389b96cc62febb3753bc31153e9ff80114f0709` | `0003-guest-dmabuf-api.patch`: `virgl_renderer_resource_set_guest_dmabuf()`, for QEMU's zero copy of guest dma-bufs |

Check: `cd host/patches && sha256sum qemu/*.patch virglrenderer/*.patch`.
