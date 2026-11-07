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

## A longer hold while VMs run

`--vm-hold S` holds the GPU S seconds after an input while a VM runs (a
`qemu-system-*` or `crosvm` process), the bare desktop keeping `--hold`.
On an Arc Pro B50, `--vm-hold 0.3` brought keys in VMs ~0.5 ms sooner while
typing, for ~5 W more while typing in a VM and nothing at rest
(docs/host-tuning.md). The unit does not set it. To try it, as root, a
drop-in that repeats the unit's command line with it (Vitrine's two files
stay as installed):

```sh
mkdir -p /etc/systemd/system/gpu-input-awake.service.d
cat > /etc/systemd/system/gpu-input-awake.service.d/vm-hold.conf <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/python3 -B /usr/local/sbin/gpu-input-awake --hold 0.02 --vm-hold 0.3 --activity --vm-activity --activity-hold 10 --sample 0.05 --temp-refresh 2
EOF
systemctl daemon-reload && systemctl restart gpu-input-awake
```

It needs this version of the script: an older one stops at the unknown
option (`journalctl -u gpu-input-awake`). The drop-in's line replaces the
unit's: compare the two after Vitrine updates gpu-input-awake. To go back:

```sh
rm -r /etc/systemd/system/gpu-input-awake.service.d
systemctl daemon-reload && systemctl restart gpu-input-awake
```

Remove… leaves the drop-in, which is yours: after it, the `rm` and
`systemctl daemon-reload` are enough.

## The pinned copy

`gpu-input-awake` and `gpu-input-awake.service` are the research's
(virtio-gpu-desktop) `bench/tools/gpu_input_awake.py` and
`src/host-config/gpu-input-awake.service` at commit `4ee46dd` (2026-11-07,
the same at `fd5c043`), byte for byte, with their modes. GPL-2.0-or-later,
as the research's README says of its files without a notice of their own.

| sha256 | file |
|---|---|
| `e93c081ed9b7ab47d0a37db6fe2b7ea3ed64394f2abe02c4c2ae831316aeeae6` | `gpu-input-awake` |
| `ad36c266a625a4c5d37bd743316f1ca480dd8c7ebe81a4a3bc307d0eaa6b97da` | `gpu-input-awake.service` |

The app tells Vitrine's copy from another by these: the copy of `6c4e5ed`
(2026-10-28), installed before, shows as another version until Install…
replaces it. When the research's moves, take it again (`r` its repository,
`c` the commit) and update the table:

```sh
r=~/Documents/virtio-gpu-desktop c=4ee46dd
git -C "$r" show "$c:bench/tools/gpu_input_awake.py" > gpu-input-awake
git -C "$r" show "$c:src/host-config/gpu-input-awake.service" > gpu-input-awake.service
chmod 0755 gpu-input-awake && chmod 0644 gpu-input-awake.service
```
