# The nested desktop's session

The guest side of vitrine's nested desktop: the guest's Plasma as one window
of the host's desktop, through its KWin's Wayland backend on
wl-cross-domain-proxy (guest/proxy) over virtio-gpu cross-domain.

These files are the research's crosvm-desktop session
(`~/Documents/virtio-gpu-desktop`, `src/crosvm-desktop/host-package/guest/files`
at commit 5799347). `UPSTREAM.sha256` has their hashes there:
`sha256sum -c UPSTREAM.sha256` fails only for the files changed below.

| file | installed as |
|---|---|
| `session` | `/usr/libexec/vitrine-guest-tools/nested/session` |
| `kwin_wayland_wrapper` | `/usr/libexec/vitrine-guest-tools/nested/bin/kwin_wayland_wrapper` |
| `stats-overlay.qml` | `/usr/libexec/vitrine-guest-tools/nested/stats-overlay.qml` |
| `nested.conf` (their `session.conf`) | `/etc/vitrine-guest-tools/nested.conf` |
| `plasma.desktop` (vitrine's, from their `plasma-crosvm-window.desktop`) | `/usr/share/vitrine-guest-tools/wayland-sessions/plasma.desktop` |

Their `fwcfg` and `detect` are one script of vitrine's,
`libexec/nested-boot` (`vitrine-nested-boot.service`), without the automatic
login; their journal service (crosvm debug boots), `crosvm-desktop-check` and
`install-guest.sh` are not taken.

## Local changes

| file | change | why |
|---|---|---|
| `session` | header comment; `conf`, `launcher_words`, `LIBEXEC` and `PROXY` paths | vitrine's package layout: no files in /usr/local; the boot's words in /run/vitrine-nested |
| `nested.conf` | header comment; no `CROSVM_DESKTOP_AUTOLOGIN` (a note instead); `CROSVM_DESKTOP_STATS=off`; a note on the `CROSVM_DESKTOP_KWIN_ENV` settings vitrine's KWin 6.7.5 knows; `vitrine-nested-boot.service` for their detect service | vitrine sets no automatic login (the user's decision, 2026-10-14): the guest's own, if any, logs into the nested desktop; no numbers in the window's title, as the research's own guests |

## How a boot becomes a nested-desktop one

vitrine passes `crosvm_desktop=1` (with `crosvm_desktop_guest_dmabuf=1` and
`crosvm_desktop_label=NAME`) through QEMU's fw_cfg
(`opt/org.drm-native-context/cmdline`) when the VM's Nested desktop box is
checked. `vitrine-nested-boot.service` reads it early at boot and, in such a
boot only, sets up in /run:

- `/run/vitrine-nested/nested` and `cmdline` (the words, for the session);
- the login screen's sessions: `plasma.desktop` is found first in
  `/usr/share/vitrine-guest-tools/wayland-sessions`, which is "Plasma (nested
  desktop)". plasmalogin takes it from its `XDG_DATA_DIRS`
  (`/run/vitrine-nested/plasmalogin.env`, read by the drop-in
  `plasmalogin.service.d/vitrine-nested.conf`; the greeter gets the daemon's
  `XDG_DATA_DIRS`), SDDM from its `SessionDir`
  (`/run/vitrine-nested/sddm.conf`, which the link
  `/usr/lib/sddm/sddm.conf.d/vitrine-nested.conf` names: a dangling link in
  other boots, which SDDM skips). The guest's own automatic login into
  Plasma (`/etc/plasmalogin.conf`, which System Settings writes) then logs
  into the nested desktop;
- shared-memory huge pages (`CROSVM_DESKTOP_SHMEM_HUGEPAGES`).

`vitrine-virtio-gpu-check.service` then sets the driver's options of such a
boot: `blob_flush_fence=0` and `hide_virgl=1`. spice-vdagent and vitrine's
clipboard bridge do not start in it (drop-ins in
`/usr/lib/systemd/user/*.service.d/vitrine-nested.conf`): the proxy shares
the clipboard with the host. Any other boot is the main line, unchanged.
