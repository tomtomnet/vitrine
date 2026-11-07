# The host's KDE packages

Optional components of Vitrine's Builds window ("Host desktop"): this
computer's own KWin, KSvg, Kirigami and Qt base, with the research's host
sets. They replace packages the whole desktop runs on, so Vitrine builds
one only when asked, and installs it only after the administrator's
password. Each is a section of `components.conf`; all are built from one
pinned copy of the research's recipe, `research/`.

What the sets change (the research's measurements):

- KWin, sets `hl39y` for Plasma 6.7.91, `hl39-68` for 6.8.0, `h681zy` for
  6.8.1 and later (Fedora 44's and 45's Plasma 6.8.2); none for Fedora 44's
  earlier Plasma 6.7.5. Patch 0082 (damage-free layers) is left out of the
  builds of the sets that have it (`h681zy_no82`): with a host window over a
  VM, Firefox in the guest froze. Keys in a VM take 8.5-9.3 ms against 12.5
  ms with Fedora's KWin; drags and the launcher under load are smoother;
  the guest's frames are paced to the host's deadlines (presentation-time
  v3); the pointer grab (Ctrl+Alt+G) works inside Vitrine's Desktop view
  (patch 0039). On Intel xe GPUs (Arc), KWin no longer makes the GPU read
  every window the CPU draws from system memory at each frame (patch
  0075): with Fedora's KWin and the sets before `hl39y`, Konsole drags
  missed 47 % of the refreshes on an Arc Pro B50. `hl39-68` is the
  research's set of 2026-10-08, without 0075 and the patches after it:
  with an Intel xe GPU on Plasma 6.8.0, set `KWIN_DISABLE_UDMABUF_IMPORT=1`
  (in `~/.config/environment.d`).
- `h681zu` adds three patches to the research's earlier `h681a`:
  - 0090, on: big texture uploads go through one buffer that stays mapped.
    On the Arc Pro B50 at 4K and 240 Hz, the launcher opened after typing in
    Firefox repeated 16 refreshes in ten openings instead of 95, and no
    longer stutters for nine refreshes. `KWIN_UPLOAD_BUFFER=0` turns it off.
  - 0089, off: Night Light after blending (the switches, below).
  - 0083: a file `$XDG_RUNTIME_DIR/kwin-udmabuf-import` holding 0 or 1
    decides the udmabuf import of shared-memory windows in a running
    session; with no file, nothing changes.

  On the research's desktop (Arc Pro B50, Plasma 6.8.2), the bare desktop
  is level with the 6.7.91 sets and VMs drag about 2 ms sooner.
- `h681zy` adds five patches to `h681zu`:
  - 0092: a new texture's big upload goes through 0090's buffer too: a
    CPU-drawn window's first buffer at a new size, after a maximize say.
    `KWIN_UPLOAD_BUFFER=0` turns both off.
  - 0093: a shared-memory window's udmabuf is made only when KWin imports
    one. It pinned each buffer's pages on KWin's main thread (~5.5 ms for a
    4K window on the B50), for nothing on Intel xe, i915 and NVIDIA.
  - 0094: a window's old shared memory is let go on a thread of its own:
    its last close freed ~6.5 ms of pages on KWin's main thread.
    `KWIN_SHM_RELEASE_THREAD=0` turns it off.
  - 0095: the Overview keeps its views between openings.
    `KWIN_OVERVIEW_VIEW_CACHE=0` turns it off.
  - 0096: KWin reads the GPU's clock once a second for its render-time
    queries, not at every frame: on xe each read woke the GPU, ~0.3 ms of
    KWin's main thread a frame. `KWIN_GPU_CLOCK_EVERY_FRAME=1` reads it at
    every frame.

  On the Arc Pro B50 at 4K and 240 Hz, against `h681zu`: a CPU-drawn
  window's maximize (Konsole) has at most 8-13 ms between two frames
  instead of 42, and KWin's main thread runs 5-9 ms at a stretch instead of
  36-49; from its second opening, the Overview shows its first frame after
  17-18 ms instead of 37-45; window operations repeat a refresh 106-111
  times in a slow pass instead of 154-155, and 73-82 times instead of
  96-101 in a fast one (KWin's count). The research's desktop also ran it
  through a night of tests with an RX 6900 XT: no crash or GPU error, and a
  CPU-drawn 4K window's maximize repeated one refresh.

  On an AMD laptop GPU (Radeon 780M, Fedora 45, 4K at 240 Hz), against
  Fedora's KWin 6.8.2: keys in 10.6-11.0 ms instead of 12.8-13.4 (p50);
  window operations repeat 104-115 refreshes a pass against 110-139; Super
  pressed 8 times a second under load is level; no short minimize animation
  in 24 for either. With `h681zu`, a cluster of short minimize and restore
  animations was seen once on Fedora 44 (kernel 7.2.13), and not on Fedora
  45.
- KSvg 6.31.0, set `k2`: Plasma's popups stop uploading their themed frames
  again at each opening.
- Kirigami 6.31.0, set `f1`: its items react to the frame contrast only
  when it changes (Plasma's style set it again at each color sync). With
  KSvg's set, on an Arc Pro B50 at 4K and 120 Hz: the launcher opens in
  61-69 ms instead of 72-78, and Super pressed 8 times a second repeats 0.2
  frames a second instead of 6-9.
- Qt base 6.11.2, set `q4`: a window being resized reuses its shared memory
  pools for the next sizes. Bare 4K resizes: Konsole shows 111-114 sizes a
  second instead of 85-88, Dolphin 99-102 instead of 62.
  `QT_WAYLAND_SHM_NO_REUSE=1` in a program's environment turns it off.

How:

- `build.sh --package PACKAGE` builds one: the research's
  `setup/tools/kde-rpms.sh`, run as it is from `research/`, in a Fedora
  container (rootless podman, memory capped at 10 GiB). It takes the
  source RPM of the package's newest version in the repositories the
  host's Plasma comes from (Fedora's, or the KDE SIG's beta COPR) and the
  research's set for that version (`kde_set_for host` in
  `research/setup/versions.sh`). The research's overrides of its sets
  (`VGD_HOST_KWIN_SET` and the like) and its switches of packages
  (`HOST_QT_Q4=0` and the like) are ignored. The builds go to
  `~/.local/share/vitrine/host-kwin/`, `host-ksvg/`, `host-kirigami/` and
  `host-qtbase/`, a folder per version-release, with the container's logs
  and the packages they were built against.
- `install.sh` installs builds, as root (Vitrine runs it with pkexec): of
  each source package, all its packages installed now are replaced by the
  build's in one dnf transaction, or none if the build lacks one of them
  (Qt base's require each other at the same release); they are checked by
  their header digests, and version-locked by their exact names
  (`dnf versionlock`) so that updates leave them alone. `install.sh
  revert` removes the locks and puts the distribution's packages back
  (`dnf distro-sync`), only for packages Vitrine installed; several at once
  in one transaction.
- Some of KWin's patches act only when a file of `$XDG_RUNTIME_DIR` says so
  (the research's `src/host-config/kwin-*.conf`: the scanout journal's
  variance cap, the active window as a plane candidate, empty renders,
  effects starting one refresh in, damage-free layers). `install.sh` turns
  those of its `kwin_switches` list on for every user, in
  `/etc/user-tmpfiles.d/vitrine-host-kwin.conf` (made at each login; for
  the user installing, at once). To turn one off, remove its line there.
  Reverting KWin removes the file. Damage-free layers (patch 0082) stay off
  since 2026-11-01: with a host window over a VM, Firefox in the guest froze.
- Night Light after blending (patch 0089) is written there off (the
  research's `kwin-night-light-after-blending.conf`, 0). Plasma 6.8's Night
  Light composites every window, which costs 3.3-3.9 ms per key on the Arc
  Pro B50. With the switch on, windows keep their planes at night and keys
  are as fast as by day; white and greys stay exactly the same, but
  saturated colours get the classic per-channel tint instead of 6.8's
  colour adaptation (pure green stays green, darker; KWin 5 and GNOME
  looked like that). To try it in a session, write 1 and turn Night Light
  off and on (KWin reads the file at each colour change):

  ```sh
  echo 1 > "$XDG_RUNTIME_DIR/kwin-night-light-after-blending"
  ```

  To keep it on at every login, a file of your own whose name sorts before
  `vitrine-host-kwin.conf` wins over that line:

  ```sh
  mkdir -p ~/.config/user-tmpfiles.d
  echo 'f %t/kwin-night-light-after-blending 0644 - - - 1' > ~/.config/user-tmpfiles.d/kwin-night-light.conf
  ```
- From Plasma 6.8.2, KWin offers commit timing to programs only with
  `KWIN_WAYLAND_SUPPORT_COMMIT_TIMING_V1=1`. Upstream turned it off by
  default because of frame-time spikes in some games and an NVIDIA driver
  bug that freezes resumed Vulkan videos (KDE bugs 525514 and 527500). VMs
  in the Desktop view pace their frames with it: on an Arc Pro B50 under
  FurMark, a VM's launcher opening missed 0.55 of the 12 refreshes after its
  first frame instead of 4.45, and an idle VM's QEMU used about 0 % of the
  GPU's render engine instead of 18-22 %. `install.sh` sets it
  for every user in `/etc/environment.d/vitrine-host-kwin.conf`, read as
  each user's session starts (for the user installing, at once); earlier
  KWins offer commit timing anyway. A value of your own in
  `~/.config/environment.d` wins: `KWIN_WAYLAND_SUPPORT_COMMIT_TIMING_V1=0`
  keeps it off. Reverting KWin removes the file.
- `components.conf` describes them to the app: which installed package
  says what release the distribution has (`follows`: Plasma for KWin, the
  KDE Frameworks for KSvg and Kirigami) and what a build must match
  (`against`: KWin and Kirigami use Qt's private API).

What the locks mean:

- KWin must match the installed Plasma and Qt, whose updates do not wait
  for a locked KWin. After one, Vitrine says so (a banner in the main
  window) and offers to build and install the matching set, or to revert.
  It runs from the next login.
- KSvg and Kirigami stay at their version while the other KDE Frameworks
  update around them, a mix of releases that upstream does not support.
  When the frameworks move to a new release (each month in Fedora),
  Vitrine says so and offers the matching set if the research has one,
  else to revert. Kirigami uses Qt's private API: after a Qt update it must
  be built again too.
- Qt base holds every Qt update back while it is locked: Qt's other modules
  require the same version of it. The Builds window says when an update
  waits (from dnf's cached metadata); a build of the research's set for
  the new version takes it, or Revert… lets it in. After a Qt update,
  Vitrine's KWin and Kirigami must be built again.
- Programs started after an install use the new libraries, the desktop
  from the next login.

If the desktop does not start, log in on a text console (Ctrl+Alt+F3) and
put the distribution's packages back. For KWin:

```sh
sudo dnf versionlock delete kwin kwin-common kwin-libs
sudo dnf distro-sync kwin kwin-common kwin-libs
```

For the others, the same with the packages the Install… dialog names (`rpm
-qa 'qt6-qtbase*'` lists Qt base's; `kf6-kirigami*` also matches
kf6-kirigami-addons, which is not Kirigami's). Revert… also removes KWin's
switches and commit timing; after KWin is put back from a console, remove
them by hand (the distribution's KWin reads the commit timing variable too):

```sh
sudo rm -f /etc/user-tmpfiles.d/vitrine-host-kwin.conf /etc/environment.d/vitrine-host-kwin.conf
```

## `research/`: the research's recipe, pinned

A copy of what `kde-rpms.sh` reads in the research's repository
(virtio-gpu-desktop) at commit `4ee46dd6c4e6d456efe4a31c84a91a55c7f9db30`
(2026-11-07), in its layout (the container mounts the folder as `/repo`),
byte for byte and with its files' modes: the scripts, the host sets of the
four packages (`kde_set_for host ...` in `setup/versions.sh`) and the
patches they name, and the run-time switches of KWin's patches
(`src/host-config`), which `install.sh` reads. `build.sh` passes it
everything through the environment: Vitrine's folders (`VGD_BUILD`), no
`paths.conf` of the research's (`VGD_CONFIG=/dev/null`), the jobs, the
package (`VGD_KDE_PACKAGES`), the Plasma's origin (`PLASMA`), the host's
Fedora (`VGD_FEDORA`), podman's options (`VGD_PODMAN_ARGS`: the
container's name, which Stop needs, and its memory cap) and live output
(`VGD_VERBOSE=1`). Each build keeps `buildroot-TAG.txt`, the packages it
was built against, which the app compares with the host's before
installing.

| sha256 | file |
|---|---|
| `a333fddbfabbe12d8e5f224d441f174221be8c321d0634936b3dfea4c46d6c2d` | `patches/kirigami/0001-PlatformTheme-signal-a-frame-contrast-change-only-wh.patch` |
| `8d04c83da608399e7db51363d7be6e340568a290c0182981c570b0c996cc013d` | `patches/kirigami/sets/f1.series` |
| `795c21bfd266da8d22a9666c7252b0e865bd5f4e75c9d520059ff686e3c91f27` | `patches/ksvg/0001-Svg-keep-a-cached-pixmap-s-copy-at-the-requested-dev-v2.patch` |
| `012a24af7c7d5e6402f2c455ac2ba1bc0fb4ea3b5c170d70b4c107a90b70c2e1` | `patches/ksvg/0002-FrameSvgItem-release-the-previous-nodes-after-the-ne.patch` |
| `5018b0aa3b29d3c19772cc51433a701a1a1580e84044c749d895cc6a56d4e098` | `patches/ksvg/sets/k2.series` |
| `0d02a6e21c7c8aa7577eef557695bfa2c3bdd86706c1ae62f7b63f3715b811cb` | `patches/kwin/0001-Patch100-kwin-vm-frame-timing-ported-to-6.8.patch` |
| `61c540b5029879629218f527ade8a48605dbc4a560c2e32056b860526dfcf3f0` | `patches/kwin/0002-Patch101-kwin-render-journal-variance-cap-applies-un.patch` |
| `7cc5ce66afed63a27dbb6d8c0d1b56f3c4da975a83fd17339aa40df1a7e51437` | `patches/kwin/0003-Patch102-kwin-late-frame-scheduling-ported-to-6.8-re.68.patch` |
| `8b75a6a1c58fe1a90d7fd418eecfb463ffc5413a847f3e30282cee3c3cb99e6f` | `patches/kwin/0003-Patch102-kwin-late-frame-scheduling-ported-to-6.8-re.patch` |
| `427a63333f8fb461fde2528addd8cb47376cd4ff8935ae4290d6ce56f80a8848` | `patches/kwin/0004-Patch103-kwin-late-flip-compensation-ported-to-6.8-b.patch` |
| `3ca0bc02b7703d5d92bb28e72e662e3e60fca1ae7bfe846adaf24cac5a566c80` | `patches/kwin/0005-Patch104-kwin-cursor-frames-estimate-ported-to-6.8-L.patch` |
| `1422c6baad0b0149ef9013feda199ee713d34fc1e2fb0b1d1aaaca02fe059d79` | `patches/kwin/0006-Patch111-kwin-scanout-frame-slack-applies-with-offse.patch` |
| `a81c5471ed1c3a9eedf71b9319321acd22d76c0635daa1dac1ec1683b74e049b` | `patches/kwin/0007-FIX-A-Patch102-read-scheduledLate-before-the-commit-.patch` |
| `bf51b1f59a0707876e42e9dee1ae578028184bb30ac38ab62af8fd09cfc5fe16` | `patches/kwin/0008-FIX-E-Patch103-111-renderedByCompositor-from-the-lay.patch` |
| `e167a6eca588a715eca608d23ca90878eba47dc328f96208bf6772d8e4b17420` | `patches/kwin/0008-upstream-maximize-size-tolerance.patch` |
| `0579945143f039832773f7fd862cd604659939365f25ccf77142f797a36a1f68` | `patches/kwin/0009-FIX-F-renderloop-the-2-ms-render-time-floor-only-for.patch` |
| `040869c66d3e8fade70d8f38b4f1c990b6a9d9d1050318066cf5c87170f4eacd` | `patches/kwin/0010-FIX-G-renderloop-commit-timing-target-sets-the-earliest-vblank.patch` |
| `e3b01390318bb995147b65772051d2972fdaebe5bdf9c5ad51279442ee9c07a9` | `patches/kwin/0012-presentation-time-v3-content-update-deadline.patch` |
| `774bf1a2abb37b3accfd5a07e15e1615065d2916e0d86b3aa6a048f1ef4a88e1` | `patches/kwin/0038-FIX-window-activate-raise-and-unrestricted-resize-is-unrestricted.patch` |
| `727678c258456da0e67cf2db0dd6501f05471f74c3e5d2aefc9d9c6156b140a8` | `patches/kwin/0039-EXP-input-pointer-constraints-on-subsurfaces.68.patch` |
| `1621f555a8ecd3595a534778c737aea6a4956845a7624d779674eea2596cb102` | `patches/kwin/0039-EXP-input-pointer-constraints-on-subsurfaces.patch` |
| `87e0964e9c62fea59a7e02c7a60df15d1a967cd9f13928aa7d5e357e78f75d24` | `patches/kwin/0060-EXP-interactive-move-once-per-frame.68.patch` |
| `4550104619285972c8b59768972dd7974463f28e2bdd725810ae1e99e4e852f7` | `patches/kwin/0060-EXP-interactive-move-once-per-frame.patch` |
| `dfb62f39fd1b974f0578b04ef46dbff45730880dee43b8c46ac578e162a625fa` | `patches/kwin/0061-EXP-plasma-window-geometry-once-per-pass.patch` |
| `b4d4b2fadec11d1778afac5efe345846efec5722780351fc196ea39f5bf9c560` | `patches/kwin/0062-EXP-cursor-async-test-once.patch` |
| `9ff1dfab9937ccef2761a6a470f944887da608f259eb99dddae797dc260c8b47` | `patches/kwin/0063-EXP-pointer-motion-to-clients-once-per-ms.patch` |
| `77e23df8ec1ac727f8071e84f4218125a6a3c56bcf79692fd1a80fb8dc5fd10e` | `patches/kwin/0066-FIX-renderloop-a-render-journal-of-its-own-for-direct-scanout.patch` |
| `9632c39dc81d0f01ffe5da4c2327f9615444b93f538c20e503c2432ce9ba1c28` | `patches/kwin/0068-UPSTREAM-e3f5caef-pointerconstraints-no-commit-for-lock-or-unlock.hl.patch` |
| `c0155baae94d01e5c615ba304c4a7278347cbaac8a71c51b9fbdbd26ba201bf5` | `patches/kwin/0070-FIX-drm-keep-both-layer-surfaces-when-switching-back.patch` |
| `e192e81e652926ea735b0a275c69a5ac2c3230e9b366a53737dd60551a3f4382` | `patches/kwin/0074-EXP-renderloop-lone-frame-lead-hl.patch` |
| `557137af68be546c9c66cd78da147f54b056d39a9d654526e7f106962d56cb42` | `patches/kwin/0075-FIX-opengl-no-udmabuf-import-on-Intel-xe-either.patch` |
| `8fceb6e1df83b20a0d5e15cb4236d31e05a8928fb05fd915f4e6f04b1fd7571a` | `patches/kwin/0077-EXP-effects-first-animation-frame-one-refresh-in-at-run-time.68.patch` |
| `32b5598cb6e643d11e56bbd0466a4816fc8ea6a42d3cefa4aa569d1cce407916` | `patches/kwin/0077-EXP-effects-first-animation-frame-one-refresh-in-at-run-time.patch` |
| `0384aba1a9109ae2b2e1e58fd7d68b411136467b9d306764fa5469281a2c3da6` | `patches/kwin/0078-EXP-renderloop-scanout-journal-variance-cap.patch` |
| `5c5b49f1e59b4332adcec6b5c8433f20b296657ccd996138eef7646f108be75f` | `patches/kwin/0079-EXP-renderloop-scanout-frame-slack-at-run-time.patch` |
| `31673bd7249b7eb8609e2fb97820f9dc29f5d07636203a32ee6f8bd95f3059af` | `patches/kwin/0080-EXP-scene-the-active-window-a-layer-candidate-at-run-time.patch` |
| `9a8b47ba2feab7753103975e1e8d8cc9a40762146a5efd0972400489bd0edea1` | `patches/kwin/0081-EXP-compositor-an-empty-render-is-not-content-at-run-time.patch` |
| `5ca98067b01dba5dc8c9b9c7804d1726d937f19c3fc73ed3b77d89c4636f8642` | `patches/kwin/0082-EXP-compositor-a-layer-with-no-damage-keeps-its-buffer-at-run-time.patch` |
| `318443465694ad35a85d1f5686e8a34837abadeb9ef2eb96bd3b2b31394d0f18` | `patches/kwin/0083-EXP-opengl-udmabuf-import-at-run-time.patch` |
| `a2cafc576a9bcce8b7064daba66578b71f7a71f3fd9350cc7c90e3ebae933c2a` | `patches/kwin/0089-EXP-drm-night-light-after-blending-at-run-time.patch` |
| `bc91ee813206b23353a848555ee9fc62a5c7b71b837291b98a215408f42b3c6c` | `patches/kwin/0090-EXP-opengl-persistent-upload-buffer-for-big-texture-uploads.patch` |
| `18dd95d05d04bfdb04810403663e216ea986c9b7870cf1795fbc904ab1f9f6e7` | `patches/kwin/0092-EXP-opengl-new-textures-big-uploads-through-the-upload-buffer.patch` |
| `b497dca1af670d1795671a0a639ac6511c0a62748284a50664bea808928b622d` | `patches/kwin/0093-FIX-wayland-a-wl_shm-buffer-s-udmabuf-made-when-first-asked-for.patch` |
| `0ccdf78c3fcc9c50bc7902989d4ca925ffe8d47336e1d1072d7753ccd6d888d6` | `patches/kwin/0094-EXP-wayland-a-client-s-shm-memory-let-go-on-a-thread-of-its-own.patch` |
| `90ffb02e25ac75ab1e305d3592e2cacfa262b69d8c6a2251cdd578b4c4cdf30e` | `patches/kwin/0095-EXP-overview-keep-the-views-between-openings.patch` |
| `1afe99bbcd5b04da7865274f9256cffa6df922b51c0eb40666fb4be893bef515` | `patches/kwin/0096-EXP-opengl-the-GPU-s-clock-read-once-a-second-for-render-time-queries.patch` |
| `c43b05c3c6c2fc11a460184fd73046399c57df4360f80c17a10230ff69d1fd38` | `patches/kwin/sets/h681zy.series` |
| `c18c004fe7319f2d4509deeadf37df8434088affde845d0eb96b11bb6642367d` | `patches/kwin/sets/hl39-68.series` |
| `45da20f38c96aeb3d44540c0567de26c293e6aba799b1e64e01eebb79ee7ad11` | `patches/kwin/sets/hl39y.series` |
| `bdfcd81c8c252a31ffad2f62bd99e793a0c6d0d0300c5704d74ee0ccba454b25` | `patches/qtbase/qtbase-wayland-shm-reuse-pools-v3.patch` |
| `e1c33999e3ea401a4a5d4017023d34d85962c318c993e3bf5804799baa2ab3b3` | `patches/qtbase/sets/q4.series` |
| `475c0f28931454124716683c6c068a173fefc458d5d28a00c28961a629b086f2` | `setup/lib.sh` |
| `761707846b55e70bac195d24e30520ca5c7b96164c0ce45b113317fc13150deb` | `setup/paths.sh` |
| `fa2026f1f4b365859cf919667685f23bb7d6988ec8906dbdf2c8831428750182` | `setup/tools/build-patched-rpm.sh` |
| `044bf5dc522dc2ae268ae734bf0389569eff2ef095f2ddd48ddd270488656bdb` | `setup/tools/kde-rpms.sh` |
| `8868c2cecf05cf8fa0f8695dd05d0bfcd3ea4917935a0cae594a63ff77e89816` | `setup/versions.sh` |
| `19cf4324887903cc1185fc8dc83d38bb9e97176a36ef8ed9506611be4f6e1ddf` | `src/host-config/kwin-active-window-candidate.conf` |
| `cf647e7f955d60ec6cae1ee58196b94d939996df44697f9e479023921bbe1f91` | `src/host-config/kwin-animation-start-step.conf` |
| `ebb40936ea5b94c0970194727a29954e954780bc5b7817421e79fc5ee9d04911` | `src/host-config/kwin-empty-render-not-content.conf` |
| `4d95be9e131077102159769046807e96d2d0c0625ced3b7813dd1d023f5b51fc` | `src/host-config/kwin-night-light-after-blending.conf` |
| `c28ab03912079127f981603c228443ecfcdf030e7fc0170728f3e400735d2d93` | `src/host-config/kwin-scanout-variance-cap.conf` |
| `ac0cb19c913e350f76be6c0f5c129dfc77eb7c25b3a717205efaa57aaca46541` | `src/host-config/kwin-skip-empty-layer-render.conf` |

Check: `cd host/kde/research && find . -type f | LC_ALL=C sort | xargs sha256sum`.

When the research's recipe moves (`r` its repository, `c` the commit, `v`
this folder), take it again as it is, the sets' patches with it:

```sh
r=~/Documents/virtio-gpu-desktop c=4ee46dd v=~/Documents/vitrine/host/kde/research
rm -rf "$v" && mkdir -p "$v/setup/tools" "$v/src/host-config"
for f in setup/lib.sh setup/paths.sh setup/versions.sh setup/tools/kde-rpms.sh \
	setup/tools/build-patched-rpm.sh \
	src/host-config/kwin-{active-window-candidate,animation-start-step,empty-render-not-content,night-light-after-blending,scanout-variance-cap,skip-empty-layer-render}.conf; do
	git -C "$r" show "$c:$f" > "$v/$f"
done
# patches/DIR (kde_patch_dir) and the host sets kde_set_for names for each package
for d in kwin:hl39y,hl39-68,h681zy ksvg:k2 kirigami:f1 qtbase:q4; do
	dir=${d%%:*} sets=${d#*:}
	mkdir -p "$v/patches/$dir/sets"
	for s in ${sets//,/ }; do
		git -C "$r" show "$c:patches/$dir/sets/$s.series" > "$v/patches/$dir/sets/$s.series"
	done
	for p in $(cat "$v/patches/$dir/sets/"*.series | sed 's/#.*//' | sort -u); do
		git -C "$r" show "$c:patches/$dir/$p" > "$v/patches/$dir/$p"
	done
done
# the research's modes: its scripts are executable
for f in $(cd "$v" && find . -type f | sed 's|^\./||'); do
	[ "$(git -C "$r" ls-tree "$c" -- "$f" | cut -d' ' -f1)" = 100755 ] && chmod 755 "$v/$f" || chmod 644 "$v/$f"
done
```

Then: a new host set in `kde_set_for` needs its series file in the list
above, a new host package its section in `components.conf`, and a new
run-time switch of the research's host session (`setup/steps/host-session.sh`)
for a patch of the sets its file; `research=` in `components.conf` gets the
new commit and the table its new checksums. No file may have a
`Claude-Session` trailer line (the research removed them on 2026-10-14). A change
of the copy makes the app's builds of the packages it concerns out of date.
