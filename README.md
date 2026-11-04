# vitrine

A Qt application for running Linux desktop virtual machines with QEMU/KVM.
The guest uses the host's GPU through virtio-gpu DRM native context, and the
app shows the guest's display in its window through QEMU's D-Bus display, or
leaves it to QEMU's own SDL or GTK window.

QEMU, virglrenderer, rutabaga_gfx, passt and the guest's graphics stack
(virtio-gpu driver, Mesa, KWin) are used at pinned versions, with the
patches vitrine needs kept in this repository: `host/versions.conf` and
`host/patches` for the QEMU, virglrenderer, rutabaga_gfx and passt vitrine
builds (upstream QEMU with a series of patches, `host/patches/qemu/series`),
`guest/` for the guest's, `host/kde` for the optional host packages (KWin,
KSvg, Kirigami, Qt base), and `host/gpu-input-awake` for an optional service
that wakes a dedicated GPU at each key or pointer event.

Early development. It is built and tried on Fedora 44 KDE Plasma (Wayland)
with an AMD Radeon 780M, with Fedora 44 KDE guests. It is not packaged.

## Requirements

- A Fedora 44 x86-64 host with KVM (`/dev/kvm`). Other distributions and X11
  are not tried; the keyboard grab and pointer lock use Wayland protocols.
- For 3D through DRM native context: on the host, an AMD GPU (amdgpu) or an
  Intel GPU on the i915 or xe driver (only AMD is tried); in the guest, Linux
  6.14 or later and a Mesa with native context, which the guest tools install in
  Fedora 44 and 45 guests ([docs/guest-mesa.md](docs/guest-mesa.md) covers others).
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

Running VMs: UEFI firmware (else new VMs use BIOS), virtiofsd for shared
folders, Qt's Wayland and SVG plugins (Plasma has them). The NAT network
uses the passt that vitrine builds with its QEMU, else the system's passt,
else QEMU's user-mode network:

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
(builds, clones) and how far they are: a click shows the task's window, or
a menu of the tasks when there are several.

File > Builds… (Ctrl+B) shows what vitrine builds: its QEMU (virglrenderer,
rutabaga_gfx, passt, QEMU), the guest tools (their packages and medium) and,
optional, the host's KWin, each with its source, its patches by origin,
whether it is up to date and why not, when it was built and how long that
took, and its log. A build redoes only what changed: a component built
from the same inputs is not built again, a changed patch compiles only the
files it changes, and the guest packages compile through ccache.

