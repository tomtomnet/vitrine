# gpu-input-awake

An optional component: the research's small root service that wakes the GPU
from its deep idle at each key or pointer event, so that the frame answering
it does not wait for the GPU to power up. It is for dedicated GPUs, where a
clock floor does nothing (measured); AMD APUs get the clock floor of host
tuning instead (docs/host-tuning.md).

- Intel xe GPUs: the GT kept out of gt-c6 (debugfs `forcewake_all` held
  open) from an input event until 0.02 s without input; also while the GT
  draws under CPU load or with a VM running, and for 20 ms every 2 s after
  a load, so that the card's temperature reading, which drives its fan,
  does not freeze. On an Arc Pro B50: keys on the bare desktop 9.0 -> 7.8 ms
  (p50), about 2 W while typing without a pause, nothing at rest; a 60 fps
  video in a VM on an idle host on cadence 100 % of the time instead of
  28-48 %.
- AMD GPUs: GFXOFF held off (debugfs `amdgpu_gfxoff`) for 0.3 s after each
  input event. On an RX 6900 XT: keys 8.6-8.9 -> 8.1 ms (p50), nothing at
  rest. Measured on that card only.
- No such GPU (in a VM, say): it waits up to 60 s for the driver's files,
  then exits without doing anything.

It runs as root, from boot, all the time: it reads the input devices
(`/dev/input/event*`, without grabbing them: the keys go where they went)
and writes to debugfs. With several GPUs it takes the first xe GPU, else
the first AMD GPU in debugfs order.

## Installing and removing

Preferences > Host shows it on the GPU clock floor row of each dedicated
GPU, with Install… and Remove…; both ask for the administrator's password
(`pkexec bash install.sh install|remove`). The files go where the research's
setup puts them, so a host has one copy, whoever installed it:

- `/usr/local/sbin/gpu-input-awake`, the script;
- `/etc/systemd/system/gpu-input-awake.service`, its unit, enabled.

A copy that is not this folder's (the research's own install, another
version) shows as such; Install replaces it, Remove removes it. By hand, as
root:

```sh
install -D -m 0755 gpu-input-awake /usr/local/sbin/gpu-input-awake
install -D -m 0644 gpu-input-awake.service /etc/systemd/system/gpu-input-awake.service
systemctl daemon-reload && systemctl enable --now gpu-input-awake.service
```

and to remove it:

```sh
systemctl disable --now gpu-input-awake.service
rm /usr/local/sbin/gpu-input-awake /etc/systemd/system/gpu-input-awake.service
systemctl daemon-reload
```

`journalctl -u gpu-input-awake` shows what it did.

## The pinned copy

`gpu-input-awake` and `gpu-input-awake.service` are the research's
(virtio-gpu-desktop) `bench/tools/gpu_input_awake.py` and
`src/host-config/gpu-input-awake.service` at commit `6c4e5ed` (2026-10-28,
the same at `8289f62`), byte for byte, with their modes. GPL-2.0-or-later,
as the research's README says of its files without a notice of their own.

| sha256 | file |
|---|---|
| `6286c5d8ec3bf91ce6164b34d053a4fc5b960001b9973cd4eb64fd8fab4cb47d` | `gpu-input-awake` |
| `85e100e8a370d6ae92879f92a9697fc99f3f504a409cd6ff3504f7f6a91dcd66` | `gpu-input-awake.service` |

The app tells Vitrine's copy from another by these. When the research's
moves, take it again (`r` its repository, `c` the commit) and update the
table:

```sh
r=~/Documents/virtio-gpu-desktop c=6c4e5ed
git -C "$r" show "$c:bench/tools/gpu_input_awake.py" > gpu-input-awake
git -C "$r" show "$c:src/host-config/gpu-input-awake.service" > gpu-input-awake.service
chmod 0755 gpu-input-awake && chmod 0644 gpu-input-awake.service
```
