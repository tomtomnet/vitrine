# qemu-gui-manager

A VirtualBox-style manager for QEMU virtual machines on Linux, written with
Qt 6.

Each VM is its QEMU command line: a `vm.args` file in the VM's folder, one
option per line. The settings pages read and edit those same lines, and
keep everything they don't know about. So any QEMU option works, and you
can edit the file by hand at any time, in the manager or in any editor.

It goes with [qemu-gui](https://github.com/tomtomnet/qemu-gui), the QEMU
fork whose SDL window has a menu, but it runs any `qemu-system-x86_64`.

> This project was written by AI: Claude (Anthropic) wrote its code, tests
> and documentation in Claude Code, following its maintainer's requirements
> and feedback.

## Features

- A list of your VMs with their state. Start, pause, shut down, reset or
  force off. VMs keep running when you close the manager, and it picks them
  up again when it starts. A double click starts a VM, or brings the window
  of a running one to the front: on KDE Plasma, through KWin, since Wayland
  lets no app raise the window of another.
- New VMs get modern defaults: q35 with KVM and the host CPU, UEFI from your
  distribution's firmware (optionally with Secure Boot), virtio disk,
  network and GPU, SDL with OpenGL, a USB tablet, PipeWire sound and the
  shared clipboard.
- Tabs beside the list for the selected VM: its details; its settings,
  with their pages down the side: memory and CPUs, boot, display and 3D
  acceleration, disks, shared folders (virtiofs), PCI passthrough (vfio)
  and USB passthrough; and its log, which follows what QEMU writes.
  Changes to the settings wait from page to page until you apply them, and
  the manager asks before it drops any.
- The full argument list in a plain-text editor, with highlighting, checks
  and completion.
- A QEMU reference: every option, device (with its properties), machine, CPU
  model and backend of the QEMU you run, searchable, with an Insert button.
  When QEMU runs from a build tree, it includes the full documentation from
  `qemu-options.hx`.
- Snapshots, in a tab of their own: take one, go back to one, start the VM
  from one or delete one. Taken while the VM runs, a snapshot keeps its
  running state too.
- Clone a VM: a new VM with the same settings and copies of its disks
  and firmware; CD/DVD images stay shared, and the network cards
  get new addresses.
- Import a launch script: the manager takes the QEMU command out of it.
- A different QEMU build per VM, if one needs it.
- How smoothly the selected VM runs, in the status bar while it runs:
  frames on screen and their latency, input latency, and whether QEMU's
  main loop waits for a CPU. See [Performance](#performance).
- An Updates button in the status bar when qemu-gui-manager, or the qemu-gui
  QEMU that File > Build QEMU builds, has new commits on GitHub.

## Build (Fedora)

    sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel
    git clone https://github.com/tomtomnet/qemu-gui-manager.git
    cd qemu-gui-manager
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ./build/qemu-gui-manager

To install it to `/usr/local`, with its menu entry, run
`sudo cmake --install build`.

To run VMs, it also needs QEMU (build [qemu-gui](https://github.com/tomtomnet/qemu-gui),
or `sudo dnf install qemu-system-x86-core`) and:

    sudo dnf install qemu-img edk2-ovmf virtiofsd hwdata

Choose the QEMU binary in File > Preferences. It defaults to the
`qemu-system-x86_64` in your `PATH`.

## The vm.args file

    # Fedora with a shared folder
    -name Fedora
    -machine q35,accel=kvm,memory-backend=mem
    -cpu host
    -smp 8
    -object memory-backend-memfd,id=mem,size=8G,share=on
    -drive file=disk.qcow2,if=virtio,format=qcow2
    -device virtio-vga-gl
    -display sdl,gl=on
    #share tag=public,path=/home/me/Public

- One option per line. The value is the rest of the line, as is: no shell
  quoting, so spaces need nothing special.
- Lines starting with `#` are comments, except for the manager's own
  directives, which QEMU never sees:
  - `#share tag=...,path=...[,cache=auto|always|never][,readonly=on][,mount=DIR]`
    shares a host folder with virtiofs, which the guest's agent mounts at DIR.
  - `#qemu /path/to/qemu-system-x86_64` runs this VM with that QEMU instead
    of the one in the preferences.
- QEMU runs in the VM folder, so relative paths, like `disk.qcow2` above,
  point into it.

## Building QEMU and virglrenderer

File > Build QEMU downloads [qemu-gui](https://github.com/tomtomnet/qemu-gui),
builds just the emulator for your computer's architecture (x86-64, or
aarch64 on ARM such as Asahi Linux) and `qemu-img`, and makes the result the
QEMU of your VMs. Building again updates it first. It needs QEMU's build
dependencies: `sudo dnf builddep qemu`. To run another QEMU, choose it in
the preferences, or for one VM with a `#qemu` line.

**Branch** chooses what to build: `master`, or one of the fork's
experimental branches, listed from GitHub. Each experiment's README says what
it changes and needs, and building `master` again goes back. A branch that
needs changes to virglrenderer carries them as patches in
`contrib/qemu-gui/virglrenderer/`, which the build applies after its own.

Tick **DRM native context** to also build a virglrenderer with the
native context renderer of every GPU that has one. The guest's GPU driver
then talks to the host GPU's own driver, for 3D acceleration close to the
host's:

- Most distributions' virglrenderer has no native context renderer.
  Fedora's has none.
- The build has Intel (Xe included, through a patch from
  [xe-native-context-enablement](https://github.com/cmspam/xe-native-context-enablement)
  that isn't upstream yet), AMD, Qualcomm, Apple (Asahi), Arm Mali, and
  Venus for Vulkan.
- For AMD, [a patch of ours](data/patches/virglrenderer-amdgpu-force-wc.patch)
  maps the GPU memory the guest writes into with write-combining. Without
  it the guest's uploads stall, and KDE and other Qt desktops stutter, the
  more the bigger the windows. It's a pragmatic fix, not upstream; it
  needs `-accel kvm,honor-guest-pat=on`.
- The patches go on upstream virglrenderer, or on the newest release they
  all apply to. When a manager update brings a new patch, build again:
  virglrenderer is patched and recompiled.
- virglrenderer installs into the manager's data folder, and the QEMU it
  builds loads it from there. The system's virglrenderer stays as it is,
  and a rebuilt one takes effect at the next start of a VM.

This needs virglrenderer's build dependencies too:
`sudo dnf builddep virglrenderer`. The guest needs native context support
in its Mesa (for Xe, the Mesa patch of the repository above). A VM uses it
with `-device virtio-vga-gl,blob=on,hostmem=4G,drm_native_context=on`
(`virtio-gpu-gl-pci` on ARM), and with `-accel kvm,honor-guest-pat=on`,
which Intel needs and the AMD patch relies on. The Display settings add
it, and so does importing a script that uses native context.

A guest whose Mesa has no native context for the GPU falls back to virgl
without a word, and most distributions leave it out (Arch has it for AMD,
Intel i915 and Qualcomm). To build a guest Mesa with it, on Fedora, Debian,
Ubuntu or Arch, see [Building Mesa with native context](docs/guest-mesa.md).
With the qemu-gui build of QEMU, Details says whether the running guest
really uses native context: QEMU counts the contexts the guest creates of
each kind (`qom-get` of `x-drm-offered`, `x-drm-contexts` and
`x-virgl-contexts` on the GPU), and a warning shows when the guest draws
through virgl, or when this computer offers no native context.

## Shared folders

For each `#share`, the manager starts a `virtiofsd` as you, and adds the
virtiofs device to QEMU. virtiofs needs the guest RAM in shared memory. The
Shared Folders page sets that up when you add a folder.

The guest can mount a folder by itself at each start, where the share's
`mount=` says (`/mnt/<name>` by default for new folders). The manager
tells the guest's systemd, 254 and later (Fedora 39, Debian 13, Ubuntu
24.04 and later), through a credential in SMBIOS:
`-smbios type=11,value=io.systemd.credential.binary:fstab.extra=...`.
systemd mounts those folders at boot, as if they were in `/etc/fstab`.
The guest needs nothing installed for that.

Older guests need the QEMU guest agent, which starts by itself once
installed:

    sudo apt install qemu-guest-agent

The manager gives the VM the agent's channel. When the agent comes up at
boot, the manager has it create the folder and mount the share there, as
root, unless systemd has already. The status bar says when the folders
are mounted, and a message says what went wrong. SELinux, as in Fedora
and RHEL, confines the agent: it may not mount, so there only systemd's
mounts work (RHEL 9 and its clones have systemd 252). To mount a folder
by hand instead:

    sudo mount -t virtiofs public /mnt/public

or in `/etc/fstab`:

    public  /mnt/public  virtiofs  defaults,nofail  0  0

## Snapshots

The Snapshots tab works with QEMU's internal snapshots, kept inside the
qcow2 files of the VM, as the snapshot button of the qemu-gui menu does:

- While the VM runs, QEMU takes them with the running state (`savevm`,
  which pauses the VM while it saves its memory) and goes back to them
  (`loadvm`). A snapshot of the disks alone needs the VM stopped to go back
  to.
- While it is stopped, `qemu-img snapshot` takes them of the disks alone,
  and goes back to them. Start From It starts the VM with `-loadvm`, where
  a snapshot with the running state left it.
- A snapshot of the disks costs nothing when taken: the qcow2 file only
  grows as the guest writes over what the snapshot keeps, since the old
  data stays for it. One with the running state adds the saved memory of
  the VM to the file at once (the Running state column). Deleting a
  snapshot frees its space for reuse inside the file.
- They are not backups: they live in the same files as the VM, and go with
  them. They help before an update or an experiment, which a snapshot can
  undo.
- Only qcow2 files can hold them. A snapshot with the running state needs
  every file the VM writes to in qcow2, UEFI variables included: new VMs
  get them in qcow2 where the distribution's firmware comes so, as
  Fedora's `OVMF_VARS_4M.qcow2` does.
- QEMU cannot save the running state of a VM with 3D graphics (virgl, "not
  yet migratable"), nor of one with a raw file it writes to. A snapshot of
  such a running VM holds its qcow2 disks only, all at the same point (a
  QMP transaction of `blockdev-snapshot-internal-sync`), as the qemu-gui
  menu does, and the tab says so. The raw files are left out.

## UEFI firmware

A UEFI VM keeps copies of the firmware and of its variable store (boot
entries, boot order, Secure Boot keys) in its folder. A power cut while it
runs, or a slip of the hand, can damage or delete them:

- A missing copy is made again from the distribution's firmware when the
  VM starts, and the log says so. Deleting the variable store is one way
  to reset it.
- Before a start, the manager checks the copies: their qcow2 tables and
  headers, against the firmware they come from. If one looks damaged, it
  offers new copies, or to start anyway. When QEMU refuses one, the error
  offers the same.
- Settings > System > Reset UEFI Variables gives the VM the variables of a
  new one, for a system that no longer starts, e.g. when the firmware stays
  on a black screen. Most systems still start, from the fallback boot
  loader they install.
- The old files stay in the VM folder, renamed to end in `.bak`. The
  snapshots of a variable store that can still be read keep the variables
  they were taken with.

## PCI passthrough

The PCI page lists your devices by IOMMU group, and says what is still
missing for each one:

- The IOMMU must be on, in the firmware (VT-d or AMD-Vi) and in the kernel.
  AMD kernels turn it on by themselves. On Intel, you may add `intel_iommu=on` to
  the kernel command line.
- The device must be bound to `vfio-pci`, along with every device in its
  IOMMU group (bridges excepted), for example with driverctl
  (`sudo dnf install driverctl`):
  `sudo driverctl set-override 0000:03:00.0 vfio-pci`.
- You need read and write access to `/dev/vfio/<group>`, for example through
  a udev rule.
- QEMU locks all the guest RAM in memory, so your locked memory limit
  (`ulimit -l`) must cover it. Raise it with `DefaultLimitMEMLOCK=` in
  `/etc/systemd/system.conf` for your desktop session, and `memlock` in
  `/etc/security/limits.conf` for terminal logins, then log in again.

## USB passthrough

The USB page passes devices through by vendor and product ID. QEMU needs
read and write access to the device's node in `/dev/bus/usb`, and without it
QEMU silently doesn't attach the device. So when you start a VM, the manager
asks for access to its plugged-in devices that you can't open yet. Your
desktop shows its password dialog (polkit), like the menu of qemu-gui does.
The access lasts until the device is unplugged; if it isn't given, the
manager tells you which devices the VM won't get.

To give a device access for good, add a udev rule:

    # /etc/udev/rules.d/70-qemu-usb.rules
    SUBSYSTEM=="usb", ATTR{idVendor}=="046d", ATTR{idProduct}=="c52b", TAG+="uaccess"

## Performance

While the selected VM runs, the status bar shows how smoothly it goes, for
example `240 fps · frame 6.1 ms · input 11 ms · main loop wait 0.4 %`. The
manager asks QEMU twice a second; the tooltip has the details.

- **fps**: guest frames that reached the screen per second, over the last
  second. **frame**: the median time from QEMU receiving a frame from the
  guest to the compositor showing it (Wayland); on X11 it ends when QEMU
  hands the frame to the window system, and says `(to swap)`.
  **input**: the median time from a key press or click in the VM window to
  the guest's answer on screen, over the last 10 seconds; only measured when
  the guest had drawn nothing just before, so it is there after you type or
  click on a quiet desktop, and not while a game or a video plays.
  These need the SDL display of a current qemu-gui QEMU (QMP
  `x-query-display-stats`).
- **main loop wait**: the share of the time QEMU's main loop was ready to run
  but waited for a CPU, from `/proc/PID/task/TID/schedstat`. The main loop
  draws the frames and runs the emulated devices, so when the host is busy
  and this grows, the VM stutters. The tooltip also has the CPU use of the
  main loop, the vCPUs and QEMU's other threads, and how long the vCPUs
  waited.
- **KVM**, in the tooltip, from QMP `query-stats`: VM exits per second (each
  costs about 4 µs on a Ryzen host with its branch predictor flushed), halt
  exits, exits for emulated devices, and how often halt polling ended a halt.

## Files

| What | Where |
| --- | --- |
| VMs, one folder each | `~/.local/share/qemu-gui-manager/vms/` |
| Settings | `~/.config/qemu-gui-manager/settings.conf` |
| Sockets of running VMs | `$XDG_RUNTIME_DIR/qemu-gui-manager/` |
| QEMU reference index | `~/.cache/qemu-gui-manager/` |

Removing a VM moves its folder, disks included, to the trash.

## Tests

    ctest --test-dir build

Some tests run QEMU itself. Point them at a binary with
`QGM_TEST_QEMU=/path/to/qemu-system-x86_64`; otherwise they use the one in
your `PATH`, or skip.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

The icon is the QEMU logo by Benoît Canet, under
[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/), with its colors
inverted.
