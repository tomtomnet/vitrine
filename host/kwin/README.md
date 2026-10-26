# The host's KWin

An optional component of Vitrine's Builds window ("Host desktop"): this
computer's own KWin with the research's host patches. It replaces the
desktop's compositor, so Vitrine builds it only when asked, and installs it
only after the administrator's password.

What the host set changes (the research's measurements, on Plasma 6.7.91):
keys in a VM take 8.5-9.3 ms against 12.5 ms with Fedora's KWin; drags and
the launcher under load are smoother; the guest's frames are paced to the
host's deadlines (presentation-time v3); and the pointer grab
(Ctrl+Alt+G) works inside Vitrine's Desktop view (patch 0039). On Intel xe
GPUs (Arc), KWin no longer makes the GPU read every window the CPU draws
from system memory at each frame (patch 0075): with Fedora's KWin and the
sets before `hl39y`, Konsole drags missed 47 % of the refreshes on an Arc
Pro B50.

- `build.sh` builds it: the research's `setup/tools/kde-rpms.sh`, run as
  it is from the copy in `research/`, in a Fedora container (rootless
  podman, memory capped). It takes the source RPM of the newest KWin in the
  repositories the host's Plasma comes from (Fedora's, or the KDE SIG's
  beta COPR) and the research's set for that version (`kde_set_for` in
  `research/setup/versions.sh`): `hl39y` for 6.7.91, `hl39-68` for 6.8.0,
  `h681a` for 6.8.1 and later (the research has built `h681a` once, and
  not run it yet). `hl39-68` is the research's set of 2026-10-08, without
  0075 and the patches after it: with an Intel xe GPU on Plasma 6.8.0, set
  `KWIN_DISABLE_UDMABUF_IMPORT=1` (in `~/.config/environment.d`). Fedora
  44's own Plasma, 6.7.5, has no set. The research's overrides of its sets
  (`VGD_HOST_KWIN_SET` and the like) are ignored.
- `install.sh` installs it, as root (Vitrine runs it with pkexec): the KWin
  packages installed now (kwin, kwin-common, kwin-libs) are replaced by
  the build's, checked by their header digests, and version-locked
  (`dnf versionlock`) so that updates leave them alone. Some of the
  sets' patches act only when a file of `$XDG_RUNTIME_DIR` says so (the
  research's `src/host-config/kwin-*.conf`: the scanout journal's
  variance cap, the active window as a plane candidate, empty renders and
  damage-free layers, effects starting one refresh in). `install.sh` turns
  them on for every user, in `/etc/user-tmpfiles.d/vitrine-host-kwin.conf`
  (made at each login; for the user installing, at once). To turn one off,
  remove its line there. `install.sh revert` removes the locks and that
  file, and puts the distribution's KWin back (`dnf distro-sync`), only
  for a KWin Vitrine installed.
- `components.conf` describes it to the app.

The new KWin runs from the next login. It must match the installed Plasma
and Qt: KWin uses Qt's private API, and Plasma updates do not wait for a
locked KWin. After a Plasma or Qt update Vitrine says so, and offers to
build and install the matching set, or to revert. If the desktop does not
start after an update, log in on a text console (Ctrl+Alt+F3) and put
the distribution's KWin back:

```sh
sudo dnf versionlock delete kwin kwin-common kwin-libs
sudo dnf distro-sync kwin kwin-common kwin-libs
```

## `research/`: the research's recipe, pinned

A copy of what `kde-rpms.sh` reads in the research's repository
(virtio-gpu-desktop) at commit `ce2c5ee6349ac3eda027743803b17f40ab3f86b0`
(2026-10-26), in its layout (the container mounts the folder as `/repo`),
byte for byte: the scripts, the three host sets and the 33 patches they
name, and the run-time switches of those patches (`src/host-config`),
which `install.sh` reads. `build.sh` passes it everything through the
environment: Vitrine's folders (`VGD_BUILD`), no `paths.conf` of the
research's (`VGD_CONFIG=/dev/null`), the jobs, the Plasma's origin
(`PLASMA`), the host's Fedora (`VGD_FEDORA`), podman's options
(`VGD_PODMAN_ARGS`: the container's name, which Stop needs, and its memory
cap) and live output (`VGD_VERBOSE=1`). Each build keeps
`buildroot-TAG.txt`, the packages it was built against, which the app
compares with the host's before installing.