1. **Build vitrine's QEMU.** The banner's Build… button (or File > Builds…)
   opens the Builds window, whose Build button fetches QEMU, virglrenderer,
   rutabaga_gfx and passt at their pinned commits, patches them and installs
   them into `~/.local/share/vitrine/stack` (a few minutes). VMs need it for their
   display in vitrine's window and for 3D. Until it is built, they use the
   system's QEMU, if there is one, except those that use what it lacks,
   such as the native context of new Linux VMs: these wait for the build.
   `host/build.sh` does the same build by hand; vitrine takes in what it
   built (its banner, the Builds window, the VMs' QEMU) when its window
   comes back to the front.
2. **Create a VM.** File > New… (Ctrl+N): name, system, memory, processors,
   disk, and optionally an ISO to install from. Machine > Start (Ctrl+Return)
   starts it. Each VM is its QEMU command line, which Machine > Settings
   (Ctrl+S) edits. A Linux VM whose 3D card lacks what new VMs get (native
   context, the host vblank timing) offers Update… on its Details tab.
3. **Guest tools** for Fedora 44 and 45 x86-64 guests, a medium for each
   release. Fedora 44's: virtio-gpu driver, Mesa 26.2.6, KWin and KPipeWire
   for Plasma 6.8.2, Qt Base 6.11.2, KSvg and Kirigami 6.31.0, the nested
   desktop's session and proxy, an agent. Fedora 45's has the driver, Mesa
   and the agent, no KDE packages yet: a Plasma guest keeps Fedora's KWin.
   The Builds window (also Build… in Machine > Install Guest Tools… and in
   Preferences) builds those of one release at a time, its "Guest tools for"
   choice, in a Fedora container of that release with rootless podman, then
   makes the medium VMs install them from: 40 to 70 minutes the first time
   for Fedora 44 (Mesa ~15-25 min, Qt Base ~15, KWin ~10, Kirigami ~3, the
   proxy, KPipeWire and KSvg ~2 each, tools ~1), 15 to 30 for Fedora 45, up
   to 10 GiB of memory. Mesa, Qt Base and KWin wait until that much is free,
   unless told to build anyway. `guest/build-rpms.sh` then
   `guest/build-medium.sh`, with `FEDORA_RELEASE=45` for Fedora 45, do the
   same by hand, which vitrine takes in the same way.

   Then Machine > Install Guest Tools… > Install starts the VM with the tools
   medium (Restart and Install restarts a running one); the guest installs
   them before its desktop starts, then restarts once more. It needs the
   network and Secure Boot off (the driver is not signed). Attach Medium Only
   starts the VM with the medium alone: open VITRINETOOL in the guest's file
   manager, then run `sudo bash /run/media/$USER/VITRINETOOL/install`.

   The tools keep their Mesa, KWin, KPipeWire and proxy at those versions
   (dnf versionlock) until the next guest tools. Qt Base (qtbase 6.11.2,
   with the research's shm pool reuse for resized Wayland windows), KSvg and
   Kirigami (6.31.0, with the research's fix for Plasma's popups, which no
   longer upload their frames again at each opening) replace the guest's
   only where it has that version, all of a package's subpackages or none.
   Qt Base is locked too where vitrine's KWin is installed, as that KWin is
   built against Qt's private API, and so is the guest's Plasma, at the
   version that KWin is built for: Plasma's packages would otherwise update
   around it, and Plasma with a KWin of another version has no session at
   all. A guest with Fedora's older Plasma (6.7.5, which the tools held
   until 0.1.0-30) moves to Fedora's Plasma 6.8.2 in the transaction that
   installs the tools' KWin, as long as Fedora's repositories still have
   that Plasma; otherwise its KWin stays and its Plasma is no longer held.
   Where a guest got a Plasma that does not match the tools' KWin, the VM's
   Console and Details tabs say so, and installing the tools again repairs
   it. KSvg, Kirigami, and Qt Base without
   vitrine's KWin, are not: Fedora's next build of them replaces them with
   the rest of KDE Frameworks or of Qt, and the next guest tools bring
   vitrine's back. `--uninstall` puts Fedora's builds of all of them back.

   A VM's Console and Details tabs offer them only once its guest has said
   it runs Fedora 44 or 45: QEMU's guest agent tells at each boot (Fedora
   installs qemu-guest-agent in VMs), the tools' agent once they are in.
   Other guests, and guests that never said, get no offer; Install Guest
   Tools… shows what the guest was found to run. A guest gets the medium of
   its release; one that never said, that of the Fedora image the VM was
   made from (its name or volume label), else Fedora 45's.

   After an update of vitrine, the guest tools medium may be out of date:
   Update… rebuilds only the packages whose sources changed, and VMs with
   older tools then offer Update….

Keys: Ctrl+Alt+G gives the keyboard and mouse to the VM or takes them back,
Ctrl+Alt+F toggles full screen, F9 collapses or expands the VM list, F10 or
Alt with a menu's first letter opens the menus (not while the VM's screen has
the keys: they go to the VM), Ctrl+H asks the guest to shut down, Ctrl+L shows
QEMU's log. Machine > Send Key sends the VM keys the host keeps for itself,
like virt-manager's menu: Ctrl+Alt+Del, and Ctrl+Alt+F1 to F12, which switch
the guest's terminals (pressed on the keyboard, they switch the host's). With
the nested desktop, the terminal shows in the Screen view while the session
runs on. Closing vitrine can leave its VMs running; it finds them again at
its next start. SIGTERM (`kill`), SIGINT (Ctrl+C in its terminal) and SIGHUP
(its terminal closed) close it the same way without asking: its VMs run on,
settings changes not applied are lost, and what is under way stops, as with
Close Anyway. The same signal again ends it at once.

