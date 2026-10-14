# vitrine

A Qt application for running Linux desktop virtual machines with QEMU/KVM.
The guest uses the host's GPU through virtio-gpu DRM native context, and the
app shows the guest's display in its window through QEMU's D-Bus display, or
leaves it to QEMU's own SDL or GTK window.

QEMU, virglrenderer, rutabaga_gfx and the guest's graphics stack (virtio-gpu
driver, Mesa, KWin) are used at pinned versions, with the patches vitrine
needs kept in this repository: `host/versions.conf` and `host/patches` for
the QEMU, virglrenderer and rutabaga_gfx vitrine builds (upstream QEMU with a
series of patches, `host/patches/qemu/series`), `guest/` for the guest's,
and `host/kwin` for the optional host KWin.

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

Building the app (wayland-devel brings wayland-scanner, which with
wayland-protocols and python3 generates the Wayland protocol code the app
uses):

```
sudo dnf install cmake ninja-build gcc gcc-c++ pkgconf-pkg-config git python3 \
    qt6-qtbase-devel qt6-qtbase-private-devel glib2-devel mesa-libEGL-devel \
    mesa-libgbm-devel libdrm-devel wayland-devel wayland-protocols-devel
```

Building vitrine's QEMU (the list in `host/build.sh`; `host/build.sh
--print-deps` prints what is missing, `--deps` installs it):

```
sudo dnf install git gcc gcc-c++ make meson ninja-build pkgconf-pkg-config python3 \
    python3-pyyaml python3-wheel python3-setuptools python3-pip binutils util-linux \
    glib2-devel pixman-devel zlib-devel libslirp-devel SDL2-devel gtk3-devel libepoxy-devel \
    mesa-libgbm-devel mesa-libEGL-devel libdrm-devel libva-devel libusb1-devel \
    pulseaudio-libs-devel pipewire-devel spice-protocol libzstd-devel libpng-devel \
    libcap-ng-devel libattr-devel wayland-devel wayland-protocols-devel bzip2 \
    rust bindgen-cli
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

The window has one bar above the VMs: the menus (File, View, Machine, Help)
that the steps below name, the buttons that start, pause, shut down, reset
and force off the selected VM, and its tabs (Console, Details, Settings,
Snapshots, Logs) at the end. The button at the start of the bar, or F9,
collapses the list of VMs to their icons and expands it back; so does
dragging its edge. Under the list, a button shows the long tasks under way
(builds, clones), how far they are and what is left: a click shows the
task's window, or a menu of the tasks when there are several.

File > Builds… (Ctrl+B) shows what vitrine builds: its QEMU (virglrenderer,
rutabaga_gfx, QEMU), the guest tools (their packages and medium) and,
optional, the host's KWin, each with its source, its patches by origin,
whether it is up to date and why not, when it was built, how long a build
takes on this machine, and its log. A build redoes only what changed: a component built from the same
inputs is not built again, a changed patch compiles only the files it
changes, and the guest packages compile through ccache.

1. **Build vitrine's QEMU.** The banner's Build… button (or File > Builds…)
   opens the Builds window, whose Build button fetches QEMU, virglrenderer
   and rutabaga_gfx at their pinned commits, patches them and installs them
   into `~/.local/share/vitrine/stack` (a few minutes). VMs need it for their
   display in vitrine's window and for 3D. Until it is built, they use the
   system's QEMU, if there is one, except those that use what it lacks,
   such as the native context of new Linux VMs: these wait for the build.
   `host/build.sh` does the same build by hand.
2. **Create a VM.** File > New… (Ctrl+N): name, system, memory, processors,
   disk, and optionally an ISO to install from. Machine > Start (Ctrl+Return)
   starts it. Each VM is its QEMU command line, which Machine > Settings
   (Ctrl+S) edits. A Linux VM whose 3D card lacks what new VMs get (native
   context, the host vblank timing) offers Update… on its Details tab.
3. **Guest tools** for Fedora 44 x86-64 guests (virtio-gpu driver, Mesa, KWin
   for Plasma 6.7.5, an agent). The Builds window (also Build… in
   Machine > Install Guest Tools… and in Preferences) builds them in a Fedora
   container with rootless podman, then makes the medium VMs install them
   from: 20 to 40 minutes the first time (Mesa ~15-25 min, KWin ~10, tools
   ~1), up to 10 GiB of memory. Mesa and KWin wait until that much is free,
   unless told to build anyway. `guest/build-rpms.sh` then
   `guest/build-medium.sh` do the same by hand.

   Then Machine > Install Guest Tools… > Install starts the VM with the tools
   medium (Restart and Install restarts a running one); the guest installs
   them before its desktop starts, then restarts once more. It needs the
   network and Secure Boot off (the driver is not signed). Attach Medium Only
   starts the VM with the medium alone: open VITRINETOOL in the guest's file
   manager, then run `sudo bash /run/media/$USER/VITRINETOOL/install`.

   A VM's Console and Details tabs offer them only once its guest has said
   it runs Fedora 44: QEMU's guest agent tells at each boot (Fedora installs
   qemu-guest-agent in VMs), the tools' agent once they are in. Other guests,
   and guests that never said, get no offer; Install Guest Tools… shows what
   the guest was found to run.

   After an update of vitrine, the guest tools medium may be out of date:
   Update… rebuilds only the packages whose sources changed, and VMs with
   older tools then offer Update….

