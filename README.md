# vitrine

A Qt application for running Linux desktop virtual machines with QEMU/KVM.
The guest uses the host's GPU through virtio-gpu DRM native context, and the
app shows the guest's display in its window through QEMU's D-Bus display, or
leaves it to QEMU's own SDL window.

QEMU, virglrenderer and the guest's graphics stack (virtio-gpu driver, Mesa,
KWin) are used at pinned versions, with the patches vitrine needs kept in
this repository: `host/versions.conf` and `host/patches` for the QEMU and
virglrenderer vitrine builds (upstream QEMU, with the qemu-gui fork's commits
among its patches), `guest/` for the guest's.

Early development. It is built and tried on Fedora 44 KDE Plasma (Wayland)
with an AMD Radeon 780M, with Fedora 44 KDE guests. It is not packaged.

## Requirements

- A Fedora 44 x86-64 host with KVM (`/dev/kvm`). Other distributions and X11
  are not tried; the keyboard grab and pointer lock use Wayland protocols.
- For 3D through DRM native context: on the host, an AMD GPU (amdgpu) or an
  Intel GPU on the i915 or xe driver (only AMD is tried); in the guest, Linux
  6.14 or later and a Mesa with native context, which the guest tools install in
  Fedora 44 guests ([docs/guest-mesa.md](docs/guest-mesa.md) covers others).
  Otherwise the guest falls back to virgl.
- For native context, the host's udmabuf limits raised: the kernel's defaults
  (1024 entries, 64 MB per buffer) refuse the windows a 4K guest draws with
  the CPU (Qt Widgets and GTK apps, cursors), which the guest then copies.
  Host tuning raises them while such VMs run (see Install), or once for every
  boot:
  `sudo grubby --update-kernel=ALL --args='udmabuf.list_limit=65536 udmabuf.size_limit_mb=2048'`
  and a restart ([docs/host-tuning.md](docs/host-tuning.md) has other ways).
  vitrine checks them at each start of such a VM and says when they are low.
- An internet connection to build vitrine's QEMU and the guest tools.

## Dependencies (Fedora)

Building the app:

```
sudo dnf install cmake ninja-build gcc gcc-c++ pkgconf-pkg-config git \
    qt6-qtbase-devel qt6-qtbase-private-devel glib2-devel mesa-libEGL-devel \
    mesa-libgbm-devel libdrm-devel wayland-devel wayland-protocols-devel
```

Building vitrine's QEMU (the list in `host/build.sh`; `host/build.sh
--print-deps` prints what is missing, `--deps` installs it):

```
sudo dnf install git gcc gcc-c++ make meson ninja-build pkgconf-pkg-config python3 \
    python3-pyyaml python3-wheel python3-setuptools python3-pip binutils util-linux \
    glib2-devel pixman-devel zlib-devel libslirp-devel SDL2-devel libepoxy-devel \
    mesa-libgbm-devel mesa-libEGL-devel libdrm-devel libva-devel libusb1-devel \
    pulseaudio-libs-devel pipewire-devel spice-protocol libzstd-devel libpng-devel \
    libcap-ng-devel libattr-devel wayland-devel wayland-protocols-devel bzip2