Nested desktop: the Display settings' Nested desktop checkbox runs the
guest's KDE session in a KWin of its own, drawn as a window of this
computer's desktop (virtio-gpu-rutabaga with cross-domain, one card with
VGA: virtio-vga-rutabaga). It needs vitrine's QEMU built with rutabaga and a
Fedora KDE guest with the guest tools: in the boots of such a VM, a login at
its login screen starts the nested desktop ("Plasma (nested desktop)" in
place of Plasma), and so does the guest's own automatic login into Plasma,
if set (vitrine sets none). The same disk boots as before with the box
unchecked. A VM that vitrine gave two cards before (the rutabaga card beside
the standard VGA, which made the firmware and the boot loader slow) gets the
one card at its next start. With vitrine's window as the display,
the session shows in the console, which then has two views: Screen (the
VM's screen: the firmware, the boot, the guest's login screen and its
shutdown) and Desktop (the session). They follow the session's window:
Desktop while it is there, Screen otherwise. The Console button's menu (its
name says the view shown: Console 1 or 2), View > Screen and Desktop, or
Ctrl+Alt+1 and 2 choose one; the choice holds until the session's window
comes or goes. Closing vitrine ends the session: the guest goes back to its
login screen, where you log in again from the Screen view.
So does vitrine crashing or being killed, the login screen coming about a
second later as well, once both vitrine's QEMU and the guest's tools are up
to date (the Builds window and the VM's page say when they are not): with
older ones it takes a few seconds, or the session runs on without a window.
A login while vitrine is closed (the guest's automatic login, say) ends at
once the same way: its window has nowhere to show. With
QEMU's SDL or GTK window, or none, the session opens as a window of its own.

On a computer with several GPUs (a graphics card beside the processor's
own), VMs shown in vitrine's window render on the GPU the desktop runs on,
when vitrine's QEMU has a native context for its driver (amdgpu, i915 or
xe): QEMU's D-Bus display gets that GPU's render node (`rendernode=`), and
the 3D of the VM's card follows it, virgl and native context alike. With
another (an NVIDIA card), vitrine leaves the choice to QEMU, as with one
GPU. The nested desktop's card has its own setting: by default (Automatic)
it takes the desktop's GPU on the same terms, otherwise the first GPU that
has a native context, AMD before Intel. Only that card can be given another
GPU, per VM: the Display
settings then have a GPU list for the nested desktop, which says what
Automatic takes. On another GPU the guest's frames go from one GPU to the
other, which is slower, and the guest may show nothing if the two GPUs
cannot share buffers. A chosen GPU is kept by its PCI address
(`x-render-node=/dev/dri/by-path/...` on the card's line); a VM whose GPU is
no longer in the computer does not start until another GPU, or Automatic,
is chosen. Turning the nested desktop off and on again goes back to
Automatic. All this is tried so far on a computer with one GPU only.

Host desktop (optional): the Builds window also offers this computer's own
KWin, KSvg, Kirigami and Qt base with the research's host sets. KWin's
gives steadier frames and quicker input in VMs, and the pointer grab
inside the Desktop view; KSvg's and Kirigami's make Plasma's popups cheaper
to open; Qt base's makes windows cheaper to resize. The research has KWin
sets for Plasma 6.7.91 (the KDE SIG's beta COPR) and 6.8 (`h681zu` for
Fedora 44's 6.8.2), none for Fedora 44's earlier 6.7.5, and sets for KSvg
and Kirigami 6.31.0 and Qt 6.11.2: the window says which apply. Build builds one in a Fedora container
(`host/kde/build.sh`, up to 10 GiB of memory). Install… says what it does
first, then, with the administrator's password, replaces all the installed
packages of that one (Qt base's: all from the same build) and version-locks
them (`dnf versionlock`). With KWin it also turns the set's run-time
switches on, and has KWin offer commit timing again (Plasma 6.8.2 turned it
off by default): VMs in the Desktop view pace their frames with it. Night
Light after blending (keys as fast at night as by day, saturated colours
tinted the classic way) is written off, for each user to turn on
(`host/kde/README.md`). It must
match the system: KWin the installed
Plasma and Qt, KSvg and Kirigami the KDE Frameworks' release, Kirigami Qt
too; and while Qt base is locked, Qt updates wait. After an update, build
and install it again, or Revert…, which puts the distribution's packages
back. A banner says when one no longer matches; for an update made while
vitrine runs, once its window comes back to the front. A package that
vitrine did not install (another patched build) is named before anything
replaces it, and vitrine never reverts it. `host/kde/README.md` has the
details, and the commands that put the distribution's packages back from a
text console.

Shut Down goes through the guest's agent when it runs one: the guest tools',
else QEMU's guest agent (Fedora installs qemu-guest-agent in VMs). Otherwise
it presses the VM's power button, which some guests answer with a question
on their screen, as Plasma does with its logout screen: answer it there.

Network: VMs with passt's NAT network (new VMs, when there is a passt) run
the passt vitrine builds with its QEMU, which has a fix
(`host/patches/passt`): Fedora 44's passt exits when a paused VM resumes
after one of its connections was closed or got data meanwhile, and the VM
has no network until it is started again. vitrine gives each `-netdev
passt` and `-nic passt` that passt's path at start (`path=`), unless the
VM's arguments set one; without vitrine's QEMU built, QEMU runs the
system's. vitrine's passt runs unconfined, as its QEMU does, where SELinux
can confine Fedora's `/usr/bin/passt` in its `passt_t` domain.

Clipboard: text copied on the host can be pasted in the guest, and the other
way around, through spice-vdagent in the guest (new VMs have its channel).
Text only. In a Plasma Wayland guest, text copied in Wayland applications
reaches the host only with the guest tools, which add a bridge between the
guest's Wayland and X11 clipboards. On a Wayland host, text copied in the
guest replaces the host clipboard only if vitrine's window had a key press,
a click or the mouse entering it since the last host copy; copying in the
guest with its keys or mouse does that. QEMU's SDL window shares the
clipboard too, and on Wayland waits for a key or click in it. A nested
desktop shares it through its own proxy instead, following the focus of its
window; spice-vdagent does not start there.

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
lists every folder holding a `vm.args`; vitrine's QEMU and its passt in `~/.local/share/vitrine/stack/`, the
guest tools in `~/.local/share/vitrine/guest-tools/`, QEMU's build trees in
`~/.cache/vitrine/stack-build/`, the guest tools' downloads in
`~/.cache/vitrine/guest-build/`, the host packages' builds in
`~/.local/share/vitrine/host-kwin/` (`host-ksvg/`, `host-kirigami/`,
`host-qtbase/`) and their downloads in `~/.cache/vitrine/host-kde-build/`,
settings in `~/.config/vitrine/settings.conf`.

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

GPL-2.0-or-later, see [LICENSE](LICENSE); so is `app/src/nested`'s copy of the
research side's vmview library ([its note](app/src/nested/LICENSE): copies of
its versions up to the research's 182e74d, which vitrine had until 2026-10-15,
are MIT). Patches to other projects are under the licenses of the files they
change, and files with their own license header keep that license. vitrine's
own logo, `app/data/vitrine.svg`, is derived from QEMU's logo, "Kew the Angry
Emu" by Benoît Canet, and is under CC BY 3.0 ([its note](app/data/README.md)).