Keys: Ctrl+Alt+G gives the keyboard and mouse to the VM or takes them back,
Ctrl+Alt+F toggles full screen, F9 collapses or expands the VM list, F10 or
Alt with a menu's first letter opens the menus (not while the VM's screen has
the keys: they go to the VM), Ctrl+H asks the guest to shut down, Ctrl+L shows
QEMU's log. Closing vitrine can leave its VMs running; it finds them again at
its next start.

Nested desktop: the Display settings' Nested desktop checkbox runs the
guest's KDE session in a KWin of its own, drawn as a window of this
computer's desktop (virtio-gpu-rutabaga with cross-domain, beside the
standard VGA). It needs vitrine's QEMU built with rutabaga and a guest with
the nested session, "Plasma (crosvm window)", which the guest tools do not
install yet. With vitrine's window as the display, the session shows in the
console, which then has three views: Boot (the VGA card's screen: the
firmware, the boot loader, the first boot messages), Screen (the guest's
own screen: its console and its login screen) and Desktop (the session).
They change as the guest boots and its session starts or ends; the
switcher above them, View > Boot, Screen and Desktop, or Ctrl+Alt+1, 2 and
3 choose one. Closing vitrine ends the session: the guest goes back to its
login screen, where you log in again from the Screen view. With QEMU's SDL
or GTK window, or none, the session opens as a window of its own.

Host desktop (optional): the Builds window also offers this computer's own
KWin with the research's host set: steadier frames and quicker input in
VMs, and the pointer grab inside the Desktop view. The research has sets
for Plasma 6.7.91 (the KDE SIG's beta COPR) and 6.8, none for Fedora 44's
own 6.7.5: the window says so. Build builds it in a Fedora container
(`host/kwin/build.sh`, about 15 minutes, up to 10 GiB of memory). Install…
says what it does first, then, with the administrator's password, replaces
kwin, kwin-common and kwin-libs and version-locks them (`dnf versionlock`);
it takes effect at the next login. It must match the installed Plasma and
Qt: after an update of either, build and install it again, or Revert…,
which puts the distribution's KWin back. A banner says when the installed
one no longer matches. A KWin that vitrine did not install (another patched
build) is named before anything replaces it, and vitrine never reverts it.
`host/kwin/README.md` has the details, and the commands that put the
distribution's KWin back from a text console.

Shut Down goes through the guest's agent when it runs one: the guest tools',
else QEMU's guest agent (Fedora installs qemu-guest-agent in VMs). Otherwise
it presses the VM's power button, which some guests answer with a question
on their screen, as Plasma does with its logout screen: answer it there.

Clipboard: text copied on the host can be pasted in the guest, and the other
way around, through spice-vdagent in the guest (new VMs have its channel).
Text only. In a Plasma Wayland guest, text copied in Wayland applications
reaches the host only with the guest tools, which add a bridge between the
guest's Wayland and X11 clipboards. On a Wayland host, text copied in the
guest replaces the host clipboard only if vitrine's window had a key press,
a click or the mouse entering it since the last host copy; copying in the
guest with its keys or mouse does that. QEMU's SDL window shares the
clipboard too, and on Wayland waits for a key or click in it.

USB: Machine > USB Devices (also in the VM list's context menu) gives the
host's USB devices to a running VM, whatever shows its screen, and takes them
back; Keep for the Next Starts gives the VM the same devices at each start, as
its USB Devices settings do. A keyboard, mouse or Bluetooth adapter given to a
VM is no longer usable on the host: vitrine asks first. QEMU must open the
device's node (`/dev/bus/usb/BBB/DDD`); when your user cannot, vitrine asks
for access (pkexec: polkit's password dialog) and gives it with setfacl until
the device is unplugged. A udev rule gives it for good, e.g. in
`/etc/udev/rules.d/70-qemu-usb.rules`:
`SUBSYSTEM=="usb", ATTR{idVendor}=="0781", ATTR{idProduct}=="5567", TAG+="uaccess"`.

Files: VMs in `~/.local/share/vitrine/vms/<id>/` (`vm.args`, a new VM's
disk, `qemu.log`), or another folder chosen in Preferences, where vitrine
lists every folder holding a `vm.args`; vitrine's QEMU in `~/.local/share/vitrine/stack/`, the
guest tools in `~/.local/share/vitrine/guest-tools/`, QEMU's build trees in
`~/.cache/vitrine/stack-build/`, the guest tools' downloads in
`~/.cache/vitrine/guest-build/`, the host's KWin builds in
`~/.local/share/vitrine/host-kwin/` and its downloads in
`~/.cache/vitrine/host-kwin-build/`, settings in `~/.config/vitrine/settings.conf`.

## Install

```
sudo cmake --install build
```

This installs `vitrine` (also in the application menu), `host/` for building
its QEMU, `guest/` for building the guest tools, and the optional root helper
with its polkit files (polkit reads actions only from
`/usr/share/polkit-1/actions`, hence the `/usr` prefix). To update: `git pull`,
build and install again; vitrine then offers to update its QEMU and the guest
tools if needed.

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
keep that license: `app/src/nested`'s copy of the research side's vmview
library is MIT ([its licence](app/src/nested/LICENSE)). vitrine's own logo, `app/data/vitrine.svg`, is derived from
QEMU's logo, "Kew the Angry Emu" by Benoît Canet, and is under CC BY 3.0
([its note](app/data/README.md)).
