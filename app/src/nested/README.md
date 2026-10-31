# nested

The nested desktop in vitrine's window: the guest's KDE session (its nested
KWin, over virtio-gpu cross-domain) shown as part of the window, beside the
VM's own screen.

`vmview.{h,cpp}`, `protocols.{h,txt}`, `gen_protocols.py` and `LICENSE` are
the research side's vmview library (`src/vmview` of the virtio-gpu-desktop
repository, commit 95ea57d; its 9240dd0 took vitrine's changes of 776a6f2,
4a8e155, cbb9ebc and 175aa1a). QEMU's cross-domain connects to a socket the
library serves; the guest's Wayland protocol goes on to the host compositor
over vitrine's own connection, and the guest's window becomes a subsurface
of a surface of vitrine's. The library is not stable yet: keep its files as
they are, so that its changes port over by copying them. `sha256sum -c
UPSTREAM.sha256` lists the files that no longer match the version taken;
the changes vitrine needs before the research side has them are under Local
changes below, to make again after a copy. Not taken: the library's
CMakeLists.txt (app/CMakeLists.txt builds it the same way: a static
library, its protocol code generated with wayland-scanner and
`gen_protocols.py` from wayland-protocols), its demo, tools and README.

The library is under the GNU General Public License, version 2 or later, as
`LICENSE` says; the `../../LICENSE` it names is the research repository's
copy of the licence's text, the same as vitrine's top-level `LICENSE`. The
research side relicensed it from MIT after its commit 182e74d (in b16fca6):
vitrine's copies until 2026-10-15, of 182e74d and before, were MIT.

vitrine's own code is `nestedview.{h,cpp}`: the Qt side of it (a surface of
vitrine's that the guest's window hangs from, in the console or full
screen, and the library's callbacks brought to the GUI thread).

The research's review of 2026-10-31 (its ca39424, 1eb4a6a, 4dd61fa and
95ea57d) changed what a guest can do to vitrine:
- The host's clipboard reaches a guest only while its view has the keys
  (`NestedView::updateKeyboard`), and the latest copy when it gets them
  back. Before, a VM in the background could read what was copied.
- Requests the host compositor would refuse with an error on vitrine's
  own connection, which ends vitrine, are refused on the guest's: a bad
  buffer scale or transform, shared memory sizes <= 0, a dma-buf plane
  index > 3, a buffer the host cannot import (create_immed), and a colour
  management creator for a feature the host did not announce (a
  create_icc_creator sent to a host without ICC support ended vitrine).
  So are the rules that need a surface's state: a second viewport,
  fractional scale, constraint, inhibitor or other object of one kind on a
  surface, a subsurface cycle, a surface given two roles, and a subsurface
  placed next to a surface that is not its sibling.
- A drag that leaves vitrine's window without a release (an effect's
  grab, Alt+Tab) ends in the guest, so that vitrine's own widgets get
  their clicks again.
- More than 128 file descriptors left over close the guest's connection.

## Local changes

None: the files are the research's at 95ea57d.

The research side took vitrine's earlier ones: a `pressed` callback for
button presses on the guest's window and a NULL object where the protocol
allows none counting as the app's (its 182e74d), and `View::stopAll()`
sending the guest's toplevel `xdg_toplevel.close` before it closes the
connections, so that the guest's session ends at once, without KWin
restarts, when vitrine quits (its a763b59). Its 9240dd0 has the last two:

- A `shortcuts` callback (2026-10-15, vitrine's cbb9ebc): the guest's
  shortcuts inhibitor, which the library moves to PARENT, made (captured)
  and gone. vitrine's PARENT is a surface of its own, a subsurface of a
  subsurface of its window. The compositor applies an inhibitor only on the
  surface with the keyboard focus, so Meta reached both the guest's desktop
  and the host's. `nestedview.cpp` takes the window's shortcuts itself
  meanwhile, through WaylandExtras, which shares one inhibitor per surface
  with the D-Bus display's views.
- A drag that leaves the guest's window (2026-10-30, vitrine's 175aa1a,
  `View::event`, the pointer's case): the host keeps the pointer on the
  app's window while a button is down, but moves it between the window's
  surfaces (KWin 6.7, as for the D-Bus display's screen,
  ../vmview/README.md). Over one of the app's surfaces the guest lost the
  pointer: a window dragged by its title bar past the view's edge was never
  let go in the guest, nor tiled or maximized there. While a button the
  guest saw pressed is down, the guest keeps the pointer: over the app's
  surfaces its motion goes to the guest's surface, moved by the offset
  taken where the pointer crossed (the nested KWin clamps it to its
  screen's edge, where it tiles), the release reaches the guest, then its
  leave.
