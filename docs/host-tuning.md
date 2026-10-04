# Host tuning while VMs run

A desktop VM stays smooth under host load when the host gives it a hand:

- **Real-time QEMU threads** (SCHED_FIFO priority 1) for the VM in front -
  its console or full-screen window has the keyboard: the guest's vCPUs and
  QEMU's display and I/O threads run before ordinary host tasks. The other
  VMs' threads stay ordinary, their vCPUs at nice -5, so that VMs busy in
  the background cannot take every CPU at real-time priority. VMs in QEMU's
  own window (SDL) keep real-time threads: which window is in front is not
  known there.
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
once for the VMs running too (and on again, or another GPU clock floor,
the same way).

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
| `rt PID` | SCHED_FIFO 1 on every thread of a watched QEMU (the VM in front) | Only once `fair-server on` holds; watched first, and checked again to be the caller's QEMU (it may have run another program since); real-time threads it finds are left as they are; its child processes (passt) are not touched |
| `behind PID` | The threads `rt` made real-time back to SCHED_OTHER, its vCPU threads (`CPU n/KVM`) at nice -5 (a VM behind the one in front) | The same checks as `rt`; nice -5 only while the fair server holds, and only for vCPUs at nice 0 |
| `udmabuf PID` | The udmabuf module's `list_limit` at 65536 and `size_limit_mb` at 2048, while that watched QEMU runs | A watched QEMU (its GPU is not checked: vitrine asks only for native-context ones); fixed values; raised only, a higher value stays; skipped when the module is not loaded; put back after the last QEMU that asked for it |
| `setup-group` | The caller in the `vitrine` group, the group created (`groupadd --system`) if there is none | See below |

The settings need a watched QEMU, and each value is put back only if it
still holds what the helper wrote: another tool that changed it since keeps
its change.

What this amounts to:

- Real-time priority 1 for your own processes, and nice -5 for their
  threads named as vCPUs. The `qemu-system-*` name is no proof of
  anything, and a guest's own code runs on those vCPUs: think of the group
  as `rtprio 1` in `limits.conf`. The shorter fair-server period keeps
  ordinary tasks running beside them, and without it there is no
  real-time priority.
- Three host-wide settings changed while your VMs run, and put back after.
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
  measured.

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
  the overdrive bit of `amdgpu.ppfeaturemask` for any floor.
- `cat /sys/module/udmabuf/parameters/list_limit /sys/module/udmabuf/parameters/size_limit_mb`
  shows the udmabuf limits. Where udmabuf is a module that is not loaded
  yet, the helper cannot raise them (it does not load modules).
