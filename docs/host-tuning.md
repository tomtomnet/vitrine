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
runs the VM without these settings and says so once in the status bar.

Members of the `vitrine` group use the helper without a password, from
their local desktop session. To allow yourself:

```
sudo groupadd --system vitrine
sudo usermod -aG vitrine "$USER"
```

polkit sees the new membership at the next VM start (`id` lists it once
you log in again). Preferences > Tune the host while VMs run turns it off
again for you.

To take it back: `sudo gpasswd -d "$USER" vitrine` (and
`sudo groupdel vitrine` when nobody is left in it).

## What the group allows

Polkit decides who may start the helper; the helper decides what it does
for them. It does only this, for the user who started it:

| Request | What it does | Checks |
| --- | --- | --- |
| `watch PID` | Watches a QEMU until it exits | The process belongs to the caller (all its uids) and runs a `qemu-system-*` (or `qemu-kvm`) executable; held through a pidfd, so a reused pid cannot slip in |
| `fair-server on` | 10 ms / 1 ms on every CPU | Fixed values; only `cpuN` folders; nothing to choose |
| `gpu-floor CARD MHZ\|auto` | An AMD GPU's lowest gfx clock | `cardN` of vendor 0x1002 driven by amdgpu; the clock within the GPU's own overdrive range; only when its performance level is `auto` |
| `rt PID` | SCHED_FIFO 1 on every thread of a watched QEMU | Watched first; RT threads it finds are left as they are |
| `setcap PATH` | `cap_sys_nice=ep` on vitrine's QEMU build | See below |

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
  build.

Give the group to the people you would give real-time priority to.

## Installing

`cmake --install` puts the helper in `<prefix>/libexec/vitrine-helper`,
the polkit action in `<prefix>/share/polkit-1/actions` and the rule for the
group in `<prefix>/share/polkit-1/rules.d`. polkit reads only
`/usr/share/polkit-1` (and `/etc/polkit-1/rules.d`), so install with
`-DCMAKE_INSTALL_PREFIX=/usr`, or set `VITRINE_POLKIT_ACTIONS_DIR` and
`VITRINE_POLKIT_RULES_DIR` to those folders. The action names the helper
by its installed path.

## When something looks wrong

- `journalctl -t vitrine-helper` lists what the helper changed and put back.
- Its state is in `/run/vitrine-helper` (root only; gone at the next boot).
  A helper that died while holding settings leaves its state there: the next
  helper puts them back when it starts. To do it now:
  `sudo /usr/libexec/vitrine-helper < /dev/null`.
- Hosts with Secure Boot run the kernel in lockdown, where debugfs cannot be
  written: no fair-server change there, the rest still applies.
- GPUs other than AMD APUs get no automatic floor. A discrete AMD GPU needs
  the overdrive bit of `amdgpu.ppfeaturemask` for any floor.
