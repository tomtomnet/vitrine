# nested

The nested desktop in vitrine's window: the guest's KDE session (its nested
KWin, over virtio-gpu cross-domain) shown as part of the window, beside the
VM's own screens.

`vmview.{h,cpp}`, `protocols.{h,txt}`, `gen_protocols.py` and `LICENSE` are
the research side's vmview library (`src/vmview` of the virtio-gpu-desktop
repository, commit 182e74d, which took vitrine's two changes of 776a6f2),
under the MIT licence of `LICENSE`. QEMU's cross-domain connects to a socket
the library serves; the guest's Wayland protocol goes on to the host
compositor over vitrine's own connection, and the guest's window becomes a
subsurface of a surface of vitrine's. The library is not stable yet: keep its
files as they are, so that its changes port over by copying them. `sha256sum
-c UPSTREAM.sha256` lists the files that no longer match the version taken;
the changes vitrine needs before the research side has them are under Local
changes below, to make again after a copy. Not taken: the library's
CMakeLists.txt (app/CMakeLists.txt builds it the same way: a static library,
its protocol code generated with wayland-scanner and `gen_protocols.py` from
wayland-protocols), its demo, tools and README.

vitrine's own code is `nestedview.{h,cpp}`: the Qt side of it (a surface of
vitrine's that the guest's window hangs from, in the console or full
screen, and the library's callbacks brought to the GUI thread).

## Local changes

The research side took vitrine's first two (a `pressed` callback for button
presses on the guest's window; a NULL object where the protocol allows none
counts as the app's) into its 182e74d. Since:

| file | change | why |
|---|---|---|
| `vmview.cpp` | `View::stopAll()` sends the guest's toplevel `xdg_toplevel.close` (flushed, up to ~50 ms) before it closes the connections | the app's end of the socket closing tells the guest nothing: QEMU's cross-domain (rutabaga_gfx 0.1.85) turns its end of file into empty receives, which the proxy ignores, and the guest's session ran on without a window after vitrine quit (2026-10-15). KWin's Wayland backend quits on close: the session ends, and the guest goes back to its login screen |
