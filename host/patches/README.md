# Patches of Vitrine's QEMU

`host/build.sh` fetches each component at the commit `host/versions.conf`
pins and applies with `git apply` the `*.patch` files of
`patches/<component>/` and of its folders, in the C order of their paths
(`LC_ALL=C sort`): a folder's patches in name order, the folders in name
order. Folders deeper down, hidden files and other names are not applied.

## qemu (upstream at `3876503f`)

`host/versions.conf` pins upstream QEMU
(`https://gitlab.com/qemu-project/qemu.git`), and three series go on top,
in this order:

1. `qemu/fork/`: the qemu-gui fork's own commits
   (`https://github.com/tomtomnet/qemu-gui`), one patch per commit, made
   with `git format-patch`. Do not edit them: export them again (below).
2. `qemu/research/`: the research series, verbatim from the research export
   (`~/Documents/qemu-gui-experimental/qemu/patches/`, exported
   2026-10-04), made on the fork at `49fd5067`. Keep them byte-identical to
   it.
3. `qemu/vitrine/`: Vitrine's own changes, as new files here rather than
   edits of the others.

### `qemu/fork/`

The fork's master at `49fd5067` (2026-09-28) is upstream plus 14 commits,
one of them a merge of upstream's master at `3876503f` (2026-09-24). The
13 others, rebased in order onto `3876503f` (no conflicts: the merge had
none either), are these patches, authors, dates and messages kept.
Applied to `3876503f` they give the fork's tree at `49fd5067` exactly.

| sha256 | patch |
|---|---|
| `51acdbda4a77b5bf35f6fd045614760f31bbea3b47e2accaa02317f40af8b6de` | `0001-ui-sdl2-follow-the-host-refresh-rate-tolerate-grab-b.patch`: fork `a02773069` |
| `042edce8b0bebdc57fbc7c35ea94c2f434236683203807fec83d962d98954995` | `0002-ui-sdl2-add-an-in-window-control-menu-Dear-ImGui.patch`: fork `43da62cd9` |
| `38395df6a9e84fb972a3d3a399f20f087ae83d8107f7dd94c6cff24b6953e87d` | `0003-Add-a-README-for-this-fork.patch`: fork `3262ce510` |
| `5914bd2de9bcfba9bccfc5e1ce361044cc8ef970e43724e0eefaa2591279fd84` | `0004-ui-sdl2-draw-the-menu-bar-without-a-border.patch`: fork `beb1755e7` |
| `a4b36899d0f66bb9da1f620287b6d3af7029619f5b943b2f2a46fa26ef90ce9a` | `0005-ui-sdl2-share-the-clipboard-with-the-guest.patch`: fork `8542f5d13` |
| `5706cfcc3072fa490c56daaf6bb0fb9314b0fecf2377f767e84373446022c1be` | `0006-contrib-add-vdagent-clipboard-bridge-for-Wayland-gue.patch`: fork `c734fb809` |
| `585d0c0c3b7a283eeafe4152432f6ab2b975e5d4213da33cfacee40a2b1d65a0` | `0007-contrib-vdagent-clipboard-bridge-wait-for-XWayland-f.patch`: fork `5466c0def` |
| `54deeda9117c7ede0cb918dbf57e4d65c0ba194e8e84ab7dd9ffdf728a044411` | `0008-hw-display-virtio-gpu-gl-tell-whether-the-guest-uses.patch`: fork `b4b8069b0` |
| `a9125a4d87b5aeabace62daa84bca9c7281a67be1dcd0f14e070ac048ed7981a` | `0009-virtio-gpu-gl-disable-the-scanouts-on-reset-in-the-m.patch`: fork `10d20004e` |
| `5855e1c29a2e85bdd5a76e13fad90442d26198707eecf80ff5d030886ffdf3ee` | `0010-virtio-gpu-virgl-give-guest-memory-blobs-a-udmabuf-f.patch`: fork `c2e352447` |
| `2a0a1f04314a4a66a5fe0ea49fe2c743857b538df8694b3ed604db5ce8c48bb0` | `0011-zero-copy-the-virglrenderer-patch-this-branch-needs-.patch`: fork `352fd8d69` |
| `2087f47eefefafb18544c9887a39b6bd5ad3454e9aec36cbf59a3ef507052175` | `0012-ui-sdl2-frame-and-input-latency-stats-x-query-displa.patch`: fork `b45f54913` |
| `372c36695769c099cba66fd2ba3354352ae76665799a96862ae7a4a014e8fa9a` | `0013-README-zero-copy-and-frame-statistics-are-in-master.patch`: fork `49fd50675` |

When the fork moves, export its commits again, on the upstream commit it
last merged (`v` is this repository):

```sh
v=~/Documents/vitrine
git clone --no-checkout https://github.com/tomtomnet/qemu-gui.git qemu-gui
cd qemu-gui
git fetch https://gitlab.com/qemu-project/qemu.git master:upstream
base=$(git merge-base origin/master upstream)
git switch -c series origin/master
git rebase --onto "$base" "$base"          # the fork's commits, without its merges
git diff --quiet origin/master series && echo "the fork's tree"
rm -f "$v"/host/patches/qemu/fork/*.patch
git format-patch --no-numbered --zero-commit --no-signature --full-index \
    --base="$base" -o "$v/host/patches/qemu/fork" "$base"..series
sed -i "s/^QEMU_COMMIT=.*/QEMU_COMMIT=$base/" "$v/host/versions.conf"
```

