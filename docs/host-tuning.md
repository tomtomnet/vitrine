# Host tuning while VMs run

A desktop VM stays smooth under host load when the host gives it a hand:

- **Real-time QEMU threads** (SCHED_FIFO priority 1) for the VM in front -
  its console or full-screen window has the keyboard: the guest's vCPUs and
  QEMU's display and I/O threads run before ordinary host tasks. The other
  VMs' threads stay ordinary, their vCPUs at nice -5, so that VMs busy in
  the background cannot take every CPU at real-time priority: one VM is
  real-time at a time. A VM with more processors than half of the host's
  online CPUs (9 or more on 16) never is, even in front: its threads stay
  ordinary and its vCPUs at nice -5, as a VM behind. Its vCPUs at real-time
  priority would hold most of the host while the guest is busy: on
  2026-10-30 a 16-vCPU VM in front froze a desktop with 16 CPU threads, the
  shorter fair-server period below notwithstanding. The helper counts the
  vCPU threads QEMU names (`-name ...,debug-threads=on`, as vitrine's VMs
  have), else the VM's `-smp`. Preferences > Host's state line gives this
  computer's number, and the status bar says it when such a VM comes to the
  front. A host window in front changes nothing; a VM alone
  is real-time, and so is the first one started or found running until
  another comes to the front. A VM in QEMU's own window (SDL, GTK) is in
  front when that window is the active one: on KDE Plasma a small KWin
  script tells vitrine which window is active, while such VMs run. On
  other desktops vitrine cannot tell: such a VM goes behind as soon as
  another VM comes to the front, and comes back only when it is alone.
- **Real-time view threads** for the VM in front, in the GTK app: the
  threads that show its screen and take its input - the app's main thread
  (GTK's: it passes the keyboard and mouse to QEMU and draws the window),
  the view's render thread, its Wayland reader and D-Bus listener, GLib's
  D-Bus worker - go to SCHED_FIFO 1 with its QEMU's, and back to ordinary
  when it goes behind. What they start begins ordinary
  (SCHED_RESET_ON_FORK): QEMU, builds and the app's other threads. QEMU's
  own window does its copies on QEMU's threads; vitrine's view runs
  inside the app, so its threads are raised alongside. On an Arc Pro B50
  under a 4-job compile, a GNOME guest's full screen lost 3.0 frames a
  second at normal priority (6 boots), about 0.9 with the view's own
  threads real-time, with the main thread too (the default) or with every
  thread of the view's process (the research's runs, 2026-11-09: their
  medians alike). The GPU driver's threads did not help, and with the main
  thread ordinary they made it worse (1.8), so they stay ordinary. On a
  Radeon 780M the compile cost it little either way.
  A KDE guest's nested desktop in vitrine's window (its Desktop view)
  takes one more thread, in both apps: the nested library's, which passes
  the guest's frames on to the desktop's compositor and its events back.
  It goes real-time with the VM's QEMU too, and so do the app's other
  readers of its Wayland connection, which that thread's reads wait for
  (libwayland reads a connection only once every thread that prepared to
  read it has): the GTK app's main thread, as above; in the Qt app, Qt's
  two Wayland event threads and the screen's reader. Those of other VMs
  stay as they are while their VMs are behind.
