# Host tuning while VMs run

A desktop VM stays smooth under host load when the host gives it a hand:

- **Real-time QEMU threads** (SCHED_FIFO priority 1) for the VM in front -
  its console or full-screen window has the keyboard: the guest's vCPUs and
  QEMU's display and I/O threads run before ordinary host tasks. The other
  VMs' threads stay ordinary, their vCPUs at nice -5, so that VMs busy in
  the background cannot take every CPU at real-time priority: one VM is
  real-time at a time. A host window in front changes nothing; a VM alone
  is real-time, and so is the first one started or found running until
  another comes to the front. A VM in QEMU's own window (SDL, GTK) is in
  front when that window is the active one: on KDE Plasma a small KWin
  script tells vitrine which window is active, while such VMs run. On
  other desktops vitrine cannot tell: such a VM goes behind as soon as
  another VM comes to the front, and comes back only when it is alone.
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
  idle. Preferences > Host turns it off ([below](#the-cpu-ready-to-wake-up)).
- **Higher udmabuf limits** (VMs whose GPU has native context): QEMU
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
the helper sets its threads' scheduling.

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
| `rt PID` | SCHED_FIFO 1 on every thread of a watched QEMU (the VM in front), its real-time time limit (`RLIMIT_RTTIME`) lifted first | Only once `fair-server on` holds; watched first, and checked again to be the caller's QEMU (it may have run another program since); no thread made real-time if the limit cannot be lifted; real-time threads it finds are left as they are; its child processes (passt) are not touched |
| `behind PID` | The threads `rt` made real-time back to SCHED_OTHER, its vCPU threads (`CPU n/KVM`) at nice -5 (a VM behind the one in front) | The same checks as `rt`; nice -5 only while the fair server holds, and only for vCPUs at nice 0 |
| `udmabuf PID` | The udmabuf module's `list_limit` at 65536 and `size_limit_mb` at 2048, while that watched QEMU runs | A watched QEMU (its GPU is not checked: vitrine asks only for native-context ones); fixed values; raised only, a higher value stays; skipped when the module is not loaded; put back after the last QEMU that asked for it |
| `thp on` | `/sys/kernel/mm/transparent_hugepage/shmem_enabled` at `advise` | A watched QEMU; a fixed value, written only over `never`; read back |
| `cpu-latency on` | A request on `/dev/cpu_dma_latency` for the wake-up time of the CPUs' first usable idle state (C1), held by the helper's open file | A watched QEMU; the value from cpuidle, at least 1 µs; nothing recorded: the kernel drops the request with the file |
| `epp on` | Each online CPU's `cpufreq/energy_performance_preference` at `performance` | A watched QEMU; a fixed value; read back; all or nothing |
| `setup-group` | The caller in the `vitrine` group, the group created (`groupadd --system`) if there is none | See below |

The settings need a watched QEMU, and each value is put back only if it
still holds what the helper wrote: another tool that changed it since keeps
its change. The CPU latency request is the helper's own: no one else can
change it, and it ends with the helper.

What this amounts to:

- Real-time priority 1 for your own processes, and nice -5 for their
  threads named as vCPUs. The `qemu-system-*` name is no proof of
  anything, and a guest's own code runs on those vCPUs: think of the group
  as `rtprio 1` in `limits.conf`. The shorter fair-server period keeps
  ordinary tasks running beside them, and without it there is no
  real-time priority.
- Host-wide settings changed while your VMs run, and put back after: the
  fair server, a GPU clock floor, shared memory's huge pages and the CPUs'
  energy performance preference; and a request that keeps the CPUs out of
  their deeper idle states while the helper runs.
- Bigger udmabufs. `/dev/udmabuf` is open to the user at the desktop
  already (systemd's `uaccess` rule), and to the `kvm` group: they can turn
  their own memory into a udmabuf. The limits only set how many pieces and bytes
  one udmabuf may have, and it is memory its owner has anyway. While they
  are raised, they are raised for every user of the host.
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
run) and `org.vitrine.helper.setup-group`. The group's rule grants the
first only: setup-group always wants an administrator's password, from
members too, and its dialog says it adds you to the vitrine group. A rule of your own that gives the helper to more
people, or lets them use their own password, does not bring a lasting
change of `/etc/group` along unless it names setup-group's action.
(pkexec takes the first action whose path matches and whose first
argument, if the action names one, matches too, in no fixed order: each
action names its argument, so exactly one matches, and the helper refuses
to run without one, which pkexec would put under its generic action.)

Give the group to the people you would give real-time priority to.

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
off, at once for the VMs running too. It is on by default.

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

At each start of a VM whose GPU has native context, vitrine reads the
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
  the overdrive bit of `amdgpu.ppfeaturemask` for any floor; gpu-input-awake
  is what helps there (below).
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