```

Building the guest tools (without createrepo_c, the script runs it in podman):

```
sudo dnf install podman dosfstools mtools createrepo_c
```

Running VMs: UEFI firmware (else new VMs use BIOS), passt for the NAT network
(else QEMU's user-mode one), virtiofsd for shared folders, Qt's Wayland and SVG
plugins (Plasma has them):

```
sudo dnf install edk2-ovmf passt virtiofsd qt6-qtwayland qt6-qtsvg
```

## Build

```
git clone https://github.com/tomtomnet/vitrine.git
cd vitrine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure   # optional
```

## Run

```
./build/app/vitrine
```

1. **Build vitrine's QEMU.** The banner's Build… button (or File > Build
   QEMU…, Ctrl+B) opens a window whose Build button fetches QEMU and
   virglrenderer at their pinned commits, patches them and installs them into
   `~/.local/share/vitrine/stack` (a few minutes). VMs need it for their
   display in vitrine's window and for 3D. Until it is built, they use the
   system's QEMU, if there is one, except those that use what it lacks,
   such as the native context of new Linux VMs: these wait for the build.
   `host/build.sh` does the same build by hand, without the root helper's
   real-time setup for that QEMU.
2. **Create a VM.** File > New… (Ctrl+N): name, system, memory, processors,
   disk, and optionally an ISO to install from. Machine > Start (Ctrl+Return)
   starts it. Each VM is its QEMU command line, which Machine > Settings
   (Ctrl+S) edits. A Linux VM whose 3D card lacks what new VMs get (native
   context, the host vblank timing) offers Update… on its Details tab.
3. **Guest tools** for Fedora 44 x86-64 guests (virtio-gpu driver, Mesa, KWin
   for Plasma 6.7.5, an agent). Build them from the checkout, in rootless
   podman (up to 10 GiB of memory; Mesa ~15-25 min, KWin ~10, tools ~1):

   ```
   guest/build-rpms.sh
   guest/build-medium.sh
   ```

   Then Machine > Install Guest Tools… > Install starts the VM with the tools
   medium (Restart and Install restarts a running one); the guest installs
   them before its desktop starts, then restarts once more. It needs the
   network and Secure Boot off (the driver is not signed). Attach Medium Only
   starts the VM with the medium alone: open VITRINETOOL in the guest's file
   manager, then run `sudo bash /run/media/$USER/VITRINETOOL/install`.

   After an update of vitrine, run both scripts again (they rebuild only the
   packages whose sources changed): VMs with older tools then offer Update….

Keys: Ctrl+Alt+G gives the keyboard and mouse to the VM or takes them back,
Ctrl+Alt+F toggles full screen, F9 shows or hides the VM list, Ctrl+H asks
the guest to shut down, Ctrl+L shows QEMU's log. Closing vitrine can leave
its VMs running; it finds them again at its next start.

Clipboard: text copied on the host can be pasted in the guest, and the other
way around, through spice-vdagent in the guest (new VMs have its channel).
Text only. In a Plasma Wayland guest, text copied in Wayland applications
reaches the host only with the guest tools, which add a bridge between the
guest's Wayland and X11 clipboards. On a Wayland host, text copied in the
guest replaces the host clipboard only if vitrine's window had a key press,
a click or the mouse entering it since the last host copy; copying in the
guest with its keys or mouse does that. QEMU's SDL window shares the
clipboard too, and on Wayland waits for a key or click in it.

Files: VMs in `~/.local/share/vitrine/vms/<id>/` (`vm.args`, a new VM's
disk, `qemu.log`), vitrine's QEMU in `~/.local/share/vitrine/stack/`, the
guest tools in `~/.local/share/vitrine/guest-tools/`, QEMU's build trees in
`~/.cache/vitrine/stack-build/`, the guest tools' downloads in
`~/.cache/vitrine/guest-build/`, settings in `~/.config/vitrine/settings.conf`.

## Install

```
sudo cmake --install build
```

This installs `vitrine` (also in the application menu), `host/` for building
its QEMU, and the optional root helper with its polkit files (polkit reads
actions only from `/usr/share/polkit-1/actions`, hence the `/usr` prefix).
To update: `git pull`, build and install again; vitrine then offers to
update its QEMU if needed.

The helper tunes the host while VMs run, for members of the `vitrine` group
([docs/host-tuning.md](docs/host-tuning.md)); without it, VMs run untuned
and the status bar says so. At the first VM start, vitrine offers to set
the group up (Set Up: an administrator's password, in the desktop's polkit
dialog); Preferences shows whether tuning is active. Instead of Set Up, by
hand:

```
sudo groupadd --system vitrine
sudo usermod -aG vitrine "$USER"
```

## Made by AI

The code, patches and documentation in this repository are written by AI
(Anthropic's Claude, through Claude Code) under a human's direction. The
patches are not upstream submissions; QEMU, for one, does not accept
AI-generated contributions.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Patches to other projects are under
the licenses of the files they change, and files with their own license header
keep that license.
