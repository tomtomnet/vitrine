# The nested desktop's session

The guest side of vitrine's nested desktop: the guest's Plasma as one window
of the host's desktop, through its KWin's Wayland backend on
wl-cross-domain-proxy (guest/proxy) over virtio-gpu cross-domain.

These files are the research's crosvm-desktop session
(`~/Documents/virtio-gpu-desktop`, `src/crosvm-desktop/host-package/guest/files`
at commit b973b6b). `UPSTREAM.sha256` has their hashes there:
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
| `session` | the session ends when the host's display gives no answer to wayland-info: its 5 s run out (status 124) or it lists not one global (the host hung up at once); wayland-info's output is taken once, for this and for `host_screens` | vitrine not running (a login, automatic or not, while vitrine is closed): QEMU's cross-domain finds no socket. With rutabaga_gfx before the research's 8ecffaf the proxy never connected again, so KWin waited for its host forever and the session hung without a window or a login screen (2026-10-15); since 8ecffaf the guest's channel hangs up at once, and KWin, failing to connect, was restarted ten times before the session ended, with some 25 crashing clients. The guest goes back to its login screen instead, at once |
| `nested.conf` | header comment; no `CROSVM_DESKTOP_AUTOLOGIN` (a note instead); `CROSVM_DESKTOP_STATS=off`; a note on the `CROSVM_DESKTOP_KWIN_ENV` settings vitrine's KWin 6.7.5 knows; `vitrine-nested-boot.service` for their detect service | vitrine sets no automatic login (the user's decision, 2026-10-14): the guest's own, if any, logs into the nested desktop; no numbers in the window's title, as the research's own guests |
| `session` | KWin's settings for the host's GPU: on an Intel xe host `KWIN_DISABLE_UDMABUF_IMPORT=1`, `KWIN_WAYLAND_SCANOUT_BUFFERS=1` and `KWIN_LONE_FRAME_LEAD_US=1500` in place of 2500, unless `CROSVM_DESKTOP_KWIN_ENV` names them with values of its own; the GPU from `/run/vitrine-guest-tools/host-gpu` (`vitrine-virtio-gpu-check.service`, which asks the native context's capset at boot), or `CROSVM_DESKTOP_HOST_GPU`; the journal says what it found and set, and the KWin settings the session ends with | the research sets these when it builds a guest for an Intel host (its `customize.sh`, `GUEST_MESA_INTEL=1`, 2026-10-23/24); vitrine's guest tools find the host's GPU at run time: the capset's context type is the GPU the guest renders on (the host's virglrenderer takes amdgpu before xe on a host with both) |
| `nested.conf` | `CROSVM_DESKTOP_HOST_GPU` (auto, xe, another name); the notes on `KWIN_LONE_FRAME_LEAD_US` (vitrine's KWin knows it since 6.7.5-1.28.1; 1500 on an xe host) and on the session's xe settings | the session's host GPU settings above; a way to name the host's GPU when the capset cannot be read, or to keep the xe settings off |

## Licences

The research's session files, and vitrine's changes to them, are
GPL-2.0-or-later (the research's licence since b973b6b, the user's choice;
each file's SPDX line). The proxy (guest/proxy) is Victoria Brekenfeld's
wl-cross-domain-proxy, MIT, with the research's changes, GPL-2.0-or-later:
the tree is GPL-2.0-or-later AND MIT. The research's patches to KWin,
KPipeWire, Mesa and the virtio-gpu driver keep those projects' licences.

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
- shared-memory huge pages (`CROSVM_DESKTOP_SHMEM_HUGEPAGES`);
- the login screen on the VM's own screen only: plasmalogin's KWin gets
  `KWIN_DRM_DEVICES` with the virtio card (`/run/vitrine-nested/greeter-kwin.env`,
  read by the drop-in `plasma-login-kwin_wayland.service.d/vitrine-nested.conf`).
  It drew on QEMU's VGA too (bochs-drm) and put its login form there, on the
  Boot view, while vitrine showed the Screen view.

`vitrine-virtio-gpu-check.service` then sets the driver's options of such a
boot: `blob_flush_fence=0` and `hide_virgl=1`. spice-vdagent and vitrine's
clipboard bridge do not start in it (drop-ins in
`/usr/lib/systemd/user/*.service.d/vitrine-nested.conf`): the proxy shares
the clipboard with the host. Any other boot is the main line, unchanged.