| sha256 | file |
|---|---|
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
| `882402feebe54b1a640b542368afd0c2caf9ff3b20b351266f2d14cf2798ac2c` | `patches/kwin/sets/h681a.series` |
| `c18c004fe7319f2d4509deeadf37df8434088affde845d0eb96b11bb6642367d` | `patches/kwin/sets/hl39-68.series` |
| `45da20f38c96aeb3d44540c0567de26c293e6aba799b1e64e01eebb79ee7ad11` | `patches/kwin/sets/hl39y.series` |
| `67830e2765b11774b880c097b0eab79e700e2cdb7c4aaa2e6ac7bcd2514d05f0` | `setup/lib.sh` |
| `761707846b55e70bac195d24e30520ca5c7b96164c0ce45b113317fc13150deb` | `setup/paths.sh` |
| `4b4a82cd31efff9ea7ff2572db175bf2d28ce9811513a764e358144b15dc3310` | `setup/tools/build-patched-rpm.sh` |
| `d49ee93d128f6fab553691631f0a451d19e7bc2b686a226ec5ba3dac0eb65070` | `setup/tools/kde-rpms.sh` |
| `615e06fa7e64757ce434af6d4ecbff9f35a7a20aa17c0d2a6c998f1612c0f8dd` | `setup/versions.sh` |
| `19cf4324887903cc1185fc8dc83d38bb9e97176a36ef8ed9506611be4f6e1ddf` | `src/host-config/kwin-active-window-candidate.conf` |
| `cf647e7f955d60ec6cae1ee58196b94d939996df44697f9e479023921bbe1f91` | `src/host-config/kwin-animation-start-step.conf` |
| `ebb40936ea5b94c0970194727a29954e954780bc5b7817421e79fc5ee9d04911` | `src/host-config/kwin-empty-render-not-content.conf` |
| `c28ab03912079127f981603c228443ecfcdf030e7fc0170728f3e400735d2d93` | `src/host-config/kwin-scanout-variance-cap.conf` |
| `ac0cb19c913e350f76be6c0f5c129dfc77eb7c25b3a717205efaa57aaca46541` | `src/host-config/kwin-skip-empty-layer-render.conf` |

Check: `cd host/kwin/research && find . -type f | sort | xargs sha256sum`.

When the research's recipe moves (`r` its repository, `c` the commit, `v`
this folder), take it again as it is, the sets' patches with it:

```sh
r=~/Documents/virtio-gpu-desktop c=ce2c5ee v=~/Documents/vitrine/host/kwin/research
rm -rf "$v" && mkdir -p "$v/setup/tools" "$v/patches/kwin/sets" "$v/src/host-config"
for f in setup/lib.sh setup/paths.sh setup/versions.sh setup/tools/kde-rpms.sh \
	setup/tools/build-patched-rpm.sh patches/kwin/sets/{hl39y,hl39-68,h681a}.series \
	src/host-config/kwin-{active-window-candidate,animation-start-step,empty-render-not-content,scanout-variance-cap,skip-empty-layer-render}.conf; do
	git -C "$r" show "$c:$f" > "$v/$f"
done
for p in $(cat "$v"/patches/kwin/sets/*.series | sed 's/#.*//' | sort -u); do
	git -C "$r" show "$c:patches/kwin/$p" > "$v/patches/kwin/$p"
done
```

Then: a new set in `kde_set_for` for the host needs its series file in
the list above, and a new run-time switch of the research's host session
(`setup/steps/host-session.sh`) for a patch of the sets its file; `research=` in `components.conf` gets the new commit and
the table its new checksums. No file may have a `Claude-Session:` line
(the research removed them on 2026-10-14). A change of the copy makes the
app's build of it out of date.