The same commits give the same files, from any clone (`--full-index`
writes whole blob IDs: the short ones grow with the number of objects in
the clone), with git's diff and format-patch settings left at their
defaults. A commit that conflicts with upstream is resolved as the fork's
merge resolved it (`git show --remerge-diff <merge>` shows how). Then
update the table above, and check that the research and Vitrine series
still apply: build.sh stops at the first patch that does not (`... does
not apply`).

### `qemu/research/`

0005-0007 are plain diffs (no `diff --git` headers): they were uncommitted
in the fork, so these files are the only copy. 0008 (2026-10-04, the
fixes of the research side's static review) goes on top of them. On
upstream with `qemu/fork/` they apply to the same tree as on the fork.

| sha256 | patch |
|---|---|
| `0783af2d0155954962208a1a07b96b1effb8ff84c56523b67b4e939c55c3e783` | `0001-virtio-gpu-guest-vblank-locked-to-the-host-s-present.patch`: guest vblank from the host's presentation (`x-host-vblank`, `x-vblank-lead`, `x-vblank-lead-auto`, `x-vblank-swap-target`), flush fences for blob scanouts, a blob page-array leak fix |
| `d164d36e16e31e7aa2372383238852f1098f97b4f0add80158299a04b33ff776` | `0002-machine-x-vcpu-priority-QMP-command.patch`: QMP `x-vcpu-priority` |
| `a368d85f3edc4e71f2944ad17f4d1881e4d447b3d66cf5b845b72aaf21e979d0` | `0003-ui-sdl2-own-Wayland-swapchain-for-the-guest-s-frames.patch`: the SDL window's own Wayland dma-buf swapchain, frame statistics |
| `5f21c5dfa40b362de6948b39c069bb2867f4dd9b1118b0c2d43df2e7d0edd22f` | `0004-ui-dbus-presentation-feedback-from-the-D-Bus-display.patch`: D-Bus `Presented` feedback, `Listener.Unix.AsyncUpdate` |
| `db9294a8d7f3d3b7f58577d00be0c5b2d7cd945f429c07d7a69e84c29998ec34` | `0005-zero-copy-that-holds.patch`: zero copy with held flush fences |
| `b720bb1a5400594737462850853c00a2df148a170ccef9ebce94d1f79d55d683` | `0006-zero-copy-default-full-screen-tiled.patch`: zero copy by default in full screen, tiled buffers with explicit modifiers, `x-vblank-swap-target-zc` |
| `ca271b7d73b18402c826de59b28125c518e2ec574881a9370b1b70c956d4ee8e` | `0007-ui-dbus-zero-copy.patch`: zero copy for the D-Bus display (`Listener.Unix.ZeroCopy`, `ScanoutDMABUF2`) |
| `d067663b85667b28f5c1dd5929cc56eaf6651f6f325e8dfa713630cafb95f86d` | `0008-ui-virtio-gpu-fixes-from-a-static-review-of-the-seri.patch`: fixes from a static review: a guest buffer described larger than its dma-buf is copied, not attached (SDL) or offered for zero copy (D-Bus), as the compositor would end the window with a protocol error; the vblank tick's catch-up in one step; held flushes released from a bottom half, not while the SDL window draws; a deferred flush no longer strands its command; held fences dropped on reset; SDL's swapchain no longer closes plane fds twice (and fd 0); the D-Bus listener's layout lifetime and fd 0; the refresh rate known at the SDL window's creation reaches the guest |

### `qemu/vitrine/`

Vitrine's own, on top, for the research side to take over:

| sha256 | patch |
|---|---|
| `c477e012d64d05e0652e366bebc597e919af6ec64a02e794d471cd5188b0aafd` | `0001-ui-sdl2-QEMU_SDL_POLL_FOCUSED-0-is-off.patch`: `QEMU_SDL_POLL_FOCUSED=0` is off, as `QEMU_SDL_ZERO_COPY=0` is |

## virglrenderer (upstream at `cf6c62da`)

From the research export's `host/virglrenderer/`, byte-identical.

| sha256 | patch |
|---|---|
| `515120f1b7ddd08c8101acbf8d02c4fc52c147505f1010894ae99246992a98ed` | `0001-xe-native-context.patch`: cmspam's Intel Xe native context, rebased on main; single-batch exec only (2026-10-04: with more batches the kernel reads the guest's `address` as a pointer into QEMU) |
| `0f629fef2724244d1900c707e24218eb91bd97dabd3d42dcfc3658a64fb6bcc5` | `0002-amdgpu-force-wc.patch`: write-combined mappings for host-visible amdgpu blobs |
| `b2c05277f960fa91ffcd2fd90389b96cc62febb3753bc31153e9ff80114f0709` | `0003-guest-dmabuf-api.patch`: `virgl_renderer_resource_set_guest_dmabuf()`, for QEMU's zero copy of guest dma-bufs |

Check: `cd host/patches && sha256sum qemu/*/*.patch virglrenderer/*.patch`.
