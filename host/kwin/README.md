# The host's KWin

An optional component of Vitrine's Builds window ("Host desktop"): this
computer's own KWin with the research's host patches. It replaces the
desktop's compositor, so Vitrine builds it only when asked, and installs it
only after the administrator's password.

What the host set changes (the research's measurements, on Plasma 6.7.91):
keys in a VM take 8.5-9.3 ms against 12.5 ms with Fedora's KWin; drags and
the launcher under load are smoother; the guest's frames are paced to the
host's deadlines (presentation-time v3); and the pointer grab
(Ctrl+Alt+G) works inside Vitrine's Desktop view (patch 0039).

- `build.sh` builds it: the research's `setup/tools/kde-rpms.sh`, run as
  it is from the copy in `research/`, in a Fedora container (rootless
  podman, memory capped). It takes the source RPM of the newest KWin in the
  repositories the host's Plasma comes from (Fedora's, or the KDE SIG's
  beta COPR) and the research's set for that version (`kde_set_for` in
  `research/setup/versions.sh`): `hl39` for 6.7.91, `hl39-68` for 6.8.0,
  `hl39-681` for 6.8.1 and later. Fedora 44's own Plasma, 6.7.5, has no
  set.
- `install.sh` installs it, as root (Vitrine runs it with pkexec): the KWin
  packages installed now (kwin, kwin-common, kwin-libs) are replaced by
  the build's, checked by their header digests, and version-locked
  (`dnf versionlock`) so that updates leave them alone. `install.sh
  revert` removes the locks and puts the distribution's KWin back (`dnf
  distro-sync`), only for a KWin Vitrine installed.
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
(virtio-gpu-desktop) at commit `36532330292b8ff9185a0dddf6fbc92a17ea6252`
(2026-10-14), in its layout (the container mounts the folder as `/repo`),
byte for byte: the scripts, the three host sets and the 16 patches they
name. `build.sh` passes it everything through the environment: Vitrine's
folders (`VGD_BUILD`), no `paths.conf` of the research's
(`VGD_CONFIG=/dev/null`), the jobs, the Plasma's origin (`PLASMA`), the
host's Fedora (`VGD_FEDORA`), podman's options (`VGD_PODMAN_ARGS`: the
container's name, which Stop needs, and its memory cap) and live output
(`VGD_VERBOSE=1`). Each build keeps `buildroot-TAG.txt`, the packages it
was built against, which the app compares with the host's before
installing.

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
| `c18c004fe7319f2d4509deeadf37df8434088affde845d0eb96b11bb6642367d` | `patches/kwin/sets/hl39-68.series` |
| `554380a4ed85b7cd00a65f6c8e8c415e44a78a102e8b07d8f1563e52e3f1389e` | `patches/kwin/sets/hl39-681.series` |
| `c18c004fe7319f2d4509deeadf37df8434088affde845d0eb96b11bb6642367d` | `patches/kwin/sets/hl39.series` |
| `3130ad9391ec2569c0037d05a3828c5f069c266a599460583158ccb5abce8a9c` | `setup/lib.sh` |
| `761707846b55e70bac195d24e30520ca5c7b96164c0ce45b113317fc13150deb` | `setup/paths.sh` |
| `36c8010a3fbac9c4ff325d8a2dbee5d8a9fbc36c188bb6f05c9fcf5c9b6a5dd4` | `setup/tools/build-patched-rpm.sh` |
| `d49ee93d128f6fab553691631f0a451d19e7bc2b686a226ec5ba3dac0eb65070` | `setup/tools/kde-rpms.sh` |
| `ab4385055e4704958c6625be1000561588bd2c997271c1e1ad627a27275c13c7` | `setup/versions.sh` |

Check: `cd host/kwin/research && find . -type f | sort | xargs sha256sum`.

When the research's recipe moves (`r` its repository, `c` the commit, `v`
this folder), take it again as it is, the sets' patches with it:

```sh
r=~/Documents/virtio-gpu-desktop c=3653233 v=~/Documents/vitrine/host/kwin/research
rm -rf "$v" && mkdir -p "$v/setup/tools" "$v/patches/kwin/sets"
for f in setup/lib.sh setup/paths.sh setup/versions.sh setup/tools/kde-rpms.sh \
	setup/tools/build-patched-rpm.sh patches/kwin/sets/hl39{,-68,-681}.series; do
	git -C "$r" show "$c:$f" > "$v/$f"
done
for p in $(cat "$v"/patches/kwin/sets/*.series | sed 's/#.*//' | sort -u); do
	git -C "$r" show "$c:patches/kwin/$p" > "$v/patches/kwin/$p"
done
```

Then: a new set in `kde_set_for` for the host needs its series file in
the list above; `research=` in `components.conf` gets the new commit and
the table its new checksums. No file may have a `Claude-Session:` line
(the research removed them on 2026-10-14). A change of the copy makes the
app's build of it out of date.
