# nested

The nested desktop in vitrine's window: the guest's KDE session (its nested
KWin, over virtio-gpu cross-domain) shown as part of the window, beside the
VM's own screens.

`vmview.{h,cpp}`, `protocols.{h,txt}`, `gen_protocols.py` and `LICENSE` are
the research side's vmview library (`src/vmview` of the virtio-gpu-desktop
repository, commit 5f7e6af9e9f0), under the MIT licence of `LICENSE`. QEMU's
cross-domain connects to a socket the library serves; the guest's Wayland
protocol goes on to the host compositor over vitrine's own connection, and
the guest's window becomes a subsurface of a surface of vitrine's. The
library is not stable yet: keep its files as they are, so that its changes
port over by copying them. `sha256sum -c UPSTREAM.sha256` lists the files
that no longer match the version taken; the changes vitrine needs before the
research side has them are under Local changes below, to make again after a
copy. Not taken: the library's CMakeLists.txt (app/CMakeLists.txt builds it
the same way: a static library, its protocol code generated with
wayland-scanner and `gen_protocols.py` from wayland-protocols), its demo,
tools and README.

vitrine's own code is `nestedview.{h,cpp}`: the Qt side of it (a surface of
vitrine's that the guest's window hangs from, in the console or full
screen, and the library's callbacks brought to the GUI thread).

## Local changes

| file | change | why |
|---|---|---|
| `vmview.{h,cpp}` | `vmview_callbacks.pressed`: called (on the library's thread) when a pointer button goes down on the guest's window | the compositor sends those clicks to the guest's surface, never to Qt: without it, a click on the desktop could not give the guest the keyboard back once a widget of vitrine's (the list of VMs) had taken it, as a click on the VM's screen does |
| `vmview.cpp` | an event whose object argument is NULL where the protocol allows none counts as one about the app's objects (`foreign`), not the guest's | libwayland passes an object the app destroyed as NULL: vitrine's full-screen window closed, its keyboard leave went to the guest as `wl_keyboard.leave(serial, NULL)`, and the guest's client died of it ("NULL object received on non-nullable type"). Seen on the nested KWin with a guest's Konsole on the proxy, 2026-10-14 |