- **A shorter fair-server period**: the kernel keeps some CPU time for
  ordinary tasks while real-time ones run, 50 ms per second by default. An
  ordinary task stuck behind a real-time vCPU (a kernel worker pushing GPU
  jobs, say) could then wait up to 950 ms; at 1 ms every 10 ms it waits
  under ~9 ms. This is the only bound the kernel has on real-time threads
  (real-time throttling went in Linux 6.12, except with real-time group
  scheduling, which Fedora's kernel leaves out), so where it cannot be
  set, QEMU's threads stay ordinary: see below. Under a sched_ext
  scheduler, ordinary tasks wait for the ext server instead, which gets the
  same.
- **A GPU clock floor** (AMD APUs): at moderate load the GPU stays at its
  lowest clock, where copying a 4K frame takes about 1 ms. With the lowest
  gfx clock at 1800 MHz a Radeon 780M showed new images at 94-95 % of the
  refreshes during window animations instead of 87-90 %, for about 0.3 W.
  On a dedicated GPU (Intel xe, an AMD card) a floor does nothing; there,
  the optional gpu-input-awake service wakes the GPU at each key or pointer
  event instead (below).
- **A GPU power profile** (AMD's dedicated GPUs): amdgpu's `3D_FULL_SCREEN`
  profile instead of its default, `BOOTUP_DEFAULT`. With the default, an RX
  6900 XT stayed at about 550 MHz, 68 % busy, under a guest drawing on it,
  and the VM's desktop repeated frames: its launcher 21.7 and 19.3 repeated
  frames a second against 0.2 and 0.0 with `3D_FULL_SCREEN`, window
  operations 171 and 146 repeated refreshes against 99 and 44, a 1080p60
  video 38 % on cadence against 100 %; for the same power, 2 W less under
  load and 1 W more at idle. Only from the default profile, in performance
  level `auto`: a profile or level someone chose (by hand, or a tool such
  as LACT or CoreCtrl) is left as it is, and the journal says so. The
  Automatic of Preferences > Host > GPU Clock Floor covers both: the floor
  on APUs, this profile on dedicated AMD GPUs; Off or a fixed floor, no
  profile.
- **Huge pages for VM memory**: shared memory's transparent huge pages go
  from `never` (Fedora's default) to `advise`. QEMU asks for them on the
  guest's memory (a memfd, which vitrine's QEMU aligns to 2 MiB): each
  first touch of guest memory is one fault per 2 MiB instead of 512. A
  guest's first app start after its boot took 0.79 s instead of 1.38 s.
  Any other value is the host owner's choice, and is left as it is.
- **The CPU ready to wake up**: its cores rest in their first idle state
  only, at the energy performance preference `performance`. A VM's keys
  then reach its screen as fast as on the bare desktop: 11.3-11.9 ms
  instead of 15.1-16.3 ms on a Ryzen 7 260 laptop, for about 2 W at
  idle; 7.49 ms instead of 10.09 ms on a desktop under GNOME, for 3.5 W.
  Preferences > Host turns it off ([below](#the-cpu-ready-to-wake-up)).
- **Higher udmabuf limits** (VMs whose GPU has native context, the nested
  desktop's included): QEMU
  hands the host GPU each guest buffer in guest memory as a udmabuf, one
  entry per contiguous piece of guest RAM. A maximized 4K window drawn by
  the CPU (Qt Widgets and GTK apps, cursors) is about 32 MB in 1,200 to
  8,000 pieces; the kernel's defaults, 1024 entries and 64 MB, refuse it,
  and the guest's compositor then copies that window at each change. The
  helper raises them to 65536 entries and 2048 MB.

These need root. vitrine applies them through a small helper,
`vitrine-helper`, when a VM starts, and the helper puts everything back
after the last VM stops - crash included, and even if vitrine has quit
while the VMs run on. There is no service: the helper runs only while VMs
run. Nothing is applied while no VM runs. QEMU itself gets no privilege:
the helper sets its threads' scheduling, and that of the view's threads.

The same helper gets PCI devices ready for a VM that passes them through
([below](#pci-devices-passed-through)), whether host tuning is on or not.

## Allowing it without a password

The helper runs through polkit (`pkexec`). By default it would need an
administrator's password; vitrine never asks for one when a VM starts - it
runs the VM without these settings, and while such VMs run the status bar
shows "Host tuning inactive" (its tooltip says why, a click checks again
and says what to do). Preferences > Tune the host while VMs run says
whether tuning is active, and if not why. With that box unticked, vitrine
shows no warning.

Members of the `vitrine` group use the helper without a password, from
their local, active desktop session (not over ssh or waypipe, not from a
session switched away from). When VMs run untuned for want of the group,
vitrine offers once per run to set it up: Set Up, Not Now, or Turn Off
Tuning. Set Up (also in Preferences and behind the status-bar warning)
runs `vitrine-helper setup-group` through pkexec: the desktop's polkit
dialog asks for an administrator's password, then the helper creates the
group if there is none and adds you to it. Without the helper installed,
the warning says how to install it; the offer comes once it is. Instead of
Set Up, by hand:

```
sudo groupadd --system vitrine
sudo usermod -aG vitrine "$USER"
```

polkit reads the groups from the user database at each check, and vitrine
asks it again before each start of the helper, when its window comes back
to the front while VMs run untuned, and when Preferences or the warning's
explanation open. A new membership, or the helper installed meanwhile,
counts then, for the VMs running too, without logging in again or
restarting vitrine (`id` in a terminal lists it only after a new login).
Preferences > Tune the host while VMs run turns tuning off for you, at
once for the VMs running too (and on again, another GPU clock floor, or
the CPU's switch, the same way).

To take it back: `sudo gpasswd -d "$USER" vitrine` (and
`sudo groupdel vitrine` when nobody is left in it), and untick
Preferences > Tune the host while VMs run. From the next VM start on,
vitrine runs the VMs without these settings, without asking for a
password; with the box still ticked it also shows the warning while they
run, and offers Set Up again once per run.

## What the group allows

Polkit decides who may start the helper; the helper decides what it does
for them. It does only this, for the user who started it:

| Request | What it does | Checks |
| --- | --- | --- |
| `watch PID` | Watches a QEMU until it exits | The process belongs to the caller (all its uids) and runs a `qemu-system-*` (or `qemu-kvm`) executable; held through a pidfd, so a reused pid cannot slip in |
| `fair-server on` | 10 ms / 1 ms on every online CPU, read back; under sched_ext the ext server too | Fixed values; only `cpuN` folders; nothing to choose. All or nothing: a refused write puts the others back and says which CPU and why |
| `gpu-floor CARD MHZ\|auto` | An AMD GPU's lowest gfx clock | `cardN` of vendor 0x1002 driven by amdgpu; the clock within the GPU's own overdrive range; only when its performance level is `auto` |
| `gpu-profile CARD on` | A dedicated AMD GPU's power profile at `3D_FULL_SCREEN` (`pp_power_profile_mode`) | `cardN` of vendor 0x1002 driven by amdgpu, not an APU; a fixed profile; only from `BOOTUP_DEFAULT` and in performance level `auto`; the profiles' numbers read from the file; recorded first, one write, read back, no retry |
| `rt PID` | SCHED_FIFO 1 on every thread of a watched QEMU (the VM in front), its real-time time limit (`RLIMIT_RTTIME`) lifted first | Only once `fair-server on` holds; watched first, and checked again to be the caller's QEMU (it may have run another program since); never for more vCPUs than half of the online CPUs (its threads named as vCPUs, else its `-smp`): such a VM is lowered as by `behind` instead, and the answer is `skip`; no thread made real-time if the limit cannot be lifted; real-time threads it finds are left as they are; its child processes (passt) are not touched |
| `behind PID` | The threads `rt` made real-time back to SCHED_OTHER, its vCPU threads (`CPU n/KVM`) at nice -5 (a VM behind the one in front) | The same checks as `rt`; nice -5 only while the fair server holds, and only for vCPUs at nice 0 |
| `view-rt TID` | SCHED_FIFO 1, with SCHED_RESET_ON_FORK (what the thread starts begins ordinary, threads and processes alike), on one thread of the program that started the helper: vitrine's view of the VM in front (the app's main thread, the view's render thread, Wayland reader and D-Bus listener, GLib's D-Bus worker; for the nested desktop the library's thread and the other readers of the app's Wayland connection) | Only once `fair-server on` holds, and while a watched QEMU is in front (its `rt`); a thread of the helper's parent - the program that ran pkexec, taken at the start and held through a pidfd - whose uids are all the caller's; threads at SCHED_FIFO 1 before its first `view-rt`, other real-time ones and SCHED_IDLE ones are left as they are; recorded first, as for `rt` |
| `view-behind` | Those threads, and any other thread of that program at SCHED_FIFO 1 since, back to SCHED_OTHER | Only threads at SCHED_FIFO 1 that were not before the first `view-rt`; the helper also does it whenever no watched QEMU is in front any more (`behind` of the one in front, its exit), at `fair-server off` and at its end |
| `udmabuf PID` | The udmabuf module's `list_limit` at 65536 and `size_limit_mb` at 2048, while that watched QEMU runs | A watched QEMU (its GPU is not checked: vitrine asks only for native-context ones and the nested desktop's); fixed values; raised only, a higher value stays; skipped when the module is not loaded; put back after the last QEMU that asked for it |
| `thp on` | `/sys/kernel/mm/transparent_hugepage/shmem_enabled` at `advise` | A watched QEMU; a fixed value, written only over `never`; read back |
| `cpu-latency on` | A request on `/dev/cpu_dma_latency` for the wake-up time of the CPUs' first usable idle state (C1), held by the helper's open file | A watched QEMU; the value from cpuidle, at least 1 µs; nothing recorded: the kernel drops the request with the file |
| `epp on` | Each online CPU's `cpufreq/energy_performance_preference` at `performance` | A watched QEMU; a fixed value; read back; all or nothing |
| `setup-group` | The caller in the `vitrine` group, the group created (`groupadd --system`) if there is none | See below |
| `vfio ADDRESSES MIB` (`devices`) | For IOMMU groups already on vfio-pci, before the VM's QEMU starts: `/dev/vfio/<group>` the caller's (owner read-write, mode kept), and the locked memory limit (`RLIMIT_MEMLOCK`) of the program that started the helper at MIB at least | Strict addresses (`0000:6e:00.0`) of devices that exist; every device of the group on vfio-pci, pci-stub or no driver, or a bridge; no device taken from a driver or bound to vfio-pci (a helper of any verb gives back, as it starts, what a killed one left); a group another helper holds, or that a program has open, refused; the limit raised only, to the computer's memory and 1 GiB at most, which the answer says |
| `vfio ADDRESSES MIB` (`devices-bind`) | The same, and the devices taken from their drivers for vfio-pci first (`driver_override`, the driver's unbind, vfio-pci's bind, read back; vfio-pci loaded if need be) | Also refused: the GPU the computer started on (`boot_vga`), a GPU with a display connected, a device whose nodes a process has open (but the sound servers on its ALSA nodes: PipeWire, WirePlumber and PulseAudio, by the program they run, `/usr/bin/pipewire` and so on), a disk mounted, swapped on or with holders (dm, md), a network interface that is up, NVIDIA's driver while its nodes are open, platform devices (bridges, SMBus, the IOMMU, timers); the whole group or nothing; all of it recorded before the first change |

The settings need a watched QEMU, and each value is put back only if it
still holds what the helper wrote: another tool that changed it since keeps
its change. The CPU latency request is the helper's own: no one else can
change it, and it ends with the helper. `vfio` is the one request that
comes before its QEMU, which opens the devices and pins the guest's memory
as it creates them; see [below](#pci-devices-passed-through).

What this amounts to:

- Real-time priority 1 for your own processes, and nice -5 for their
  threads named as vCPUs. The `qemu-system-*` name is no proof of
  anything, and a guest's own code runs on those vCPUs: think of the group
  as `rtprio 1` in `limits.conf`. The program that runs the helper gets it
  for threads of its own too (`view-rt`), while one of its QEMUs is in
  front. The shorter fair-server period keeps
  ordinary tasks running beside them, and without it there is no
  real-time priority.
- Host-wide settings changed while your VMs run, and put back after: the
  fair server, a GPU clock floor or power profile, shared memory's huge
  pages and the CPUs'
  energy performance preference; and a request that keeps the CPUs out of
  their deeper idle states while the helper runs.
- Bigger udmabufs. `/dev/udmabuf` is open to the user at the desktop
  already (systemd's `uaccess` rule), and to the `kvm` group: they can turn
  their own memory into a udmabuf. The limits only set how many pieces and bytes
  one udmabuf may have, and it is memory its owner has anyway. While they
  are raised, they are raised for every user of the host.
- For PCI devices already set aside for VMs (on vfio-pci, bound at boot),
  their `/dev/vfio` nodes while a VM of yours has them, and a locked memory
  limit as big as that VM's memory, without a password. Taking a device
  from the host's drivers (`devices-bind`) always takes an administrator's
  password, from members too: it gives a program of yours the device
  itself, a disk controller that is not mounted for one, its data
  included. Without a password, a helper may give devices back to their
  drivers: those a killed helper left on vfio-pci, as it starts.
- Nothing for QEMU itself. Earlier versions gave vitrine's QEMU build the
  `cap_sys_nice` file capability, so that QEMU could switch its vCPUs
  itself; that also gave a guest that escaped into QEMU the scheduling of
  every host process, turned off `LD_LIBRARY_PATH`, `LD_PRELOAD` and
  `TMPDIR` for QEMU, and kept core dumps and debuggers away from it.
  vitrine now takes that capability back from its builds when it starts
  (as their owner: no password), and the helper does the scheduling. The
  one thing QEMU loses is amdgpu's high-priority GPU contexts, which the
  guest's compositor asks for and falls back from; no difference was
  measured. Without the capability, PipeWire (the VM's sound) asks RTKit
  for a real-time audio thread, and sets for that a real-time time limit
  of 200 ms on the whole QEMU: the kernel kills QEMU, silently, once a
  real-time vCPU runs that long without a pause. vitrine starts QEMU with
  `DISABLE_RTKIT=1`, so its audio thread is real-time only with the VM's
  other threads, and the helper lifts such a limit before it makes threads
  real-time.

`setup-group` takes no argument: the group is `vitrine` and the user is
the caller (pkexec's `PKEXEC_UID`, looked up in the user database),
never anyone else. It cannot add another user, add anyone to another
group, remove anyone, or change a group's other members; it refuses root
and uids the user database does not know. When the caller is a member
already it changes nothing. `groupadd` and `gpasswd` do the editing (they
lock and update `/etc/group` and `/etc/gshadow` together), and the
journal records it.

It joins an existing `vitrine` group only if it looks like the one it
would have created: a system group (an id from 1 to `SYS_GID_MAX` of
`/etc/login.defs`, 999 by default) listed in `/etc/group`, alone with its
id, and nobody's primary group. Any other group of that name - a user
named vitrine's private group, a group sharing the id of `disk` or
`wheel`, one from LDAP - would give you that group's access to files, which
the password dialog does not mention: setup-group refuses it ("a group
named vitrine exists that vitrine did not create") and changes nothing.
Rename that group, or add yourself to it by hand if that is what you want.

Each verb of the helper has a polkit action of its own, named by its
first argument: `org.vitrine.helper` (`session`, the settings while VMs
run), `org.vitrine.helper.devices` (PCI devices already on vfio-pci),
`org.vitrine.helper.devices-bind` (devices taken from the host's drivers)
and `org.vitrine.helper.setup-group`. The group's rule grants the first two
only: devices-bind wants an administrator's password from members too,
which polkit keeps for a few minutes (`auth_admin_keep`; the rule has a
commented line that would grant it as well), and setup-group always wants
one, its dialog saying it adds you to the vitrine group. A rule of your own that gives the helper to more
people, or lets them use their own password, does not bring a lasting
change of `/etc/group` along unless it names setup-group's action.
(pkexec takes the first action whose path matches and whose first
argument, if the action names one, matches too, in no fixed order: each
action names its argument, so exactly one matches, and the helper refuses
to run without one, which pkexec would put under its generic action.)

Give the group to the people you would give real-time priority to.

## PCI devices passed through

A VM's PCI devices (`-device vfio-pci,host=...`) must be ready before its
QEMU starts: QEMU opens `/dev/vfio/<group>` and has the kernel pin all of
the guest's memory for the device's DMA as it creates the device, before
its guest runs, and the IOMMU driver checks the pinned pages against the
locked memory limit QEMU started with (Fedora's: 8 MiB). So at such a VM's
start, after virtiofsd and before QEMU, vitrine runs one helper for it -
`vitrine-helper devices` when the devices are on vfio-pci already,
`devices-bind` when one must change driver, from a host driver, pci-stub
or none - and asks for `vfio` with the VM's devices and its memory and 1
GiB (once per device with a virtual IOMMU in `vm.args`). `devices` never
takes a device from a driver, but may give one back that an earlier helper
left: when that leaves the VM's devices on their drivers again, vitrine
asks once more, with `devices-bind` and its password. The helper raises
vitrine's own limit, to this computer's memory and 1 GiB at most: QEMU,
which vitrine starts next, inherits it. Then vitrine gives the helper the
QEMU to watch, and the helper puts vitrine's limit back at once (QEMU has
its own copy). When that QEMU ends, crash included, the helper gives
everything back and ends: each device off vfio-pci, its `driver_override`
as it was, its driver's bind, read back (the kernel's probe instead, where
that driver is no longer loaded). A device on vfio-pci before stays there,
its node going back to its owner and mode. If no QEMU comes - the start
failed after, vitrine went - everything goes back when the helper's input
closes, or after a minute, vitrine's limit then too, even while a group
stays because something has it open. Loading vfio-pci, when `devices-bind`
needs it and it is not loaded, also binds the devices that an `options
vfio-pci ids=` line names, as at boot: they stay on it. vitrine waits a
minute at most for the helper's answer once it runs (after the password
dialog, if any).

A group is given back only once nothing has it open: unbinding a vfio
device that a process holds waits in the kernel until it lets go. One
still open stays on vfio-pci, its node root's again, its record kept;
the next helper to start, of whatever verb, gives it back once it is free.
Each group has a record (`vfio-<group>` in `/run/vitrine-helper`, written
before the first change, each device's vendor and device ID in it: a
device gone, or another one at its address since, is left as it is) and
one helper at a time: a group opens once, two VMs cannot share it. The
helpers' lock on the other settings is not held while a driver lets go
of a device or takes it back: a slow driver keeps only that group
waiting. vitrine's limit has a record too (`memlock-<pid>`), shared by
the helpers of VMs starting side by side: the first one saves it, the
last one out puts it back, if it still reads what was written. A helper
killed in the middle leaves records the next one follows.

When everything is ready already - the devices bound at boot, a udev rule
giving you the node, a limit of your session's - vitrine runs no helper.
When a device does not go back to its driver (a GPU that does not reset
well), it stays without one, the status bar says so, and a restart of the
computer gives it back.

## The CPU ready to wake up

A core with nothing to do drops into an idle state, and the deeper the
state, the longer it takes to wake up: on a Ryzen 7 260, C1 takes 1 µs, C2
18 µs and C3 350 µs. Its energy performance preference (amd-pstate and
intel_pstate in active mode) sets how fast it clocks up once awake. The
research measured a GNOME 51 VM on such a laptop (a GNOME 51 host, QEMU
grabbing the keyboard), from a key to the VM's screen, on 2026-11-04 and
05:

| Host | Key to screen | QEMU reads the key after (p50 / p90) |
| --- | --- | --- |
| tuned's balanced profile with a 20 µs request (cores reach C2) | 15.1-16.3 ms | 2-3 ms |
| C1 only (a 1 µs request) | 13.7 ms | 1 ms |
| Energy performance preference `performance` only | 14.7 ms | |
| Both, as tuned's latency-performance profile | 11.3-11.9 ms | 1 / 1 ms |
| No idle state at all (a 0 µs request) | 11.7 ms, for 26 W more | |

The bare desktop shows keys in 10.6-11.7 ms there. Both together cost about
2 W at idle: 10.4 W against 8.35 W with a VM idle in front (3 x 60 s; a first
20 s sample had said 1.6 W). While VMs run, host tuning holds both:

- A PM QoS request on `/dev/cpu_dma_latency` for the wake-up time of the
  CPUs' first usable idle state - not POLL, not disabled; the slowest
  CPU's - so that each core may rest there and nowhere deeper: 1 µs on that
  laptop, 2 µs on Intel CPUs whose C1 takes 2. Never 0, which leaves the
  cores nothing but polling. `journalctl -t vitrine-helper` shows the value
  and the state ("1 us (C1 on every cpu; C2, C3 unused)").
- `performance` as each online CPU's energy performance preference, where
  the cpufreq driver has one; elsewhere (acpi-cpufreq, intel_pstate in
  passive mode) the helper skips it and says so.

Preferences > Host > Keep the CPU ready to wake up while VMs run turns both
off, at once for the VMs running too. It is on by default, on laptops and
desktops alike. On a desktop with a Ryzen 7 7800X3D (an Arc Pro B50, 4K at
240 Hz) the research measured a nested desktop's keys, from a key to the
screen (2026-11-05):

| Host's desktop | With the hold | Without | Cost at idle |
| --- | --- | --- | --- |
| GNOME 51 | 7.49 ms on average, 84 % of keys at the first refresh | 10.09 ms, 16 % (15 % only at the third), the launcher ~3.5 ms later | 3.5 W |
| KDE Plasma 6.8 | 9.20 ms | 9.37 ms (two runs of one setting were 0.55 ms apart) | 3.5 W |

A host's desktop can change at each login, so the hold does not depend on
it.

What comes back after the last VM, and what does not:

- The request is no setting the helper changes: the kernel applies the
  lowest of every program's requests, and drops the helper's when the
  helper ends, crash included. So nothing about it is recorded in
  `/run/vitrine-helper`, nothing is put back, and two helpers (vitrine
  restarted while VMs run, another user's) each hold their own. Another
  program's request (tuned's latency-performance profile holds one) stays
  its own, and the lowest one counts.
- Each CPU's preference goes back to its own value if it still reads
  `performance`. A value chosen meanwhile (a power profile switched in the
  desktop, say) is left as it is. A profile switched to `performance`
  meanwhile reads like the helper's change, and goes back too.
- A CPU offline at the first VM's start keeps its value; the next VM start
  gives it `performance` once it is online. One offline at the end keeps
  `performance` (the driver holds a CPU's value while it is offline) and
  stays recorded: the next helper puts it back once the CPU is online.

## gpu-input-awake, for dedicated GPUs

The research's small root service that wakes the GPU from its deep idle at
each key or pointer event, an optional component
([host/gpu-input-awake](../host/gpu-input-awake/README.md)): on an Intel xe
GPU it keeps the GT out of gt-c6 until 0.02 s without input, on an AMD GPU
GFXOFF off for 0.3 s. On an Arc Pro B50, keys on the bare desktop showed in
7.8 ms instead of 9.0 ms, for about 2 W while typing and nothing at rest.

Unlike host tuning it runs from boot, all the time, as root: it reads the
keyboard and pointer events (without taking them) and writes to debugfs.
Preferences > Host shows it on the GPU clock floor row of each dedicated
GPU, with Install… and Remove…, which ask for an administrator's password.
Its files are those of the research's setup, at the same paths:
`/usr/local/sbin/gpu-input-awake` and
`/etc/systemd/system/gpu-input-awake.service`; a copy that is not
vitrine's shows as such, and Install replaces it.

### Keys in VMs on an Arc Pro B50

Keys in VMs reached the screen 0.3-2 ms later on an Arc Pro B50 than on an
RX 6900 XT (the research, 2026-11-06 and 07, on a KDE Plasma 6.8 host at 4K
and 240 Hz; the table below). Not because the B50 draws slower: a VM's GPU
work took it at most 0.33 ms of a 4.17 ms refresh, with the guest's
launcher opened 8 times a second. Three things:

- The VMs' KWin held a key's frame back longer than an xe host needs (its
  lone-frame lead). Guest tools 0.1.0-38 shorten it on xe hosts: to 1000
  µs instead of 1500 in the nested desktop, and to 1000 µs in a KDE
  guest's own session, where it was unset. Since guest tools 0.1.0-44 the
  nested desktop's lead is 1000 µs on every host: on the 6900 XT keys came
  1.5 ms sooner with it than with 2500, vitrine's earlier default there
  (2026-11-12). Since 0.1.0-45 a KDE guest's own session has it on every
  host with a native context: on the 6900 XT in QEMU's window keys came
  1.8 ms sooner than without it.
- The GPU was asleep at each key. gpu-input-awake holds an xe GPU 20 ms
  after an input and an AMD GPU 0.3 s, so the 6900 XT was measured awake,
  with keys 0.3 s apart. Waking the B50 takes ~0.45 ms, during which a CPU
  busy-waits with interrupts off, and the key's first GPU jobs then run
  ~1.5 times slower at the 1200 MHz minimum clock: a key that finds the
  GPU asleep reaches the screen 0.5-0.85 ms later, on every VM path. While
  typing (4-7 keys a second), the 20 ms hold lets it sleep before every
  key.
- Part of the gap was the measurement: keys exactly 0.3 s apart are 72
  refreshes apart, so a run of 80 keys sweeps only about one refresh of
  phase from a random start. How many keys missed their refresh depended
  on that start, and a run's median could move by a whole refresh. The
  B50's runs below spread the keys over a refresh.

Key to screen, mean, keys 0.3 s apart:

| VM path | B50 as installed | B50, its GPU kept awake | RX 6900 XT |
| --- | --- | --- | --- |
| The nested desktop (a KDE guest); lone-frame lead 1000 µs on the B50, 1500 on the 6900 XT | 8.39 ms | 7.72 ms | 8.11 ms |
| A GNOME guest in QEMU's window, copied (no lead: GNOME) | 9.68 ms | 8.90 ms | 8.41 ms |
| A GNOME guest in QEMU's window, zero copy | 9.07 ms | 8.52 ms | 8.19 ms |
| A KDE guest in QEMU's window, copied; its session's lead 1000 µs on the B50, unset on the 6900 XT | 10.29 ms | 9.04 ms | 8.33 ms |

The B50's: two or three runs of 80 keys each (2026-11-07), the keys
spread over a refresh; the 6900 XT's: its runs of 2026-11-06, its GPU
awake through them. With its GPU awake, the B50 is 0.3-0.7 ms behind in
QEMU's window and 0.4 ms ahead on the nested desktop.

`--vm-hold 0.3` holds the GPU 0.3 s after an input while a VM runs, the
bare desktop keeping its 20 ms. While typing, keys came ~0.5 ms sooner on
the nested desktop and in a GNOME guest, for ~5 W more while typing and
nothing at rest; a KDE guest in QEMU's window answers ~1.25 ms sooner with
its GPU awake. It is not set by default:
[host/gpu-input-awake](../host/gpu-input-awake/README.md) says how to try
it.

## KWin and the windows the CPU draws

Not host tuning but a setting of the host's KWin, Fedora's or vitrine's,
made at each login. KWin hands the windows the CPU draws (`wl_shm`:
Konsole, Qt Widgets and GTK apps) to the GPU as udmabufs, which the GPU
then reads from system memory at each frame. On a GPU with memory of its
own that costs: on an RX 6900 XT a Konsole drag rendered in 3.2 ms instead
of 0.4 ms, a maximize or restore in 3.9-4.1 ms instead of 0.5, with 6.3-7.5
% of its frames a refresh late instead of 0.2-0.5 % (the research,
2026-10-28); on an Arc Pro B50, Konsole drags missed 47 % of the refreshes.
KWin skips the import by itself on NVIDIA and i915 only (vitrine's host KWin
on xe too).

vitrine installs a systemd user environment generator,
`/usr/lib/systemd/user-environment-generators/60-vitrine-kwin`, which sets
`KWIN_DISABLE_UDMABUF_IMPORT=1` at each login when the GPU KWin composites
on is:

- an AMD GPU with 4 GiB or more of its own memory (`mem_info_vram_total`):
  a dedicated one, or an APU given that much by its firmware;
- an Intel GPU on the xe driver (an Arc B-series card, say).

KWin then uploads those windows into textures instead; other GPUs keep
KWin's default. KWin's GPU is the first of `KWIN_DRM_DEVICES` if that is
set, else the one the computer started on (`boot_vga`). It takes effect
from the next login, and a change of GPU from the login after it. A value
of your own wins: `KWIN_DISABLE_UDMABUF_IMPORT=0` in a file of
`~/.config/environment.d` keeps the import.
`systemctl --user show-environment | grep KWIN_DISABLE_UDMABUF_IMPORT`
shows what the session has; other desktops ignore it.

The nested desktop's KWin in a guest imports the guest's windows the same
way, and this computer's GPU then reads them from the guest's memory at
each frame. On an RX 6900 XT that made KWrite's keys 0.8 ms slower in the
nested desktop, and nothing quicker (the research, 2026-11-12). So when
the VM's card renders on an AMD GPU with 4 GiB or more of its own memory,
vitrine leaves out the boot word that turns the import on
(`crosvm_desktop_guest_dmabuf=1`), and the VM's log says so; the card
keeps its `x-guest-dmabuf`. On an Intel xe host the guest's session turns
the import off by itself. `KWIN_DISABLE_UDMABUF_IMPORT=0` in
`CROSVM_DESKTOP_KWIN_ENV` of the guest's
`/etc/vitrine-guest-tools/nested.conf` keeps it.

## KWin's lone-frame lead

The same generator gives this computer's KWin `KWIN_LONE_FRAME_LEAD_US=1500`
at each login, unless the session has a value of its own. With it,
vitrine's host KWin (the research's patch 0074) renders the first frame
after an idle stretch (a key, a drag's first step, an animation's first
frame) at once, and aims it at the next vblank when that is at least 1.5
ms away, instead of when its render estimate plans it. Fedora's KWin
ignores the variable. On an RX 6900 XT at 240 Hz, KWrite's keys showed 2.7
ms sooner (10.0 ms instead of 12.7), the launcher's first frames 3 ms
sooner, and the nested desktop in a window answered keys in 8.6 ms instead
of 10.8, while KDE's animations stayed as long and as smooth (the
research, 2026-11-12). It has not been measured on other GPUs, nor at 60
or 120 Hz.

`KWIN_LONE_FRAME_LEAD_US=` (empty) in a file of `~/.config/environment.d`
turns it off from the next login, and
`echo off > $XDG_RUNTIME_DIR/kwin-lone-frame` at once.

## The udmabuf limits without host tuning

To have the udmabuf limits raised at every boot instead, with or without
host tuning, any of these does it (as root; the values are those host
tuning sets, and host tuning then finds them high enough and changes
nothing):

- The kernel's command line, which works whether udmabuf is built in (as
  in Fedora's kernel) or a module. On Fedora:

  ```
  sudo grubby --update-kernel=ALL --args='udmabuf.list_limit=65536 udmabuf.size_limit_mb=2048'
  ```

  then restart. Elsewhere, add the two to the kernel's command line the
  way the distribution does (`GRUB_CMDLINE_LINUX` in `/etc/default/grub`,
  say). The same command with `--remove-args` instead of `--args` takes
  them back.
- A file `/etc/tmpfiles.d/udmabuf.conf`, which systemd applies at each
  boot:

  ```
  w /sys/module/udmabuf/parameters/list_limit - - - - 65536
  w /sys/module/udmabuf/parameters/size_limit_mb - - - - 2048
  ```

  and now: `sudo systemd-tmpfiles --create /etc/tmpfiles.d/udmabuf.conf`.
  It needs the parameters there at boot: udmabuf built in, or a module
  loaded by then.
- Until the next boot only:
  `echo 65536 | sudo tee /sys/module/udmabuf/parameters/list_limit` and
  `echo 2048 | sudo tee /sys/module/udmabuf/parameters/size_limit_mb`.

Run `systemd-tmpfiles --create` or the two `echo` commands while no
native-context VM runs with host tuning on: host tuning would otherwise put
the old values back when that VM stops, until the next boot (it cannot tell
a write of the same values from its own).

`modprobe.d` options do nothing where udmabuf is built into the kernel, as
in Fedora's.

At each start of a VM whose GPU has native context, or of a nested-desktop
VM, vitrine reads the
limits and opens `/dev/udmabuf` as QEMU does. When they are too low (under
16384 entries or 128 MB) and host tuning will not raise them, or the
device does not open, it writes a `vitrine:` note in the VM's `qemu.log`,
and the status bar says so until the VM stops or host tuning raises them
(with host tuning on, the first time in a run of vitrine, the explanation
opens by itself). Limits high enough at the start only because another
VM's host tuning holds them are read again while the VM runs.
While a VM runs, vitrine also reads its `qemu.log` for buffers QEMU refused
(`ctrl 0x10c, error 0x1201`) and then shows "Guest windows copied", with
the count in its tooltip. It blames the limits only for a
`UDMABUF_CREATE_LIST: Invalid argument` with more entries or bytes than
they allow; for other refusals, `qemu.log` says why.

## Installing

`cmake --install` puts the helper in `<prefix>/libexec/vitrine-helper`,
its polkit actions in `<prefix>/share/polkit-1/actions` and the rule for
the group in `<prefix>/share/polkit-1/rules.d`. polkit reads only
`/usr/share/polkit-1` (and `/etc/polkit-1/rules.d`), so configure with
that prefix, or set `VITRINE_POLKIT_ACTIONS_DIR` and
`VITRINE_POLKIT_RULES_DIR` to those folders:

```
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

The actions and the app name the helper by its installed path, fixed when
configuring: `cmake --install --prefix` with another prefix is refused.

## When something looks wrong

- `journalctl -t vitrine-helper` lists what the helper changed and put back.
- `ps -L -o tid,cls,rtprio,ni,comm -p PID` lists a QEMU's threads: those of
  the VM in front show `FF 1`, those of a VM behind `TS`, its vCPUs at nice
  -5. A VM in front with more processors than half of the host's online
  CPUs shows `TS` too, as one behind; the journal says so once ("not made
  real-time: 16 vCPUs, more than half of the 16 online cpus").
- `ps -L -o tid,cls,rtprio,comm -p $(pgrep -x vitrine-gtk)` lists the GTK
  app's threads: with host tuning on, those of the view of the VM in front
  show `FF 1` (SCHED_FIFO 1), the others `TS`. The view also writes their
  ids to its output as they change ("vitrine: display: threads: ...").
  A helper installed before the app could raise them answers
  `unknown request`: the app then says once to install it again, and tunes
  the VMs as before.
- Preferences > Tune the host while VMs run says whether tuning is
  active, and the status bar's "Host tuning inactive" why the running VMs
  are not tuned.
- "polkit wants a password here" for a member of the group: the rule
  applies only in a local, active desktop session (not over ssh or
  waypipe, not from a session switched away from), and only once
  `49-vitrine.rules` is where polkit reads rules (see Installing).
- Its state is in `/run/vitrine-helper` (root only; gone at the next boot,
  like the values it records). A helper that died while holding settings
  leaves its state there: the next helper puts them back when it starts -
  the threads it made real-time first, then the fair server they needed.
  A CPU it could not put back (offline, say) stays recorded for the next
  one. To do it now: `sudo /usr/libexec/vitrine-helper session < /dev/null`.
- CPUs whose fair server is at 10 ms / 1 ms while the others are not, with
  no record, are taken for a run cut short: the next VM's helper records
  them with the others' value and puts them back after. All of them at
  10 ms / 1 ms with no record are left as found (another tool may hold them
  for a VM of its own).
- Hosts with Secure Boot run the kernel in lockdown, where debugfs cannot be
  written: no fair-server change there, and so no real-time QEMU threads;
  the rest still applies. The same without a fair server (Linux before
  6.12) or under a sched_ext scheduler on a kernel without an ext server
  (before 7.0). Preferences says so ("Real-time QEMU threads are off:
  ..."), and the status bar at the first VM start.
- GPUs other than AMD APUs get no automatic floor. A discrete AMD GPU needs
  the overdrive bit of `amdgpu.ppfeaturemask` for any floor; it gets the
  `3D_FULL_SCREEN` power profile instead, and gpu-input-awake helps there
  too (above). `grep -H '\*' /sys/class/drm/card*/device/pp_power_profile_mode`
  shows each AMD GPU's profile in use (marked `*`). Older AMD GPUs (Polaris,
  Vega) take a profile in performance level `manual` only: the helper
  leaves them as they are, after one refused write that the journal shows.
- `cat /sys/kernel/mm/transparent_hugepage/shmem_enabled` shows the huge
  pages' setting, in brackets; `grep ShmemPmdMapped /proc/PID/smaps_rollup`
  how much of a QEMU's memory has them.
- `grep . /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference`
  shows the CPUs' preference. While VMs run with the CPU kept ready, the
  deeper idle states' counts stay still: `grep .
  /sys/devices/system/cpu/cpu0/cpuidle/state*/usage`, twice.
- `cat /sys/module/udmabuf/parameters/list_limit /sys/module/udmabuf/parameters/size_limit_mb`
  shows the udmabuf limits. Where udmabuf is a module that is not loaded
  yet, the helper cannot raise them (it does not load modules).
