# Building Mesa with native context

With DRM native context, a Linux guest drives the host's GPU through the
Mesa driver it would use on the real hardware: radeonsi and RADV for AMD,
iris and ANV for Intel, freedreno and Turnip for Qualcomm, asahi for Apple.
That is much faster than virgl, which turns the guest's OpenGL into OpenGL
calls on the host.

The guest's Mesa must be built with native context for that GPU, and most
distributions leave it out. A guest without it falls back to virgl, without
a word. This guide shows how to check a guest, and how to rebuild your
distribution's own Mesa package with native context on Fedora, Debian,
Ubuntu and Arch.

## What the guest needs

- Linux 6.14 or later, as
  [QEMU's documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
  asks. Debian 13 has 6.12: take the kernel of trixie-backports
  (`sudo apt install -t trixie-backports linux-image-amd64`). Ubuntu 24.04
  has 6.8: take its HWE kernel, 7.0 now
  (`sudo apt install linux-generic-hwe-24.04`).
- Mesa with native context for the host's GPU. It is a build option:

| Host GPU | Guest drivers | Mesa build option | Since Mesa |
| --- | --- | --- | --- |
| AMD | radeonsi, RADV | `-Damdgpu-virtio=true` | 25.0 |
| Intel, on the i915 kernel driver | iris, ANV | `-Dintel-virtio-experimental=true` | 26.1 |
| Intel, on the Xe kernel driver | iris, ANV | the same, and [a patch](#intel-xe) | 26.1 |
| Qualcomm | freedreno, Turnip | `-Dfreedreno-kmds=msm,virtio` | 23.1 |
| Apple (Asahi Linux) | asahi | none: always built in | 24.2 |
| Arm Mali | panfrost, PanVK | not in Mesa yet | |

- The names come from Mesa's own options file, `meson.options`
  (`meson_options.txt` before Mesa 25.1), and each version is the first
  release that has the option. They agree with the table of QEMU's
  documentation.
- Meson stops at an option it doesn't know: leave out the ones your Mesa is
  too old for.
- On the host, `lspci -k` says which kernel driver an Intel GPU uses:
  `Kernel driver in use: xe` or `i915`.
- Qualcomm: Mesa 22.1 to 23.0 had `-Dfreedreno-virtio=true` instead, for
  OpenGL only. The default of `freedreno-kmds` is `msm` alone.
- Apple: since Mesa 24.2, the asahi driver always has native context, so a
  distribution that builds that driver has it.
- Arm Mali: virglrenderer has a panfrost renderer, but the guest side is
  still a merge request,
  [!36814](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/36814),
  which would add `-Dpanfrost-virtio=true`.

## What distributions ship

As of September 2026:

- **Arch** has native context for AMD and Qualcomm (since mesa 1:26.0.4-1)
  and Intel i915 (since 1:26.1.0-1). Only Intel Xe needs more, see
  [Arch](#arch).
- **Fedora** 43, 44 and Rawhide turn none of the options on. Their Mesa
  builds the asahi driver, so Apple works; for the others, see
  [Fedora](#fedora).
- **Debian** (13 to unstable) and **Ubuntu** (24.04, 26.04) turn none of
  them on either. The asahi driver is in Ubuntu's Mesa, and in Debian's
  from trixie-backports on. See [Debian and Ubuntu](#debian-and-ubuntu).
- The newer Mesa builds for them don't turn them on either: the
  kisak-mesa PPA for Ubuntu, and the Mesa COPRs for Fedora checked
  (xxmitsu/mesa-git, danayer/mesa-git, mochizuki0323/mesa-git,
  adil192/mesa-x86-64-v3).

Rebuilding the distribution's own package with the options added keeps the
rest as the distribution has it: the same drivers, files and dependencies.

## Fedora

Rebuild Fedora's `mesa` package with `fedpkg`, in a clean build root with
mock, so that its build dependencies stay out of your system:

    sudo dnf install fedpkg mock
    sudo usermod -aG mock $USER      # then log out and in again
    fedpkg clone -a -b f44 mesa      # your release: f43, f44 or rawhide
    cd mesa

Add the options to the `%meson` call of `mesa.spec`, under
`-Dplatforms=x11,wayland \`:

      -Damdgpu-virtio=true \
      -Dintel-virtio-experimental=true \
      -Dfreedreno-kmds=msm,virtio \
      -Dvideo-codecs=all \

This command does it:

    sed -i 's/^  -Dplatforms=x11,wayland \\$/&\n  -Damdgpu-virtio=true \\\n  -Dintel-virtio-experimental=true \\\n  -Dfreedreno-kmds=msm,virtio \\\n  -Dvideo-codecs=all \\/' mesa.spec

Fedora 43 has Mesa 25.3, older than the Intel option: delete that line
there. `-Dvideo-codecs=all` brings the video codecs Fedora leaves out, see
[Video codecs](#video-codecs).

Commit the change, and build. With the commit, the release of your
packages is one above Fedora's (26.2.3-2.fc44 over 26.2.3-1.fc44), so dnf
takes them as an update:

    git commit -am "Build with DRM native context and all video codecs"
    fedpkg mockbuild

The packages land in `results_mesa/<version>/<release>/`. `dnf upgrade`
installs the ones you have, and skips the others:

    sudo dnf upgrade ./results_mesa/26.2.3/2.fc44/*.x86_64.rpm

Then keep dnf from replacing them with Fedora's next Mesa:

    sudo dnf versionlock add 'mesa-*'

The lock doesn't hide Fedora's updates from `dnf list --upgrades 'mesa-*'`.
When a new Mesa shows there, build it with your change on top:

    git pull --rebase
    fedpkg mockbuild
    sudo dnf versionlock delete 'mesa-*'
    sudo dnf upgrade ./results_mesa/<version>/<release>/*.x86_64.rpm
    sudo dnf versionlock add 'mesa-*'

- `fedpkg local` builds in your system instead of mock, after
  `sudo dnf builddep ./mesa.spec`, and puts the packages in `x86_64/`.
- Steam and Wine use the 32-bit Mesa too, the `.i686` packages. If they are
  installed, build those as well, with
  `fedpkg mockbuild --root fedora-44-i386`, and give dnf both sets at once.
- `fedpkg srpm` makes a source package that a COPR of your own can build,
  if you'd rather not build in the VM.

## Debian and Ubuntu

Rebuild the distribution's `mesa` source package, with the options added
to `debian/rules`. APT needs the source repositories for that: `deb-src`
in `Types:` of `/etc/apt/sources.list.d/debian.sources` (`ubuntu.sources`
on Ubuntu), or `deb-src` lines in `/etc/apt/sources.list`. Then:

    sudo apt update
    sudo apt install build-essential devscripts
    apt source mesa
    sudo apt build-dep mesa
    cd mesa-*/

Add the options after the list of drivers in `debian/rules`:

    sed -i '/^confflags_GALLIUM += -Dgallium-drivers=/a confflags_GALLIUM += -Damdgpu-virtio=true -Dfreedreno-kmds=msm,virtio' debian/rules

On Mesa 26.1 and later, add `-Dintel-virtio-experimental=true` to that
line for Intel. Then give the build a version of its own, build it,
install it over the packages you have, and hold them:

    dch --local +nc "Build with DRM native context"
    dpkg-buildpackage -b -us -uc
    sudo debi --upgrade
    sudo apt-mark hold $(dpkg-query -W -f '${Package} ${Version}\n' | awk '/\+nc[0-9]+$/ {print $1}')

- `dch --local +nc` makes the version, for example, `26.2.3-2+nc1`, newer
  than the distribution's. `debi --upgrade` installs the new packages of
  the ones installed, and no others.
- For the next Mesa of the distribution, `apt-mark unhold` the same
  packages, and build it the same way.
- Debian 13 has Mesa 25.0: AMD and Qualcomm. trixie-backports has 26.1, with
  Intel: `apt source -t trixie-backports mesa`, and
  `sudo apt build-dep -t trixie-backports mesa`. Unstable has 26.2.
- Ubuntu 24.04 has 25.2, and 26.04 has 26.0: AMD and Qualcomm. For Intel,
  start from the source of the
  [kisak-mesa PPA](https://launchpad.net/~kisak/+archive/ubuntu/kisak-mesa),
  which follows Mesa's releases (26.2.3 now), then the same steps:
  `sudo add-apt-repository -s ppa:kisak/kisak-mesa`. The oibaf PPA is
  suspended.
- Steam and Wine use the 32-bit (i386) packages too, which need a build of
  their own for i386, not covered here.

## Arch

Arch's `mesa` and `lib32-mesa` have native context for AMD, Intel i915 and
Qualcomm. Only Intel Xe needs more: [the Xe patch](#intel-xe), prebuilt
or in a build of your own.

### Prebuilt, from cmspam

cmspam's [xe-virt-repo](https://github.com/cmspam/xe-virt-repo) has
patched `mesa`, `lib32-mesa` and `intel-media-driver` for Arch and CachyOS,
rebuilt as Arch updates them. They are built for x86-64-v3, and not signed.
Add to `/etc/pacman.conf`, above `[extra]`:

    [xe-virt-guest-v3]
    SigLevel = Optional
    Server = https://github.com/cmspam/xe-virt-repo/releases/download/latest-guest

then:

    sudo pacman -Syu mesa lib32-mesa intel-media-driver

### Building it

`prepare()` in Arch's PKGBUILD applies every `.patch` of its `source`:

    sudo pacman -S --needed base-devel devtools pacman-contrib
    pkgctl repo clone --protocol=https mesa
    cd mesa
    gpg --import keys/pgp/*.asc
    curl -LO https://raw.githubusercontent.com/cmspam/xe-native-context-enablement/master/mesa-01-xe-native-context-plus-iris-upload-fix.patch
    sed -i '/^source=(/,/^)/ s|^)|  mesa-01-xe-native-context-plus-iris-upload-fix.patch\n)|' PKGBUILD
    updpkgsums
    makepkg -s
    sudo pacman -U mesa-1:*.pkg.tar.zst vulkan-intel-1:*.pkg.tar.zst

- `lib32-mesa` builds the same way, from
  `pkgctl repo clone --protocol=https lib32-mesa`, for `lib32-mesa` and
  `lib32-vulkan-intel`.
- `pacman -Syu` replaces them with Arch's next Mesa. This line in
  `/etc/pacman.conf` keeps them until you build that one, with
  `git checkout PKGBUILD && git pull` and the same steps:
  `IgnorePkg = mesa vulkan-intel`.

## Video codecs

Mesa's hardware video, VA-API and Vulkan Video, only has the codecs its
build turns on, with the `video-codecs` option:

- `all_free`, the default: AV1 and VP9, and since Mesa 26.0 MPEG-1/2 and
  JPEG;
- `all`: those, and the patent-encumbered H.264, H.265 (HEVC) and VC-1;
- or a list of `vc1dec`, `h264dec`, `h264enc`, `h265dec`, `h265enc`,
  `av1dec`, `av1enc`, `vp9dec`, `mpeg12dec`, `jpegdec`.

`all` and `all_free` came with Mesa 24.0. From 22.2 to 23.3, the option
only listed the five patent-encumbered codecs, and none was the default.

**Fedora** builds with the default. RPM Fusion's
`mesa-va-drivers-freeworld` and `mesa-vulkan-drivers-freeworld` add H.264,
H.265 and VC-1. But they have no native context, and Fedora's libva looks
in their folder, `/usr/lib64/dri-freeworld`, before its own. Your build has
the codecs with `-Dvideo-codecs=all`, so go back to Fedora's packages
before you install it:

    sudo dnf remove mesa-va-drivers-freeworld
    sudo dnf swap mesa-vulkan-drivers-freeworld mesa-vulkan-drivers

**Debian** and **Ubuntu** already build with `-Dvideo-codecs="all"`, and so
does kisak-mesa: keep that line of `debian/rules` as it is. **Arch**
builds with `-D video-codecs=all`.

The patent-encumbered codecs are left out for legal reasons. Mesa's
description of the option says: "Distros might want to consult their legal
department before enabling these." Whether you may use them depends on
where you live.

## Intel Xe

Upstream Mesa's Intel native context only works with the host's i915
kernel driver. For GPUs on the Xe driver, cmspam wrote the missing parts,
not upstream yet, in
[xe-native-context-enablement](https://github.com/cmspam/xe-native-context-enablement).
Thanks to cmspam for them. The repository has three patches:

- `virglrenderer-xe-native-context.patch`, for the host: an Xe renderer in
  virglrenderer (`-Ddrm-renderers=xe-experimental`). File > Build QEMU
  applies it when it builds virglrenderer.
- `mesa-01-xe-native-context-plus-iris-upload-fix.patch`, for the guest: Xe
  in Mesa's Intel virtio layer, which iris (OpenGL) and ANV (Vulkan) share,
  and a fix for slow uploads. It goes on Mesa 26.1 or later, built with
  `-Dintel-virtio-experimental=true`.
- `intel-media-driver-xe-native-context.patch`, for the guest, only for
  hardware video (VA-API): a patch to Intel's media driver (iHD).

It also explains how the Xe backend differs from the i915 one
(`DIFFERENCES.md`), and links the prebuilt packages for Arch and a Nix
flake.

Get the Mesa patch from its `master` branch:

    curl -LO https://raw.githubusercontent.com/cmspam/xe-native-context-enablement/master/mesa-01-xe-native-context-plus-iris-upload-fix.patch

In September 2026, it applies as it is to Mesa 26.1.0, while 26.1.6, 26.2.3
and main need *fuzz* for one part of it, as a line next to it moved. GNU
patch allows that by default, but rpmbuild and dpkg-source don't, hence
these steps:

### Fedora

In the `mesa` folder of the [Fedora](#fedora) steps, with the patch there,
add it to the spec, allow the fuzz, and build again:

    sed -i 's/^Source1: .*/&\nPatch:          mesa-01-xe-native-context-plus-iris-upload-fix.patch/' mesa.spec
    sed -i '1i %global _default_patch_fuzz 2' mesa.spec
    git add mesa-01-xe-native-context-plus-iris-upload-fix.patch
    git commit -am "Add the Xe native context patch"
    fedpkg mockbuild

### Debian and Ubuntu

dpkg-source takes no patch with fuzz in `debian/patches`, but a binary
build keeps changes made to the source. So with the patch downloaded next
to the `mesa-*` folder, apply it in there, before `dpkg-buildpackage`:

    patch -p1 < ../mesa-01-xe-native-context-plus-iris-upload-fix.patch

### Arch

See [Arch](#arch).

Keep the patches of the host and the guest from the same time: the two
sides talk a protocol of their own, and cmspam warns that mismatched
versions can crash. File > Build QEMU takes the newest virglrenderer patch
each time it builds.

## Checking it works

Log out and in again, or restart the guest, so that programs load the new
Mesa. Then `eglinfo -B` and `glxinfo -B` name the GPU and the driver that
draw (on Fedora, the packages `egl-utils` and `glx-utils`; on Debian,
Ubuntu and Arch, `mesa-utils`):

    $ eglinfo -B
    ...
    OpenGL core profile renderer: AMD Radeon RX 9070 XT (radeonsi, gfx1201, ACO, DRM 3.64, 7.2.7-200.fc44.x86_64)

That is native context: the host's GPU, through its own driver, radeonsi.
Through virgl, the line reads `virgl (AMD Radeon RX 9070 XT (radeonsi, …))`:
the name of the host's GPU shows in both, so look at the start.
`glxinfo -B` calls it `OpenGL renderer string`.

For Vulkan, `vulkaninfo --summary` (package `vulkan-tools`) shows
`driverName = radv` and `deviceName = AMD Radeon RX 9070 XT (RADV GFX1201)`
with native context, and `Virtio-GPU Venus (…)` through Venus.

With the qemu-gui build of QEMU, the Details tab of the running VM says
**DRM native context: on, in use** once the guest has drawn in 3D, and
shows a warning when it draws through virgl.

## The host side

- QEMU needs `-device virtio-vga-gl,blob=on,hostmem=4G,drm_native_context=on`
  (`virtio-gpu-gl-pci` on ARM), and `-accel kvm,honor-guest-pat=on` for
  Intel GPUs and for the AMD patch below. The **DRM native context** box of
  the Display settings adds them.
- The host's virglrenderer needs the native context renderer of its GPU,
  which most distributions leave out. File > Build QEMU builds one, see
  [Building QEMU and virglrenderer](../README.md#building-qemu-and-virglrenderer).
- On AMD, that virglrenderer also carries a
  [write-combining patch](../data/patches/virglrenderer-amdgpu-force-wc.patch):
  without it the guest's uploads stall, and KDE stutters however good its
  Mesa is. For Intel Xe, cmspam's Mesa patch does the same on the guest
  side: it maps the upload buffers write-combined.

## What was checked

This guide was checked in September 2026 against Mesa 26.2.3 and main
(the options files and meson.build at each release), Fedora's, Debian's,
Ubuntu's and Arch's Mesa packaging, RPM Fusion's mesa-freeworld,
kisak-mesa, and the xe-native-context-enablement repository. The Fedora
spec changes were tried up to the build, and the Xe patch on the files it
changes. Not tried here: a whole Mesa build on each distribution, the
32-bit builds, and hardware video through native context.
