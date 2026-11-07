# The nested desktop's session

The guest side of vitrine's nested desktop: the guest's Plasma as one window
of the host's desktop, through its KWin's Wayland backend on
wl-cross-domain-proxy (guest/proxy) over virtio-gpu cross-domain.

These files are the research's crosvm-desktop session
(`~/Documents/virtio-gpu-desktop`, `src/crosvm-desktop/host-package/guest/files`
at commit 5eee06a: the proxy's statistics in the journal only, guest tools
0.1.0-36; before it 97e4f0d: no input method for the nested KWin, 0.1.0-33).
`UPSTREAM.sha256` has their hashes there: `sha256sum -c UPSTREAM.sha256` fails
only for the files changed below. The research's commits between b973b6b (the
version taken first) and 97e4f0d took vitrine's changes to `session` below in
their own words (the session's end when the host does not answer, the VT left
to the kernel); vitrine keeps its own, so 97e4f0d's input method change and
5eee06a's journal mode are the only ones taken from them.

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
| `nested.conf` | header comment; no `CROSVM_DESKTOP_AUTOLOGIN` (a note instead); `CROSVM_DESKTOP_STATS=journal` (their default: title); a note on the `CROSVM_DESKTOP_KWIN_ENV` settings vitrine's KWin knows; `vitrine-nested-boot.service` for their detect service | vitrine sets no automatic login (the user's decision, 2026-10-14): the guest's own, if any, logs into the nested desktop; no numbers in the window's title, but the summary in the guest's journal every minute with frames (`journalctl --user -t crosvm-desktop-proxy`: "N fps presented, M dropped"). Up to guest tools 0.1.0-35 the stats were off, and nothing showed, for weeks, that a GNOME host discarded every frame's feedback (2026-11-05) |
| `session` | KWin's settings for the host's GPU: on an Intel xe host `KWIN_DISABLE_UDMABUF_IMPORT=1`, `KWIN_WAYLAND_SCANOUT_BUFFERS=1` and `KWIN_LONE_FRAME_LEAD_US=1000` in place of 2500 or of 1500 (the xe value up to guest tools 0.1.0-37, which the journal then says it replaced), unless `CROSVM_DESKTOP_KWIN_ENV` names them with values of its own; the GPU from `/run/vitrine-guest-tools/host-gpu` (`vitrine-virtio-gpu-check.service`, which asks the native context's capset at boot), or `CROSVM_DESKTOP_HOST_GPU`; the journal says what it found and set, and the KWin settings the session ends with | the research sets these when it builds a guest for an Intel host (its `customize.sh`, `GUEST_MESA_INTEL=1`, 2026-10-23/24; the lead 1000 since its e152b8a, 2026-11-06: "KWin's settings on Intel xe hosts" below); vitrine's guest tools find the host's GPU at run time: the capset's context type is the GPU the guest renders on (the host's virglrenderer takes amdgpu before xe on a host with both) |
| `nested.conf` | `CROSVM_DESKTOP_HOST_GPU` (auto, xe, another name); the notes on `KWIN_LONE_FRAME_LEAD_US` (vitrine's KWin knows it since 6.7.5-1.28.1; 1000 on an xe host) and on the session's xe settings | the session's host GPU settings above; a way to name the host's GPU when the capset cannot be read, or to keep the xe settings off |
| `session` | the session's VT in VT_AUTO and KD_TEXT at the start, its cursor hidden (VT_SETMODE, KDSETMODE and DECTCEM off on the controlling terminal, through python3) | the login manager hands the VT over in VT_PROCESS mode, the session's process to release it at a switch (plasmalogin's and SDDM's VirtualTerminal: SIGRTMAX); the nested KWin takes no VT from logind, so nothing answered and the signal ended the session at any switch to another VT: Ctrl+Alt+F3 from vitrine's Send Key menu, or chvt (2026-10-30). The kernel switches by itself only from a VT in text mode; there the framebuffer console blinked the cursor 5 times a second once the VT had been switched away from and back |

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

## KWin's settings on Intel xe hosts: the nested desktop and the guest's own session

The host's GPU is the one the virtio-gpu native context reports, the GPU
the guest's Mesa renders on. `vitrine-virtio-gpu-check.service` asks at
each boot and writes the answer to `/run/vitrine-guest-tools/host-gpu` and
to its journal (`journalctl -b -u vitrine-virtio-gpu-check`).

Guest tools 0.1.0-38 shorten KWin's lone-frame lead on Intel xe hosts: to
1000 µs instead of 1500 in the nested desktop, and to 1000 µs in a KDE
guest's own session, where it was unset (the research's e152b8a and
ef7974d). On its Arc Pro B50 under KDE (2026-11-06/07):

- the nested desktop: keys 7.8-10.2 -> 7.4-7.6 ms p50 (bare KDE 7.6-7.7),
  the launcher's start still without a repeated refresh;
- the guest's own session in QEMU's window: keys 11.01 -> 10.29 ms mean,
  the guest's part 3.48 -> 2.44 ms, animations unchanged. Without the lead
  its KWin held a key's frame up to ~2 ms to meet its own vblank. vitrine's
  view was not measured.

Other hosts keep what they had: 2500 in the nested desktop, no lead in the
guest's own session.

The nested desktop: the session adds the xe settings to
`CROSVM_DESKTOP_KWIN_ENV` at login (the `session` row above;
`CROSVM_DESKTOP_HOST_GPU` in nested.conf can name the GPU instead). A lead
of 1500 there counts as vitrine's earlier value and is replaced, which the
session's journal says; any other value is yours and stays.

The guest's own session: on an Intel xe host the check writes
`/run/environment.d/45-vitrine-lone-frame.conf`
(`KWIN_LONE_FRAME_LEAD_US=1000`) before any user's systemd starts: it runs
before `systemd-user-sessions.service`, which every `user@.service`
follows, for an automatic login, a lingering user and ssh alike. Each
user's systemd reads environment.d as it starts and passes it on to KWin.
The file follows the native context only, not `CROSVM_DESKTOP_HOST_GPU`,
and is written again at each boot: a VM moved to another host keeps no
stale setting. The check gives up after a minute (`TimeoutStartSec=60`), as
a hung one would keep every login but root's out; stopped, it leaves each
of its files whole or absent, and the boot goes on without the settings
for the host's GPU.

environment.d sorts the files of `~/.config/environment.d`,
`/etc/environment.d`, `/run/environment.d` and `/usr/lib/environment.d`
together by name, and for the same variable the file whose name sorts last
wins:

- a file of yours whose name sorts after `45-vitrine-lone-frame.conf`
  (`70-kwin.conf`, say) overrides vitrine's value; one that sorts before it
  does not;
- a file of the same name in `/etc/environment.d` (or in
  `~/.config/environment.d`, for one user) replaces the `/run` one; empty,
  it turns the lead off.

vitrine's other KWin settings for the guest's sessions are in
`/usr/lib/environment.d/60-vitrine-kwin.conf`. The nested desktop's KWin
inherits these files too, but its `CROSVM_DESKTOP_KWIN_ENV` comes on top
(`CROSVM_DESKTOP_KWIN_UNSET` drops an inherited setting).
