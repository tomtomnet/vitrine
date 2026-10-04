# Host tuning while VMs run

A desktop VM stays smooth under host load when the host gives it a hand:

- **Real-time QEMU threads** (SCHED_FIFO priority 1): the guest's vCPUs and
  QEMU's display and I/O threads run before ordinary host tasks.
- **A shorter fair-server period**: the kernel keeps some CPU time for
  ordinary tasks while real-time ones run, 50 ms per second by default. An
  ordinary task stuck behind a real-time vCPU (a kernel worker pushing GPU
  jobs, say) could then wait up to 950 ms; at 1 ms every 10 ms it waits
  under ~9 ms.
- **A GPU clock floor** (AMD APUs): at moderate load the GPU stays at its
  lowest clock, where copying a 4K frame takes about 1 ms. With the lowest
  gfx clock at 1800 MHz a Radeon 780M showed new images at 94-95 % of the
  refreshes during window animations instead of 87-90 %, for about 0.3 W.

These need root. vitrine applies them through a small helper,
`vitrine-helper`, when a VM starts, and the helper puts everything back
after the last VM stops - crash included, and even if vitrine has quit
while the VMs run on. There is no service: the helper runs only while VMs
run. Nothing is applied while no VM runs.

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
| `fair-server on` | 10 ms / 1 ms on every CPU | Fixed values; only `cpuN` folders; nothing to choose |
| `gpu-floor CARD MHZ\|auto` | An AMD GPU's lowest gfx clock | `cardN` of vendor 0x1002 driven by amdgpu; the clock within the GPU's own overdrive range; only when its performance level is `auto` |
| `rt PID` | SCHED_FIFO 1 on every thread of a watched QEMU | Watched first, and checked again to be the caller's QEMU (it may have run another program since); real-time threads it finds are left as they are |
| `setcap PATH` | `cap_sys_nice=ep` on vitrine's QEMU build | See below |
| `setup-group` | The caller in the `vitrine` group, the group created (`groupadd --system`) if there is none | See below |

The settings need a watched QEMU, and each value is put back only if it
still holds what the helper wrote: another tool that changed it since keeps
its change.

What this amounts to:

- Real-time priority 1 for your own processes. The `qemu-system-*` name is
  no proof of anything, and a guest's own code runs on those vCPUs: think of
  the group as `rtprio 1` in `limits.conf`. The shorter fair-server period
  keeps ordinary tasks running beside them.
- Two host-wide settings changed while your VMs run, and put back after.
- `cap_sys_nice` on a QEMU you built: QEMU may then make its vCPUs real-time
  (vitrine's focus priority) and ask amdgpu for high-priority GPU contexts.
  vitrine builds that QEMU from sources in your home folder, so the content
  is yours: the capability amounts to CAP_SYS_NICE for a program of yours,
  which can change the priority and CPU affinity of other users' processes.
  It cannot read or change their data. The helper makes sure the file stays
  yours alone: an absolute path in your home folder of the shape
  `.../vitrine/stack/<build>/bin/qemu-system-<arch>`, walked without
  following links, every folder from your home down writable only by you
  (or root), a regular file of yours with one link and no setuid bit, an
  ELF executable for this machine, on a file system that honours file
  capabilities. It sets the file's mode to 0700 first, so no one else can
  run it. Any write to the file drops the capability (the kernel does
  that): each build needs it again, and vitrine asks the helper after each
  build, and when it starts or tuning is turned on again if its QEMU lacks
  it (built before you joined the group, say). A VM gets it at its next
  start.

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
run), `org.vitrine.helper.setcap` and `org.vitrine.helper.setup-group`.
The group's rule grants the first two only: setup-group always wants an
administrator's password, from members too, and its dialog says it adds
you to the vitrine group. A rule of your own that gives the helper to more
people, or lets them use their own password, does not bring a lasting
change of `/etc/group` along unless it names setup-group's action.
(pkexec takes the first action whose path matches and whose first
argument, if the action names one, matches too, in no fixed order: each
action names its argument, so exactly one matches, and the helper refuses
to run without one, which pkexec would put under its generic action.)

Give the group to the people you would give real-time priority to.

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
- Its state is in `/run/vitrine-helper` (root only; gone at the next boot).
  A helper that died while holding settings leaves its state there: the next
  helper puts them back when it starts. To do it now:
  `sudo /usr/libexec/vitrine-helper session < /dev/null`.
- Hosts with Secure Boot run the kernel in lockdown, where debugfs cannot be
  written: no fair-server change there, the rest still applies.
- GPUs other than AMD APUs get no automatic floor. A discrete AMD GPU needs
  the overdrive bit of `amdgpu.ppfeaturemask` for any floor.
