# nested

The nested desktop in vitrine's window: the guest's KDE session (its nested
KWin, over virtio-gpu cross-domain) shown as part of the window, beside the
VM's own screen.

`vmview.{h,cpp}`, `protocols.{h,txt}`, `protocols/presentation-time.xml`,
`gen_protocols.py` and `LICENSE` are the research side's vmview library
(`src/vmview` of the virtio-gpu-desktop repository, commit 3770e96, which
has vitrine's changes up to 0327fe9; its 9240dd0 took those of 776a6f2,
4a8e155, cbb9ebc and 175aa1a). QEMU's
cross-domain connects to a socket the
library serves; the guest's Wayland protocol goes on to the host compositor
over vitrine's own connection, and the guest's window becomes a subsurface
of a surface of vitrine's. The library is not stable yet: keep its files as
they are, so that its changes port over by copying them. `sha256sum -c
UPSTREAM.sha256` lists the files that no longer match the version taken;
the changes vitrine needs before the research side has them are under Local
changes below, to make again after a copy. Not taken: the library's
CMakeLists.txt (app/CMakeLists.txt builds it the same way: a static
library, its protocol code generated with wayland-scanner and
`gen_protocols.py` from wayland-protocols, `./` lines of protocols.txt from
this folder), its demo, tools and README.

presentation-time is the library's own copy at version 3 (its 3770e96): the
host KWin's content-update deadline (vitrine's host KWin, patch 0012) reaches
the guest's nested KWin (its 0029). From wayland-protocols' version 2 the
library offered the guest version 2 only, and the guest's commits were paced
without the host's deadline (2026-11-12). vitrine's views use version 2 for
their own feedback (`vitrine_wlprotocols`: their listeners know three events,
and a view bound at version 3 would be sent a fourth). One program has one
symbol of each name, so app/CMakeLists.txt compiles the two generated files of
the library that define and use its v3 interfaces with
`wp_presentation_interface` and `wp_presentation_feedback_interface` renamed
(`vmview_v3_...`); the names on the wire stay the same, and vmview.cpp's own
feedback (version 1) keeps the shared version 2 interfaces.

The library is under the GNU General Public License, version 2 or later, as
`LICENSE` says; the `../../LICENSE` it names is the research repository's
copy of the licence's text, the same as vitrine's top-level `LICENSE`. The
research side relicensed it from MIT after its commit 182e74d (in b16fca6):
vitrine's copies until 2026-10-15, of 182e74d and before, were MIT.

vitrine's own code is `nestedview.{h,cpp}`: the Qt side of it (a surface of
vitrine's that the guest's window hangs from, in the console or full
screen, and the library's callbacks brought to the GUI thread).

It also tells which threads the guest's desktop goes through, for real time
while the VM is in front (the helper's `view-rt`, as for the GTK app's
screen: ../../docs/host-tuning.md): the library's own, which passes the
guest's frames on to the compositor and its events back, and the app's other
readers of its Wayland connection, which that thread's reads wait for
(libwayland reads a connection only once every thread that prepared to read
it has). The library does not tell its thread: its callbacks
run on it, and the first comes as the guest connects. In the Qt app the
readers are Qt's two Wayland event threads (`QtWaylandClient`, Qt's
default queue's and its frame callbacks'), and the screen's (src/vmview's
`VmView::readerThread()`); the GTK app's (`../gtkview/nesteddisplay.cpp`)
is its main thread. Each change goes to stderr as one line,
`vitrine: desktop: threads: library=TID reader=TID ...` (the GTK app's:
`main=TID library=TID`), and the console hands them to the helper
(`VmConsole::realtimeThreads()`). `VITRINE_VIEW_RT=0`: none.

The research's review of 2026-10-31 (its ca39424, 1eb4a6a, 4dd61fa,
95ea57d, 45f062a, bb3b1f9, fae51db and e9a66da) changed what a guest can
do to vitrine:
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
- Limits on what a guest makes vitrine hold: a guest connection with more
  than 16 MiB of events it has not read is closed, and so is one with more
  than 65,536 objects; at most 64 guest connections. Once 64 KiB wait
  unread, each pointer's motion is coalesced (the latest position, the
  relative motion summed), so a paused VM with the mouse moving over its
  view stays far below the limit.
- The guest's surfaces are kept to the view: each gets a crop of the
  library's (a host viewport, the guest's own composed into it), which
  cuts what lies outside the view, its input included. Before, a guest
  surface placed or sized past the view was drawn over vitrine's own
  widgets in the window and took their clicks. The guest's toplevels hang
  from an anchor of the library's, a transparent subsurface of PARENT at
  the view's top-left, which `vmview_set_geometry` moves: PARENT's next
  commit applies it, as `NestedView::updateGeometry` does.
- Events that name another guest connection's object are kept from the
  guest.
- The guest's window counts as gone (the `mapped` callback, on which
  vitrine shows the VM's screen) only once no guest connection has one
  left: a second guest client's window that closed took the console to
  the Screen view while the nested desktop was still there.

## Local changes

None now: the files are the research's at 3770e96 (`sha256sum -c
UPSTREAM.sha256` passes); its a6ca435 took the two changes below from vitrine. They
stay described here, for a copy of an older version:

- A `pointer` callback (2026-11-11, vitrine's view-keys stream): the
  pointer came over the guest's window, or left it. vitrine gives the guest
  the keys only while the pointer is over its window, or while the guest
  captures the desktop's shortcuts: with the pointer elsewhere, the host
  keeps its shortcuts and a key reached both (on the VM's screen, Super
  opened both overviews; the same rule there, ../gtkview/README.md). The
  research side may take it as it is. GTK cannot tell: GDK gets a leave
  of vitrine's window as the pointer enters a guest surface, and passes
  over the guest's surfaces (the change below). The change:
  - `vmview.h`: `void (*pointer)(void *data, bool over)`, last in
    `vmview_callbacks`;
  - `vmview.cpp`: `View::setPointerFocus(Object *pointer, bool on)` sets a
    guest pointer's `focus` (over a guest surface) and counts them
    (`pointersOver`); the callback comes when that count goes from 0 to 1
    or back (`pointerOver`), with a line under VMVIEW_DEBUG. The five
    places of the event handler's `Role::Pointer` case that set `o->focus`
    call it instead (enter on a guest surface: on; enter on one of the
    app's surfaces outside a drag, leave, a leave whose frame brings no
    enter, the release that ends a drag outside: off), and `hostDestroy`
    calls it with off for a pointer (destroyed by the guest, or its
    connection closed). A drag that leaves the guest's window keeps the
    guest pointer's focus until its release, as before: it counts as over.

- No user data on the library's host objects (2026-11-05, for the GTK app):
  `View::watch` gives `wl_proxy_add_dispatcher` a null user data instead of
  the guest's `Object`, which the library never read back (it finds its
  objects through `byProxy`). The library's objects live on the app's
  connection, so the app's seat gets the compositor's events for the
  guest's surfaces too. GDK 4.24 checks the user data of every surface a
  pointer, keyboard, tablet or drag event names (`GDK_IS_SURFACE
  (wl_surface_get_user_data (surface))` in gdkseat-wayland.c) and leaves out
  the surfaces that are not its own. With an `Object` there, whose first
  field is an object id, that check read the id as a class pointer: the GTK
  app would have crashed as the pointer entered the guest's window. With
  none, GDK passes over the guest's surfaces. Qt was never affected: it
  recognises its surfaces by their listener.
  - One gap stays on GTK's side: its touch handler (`touch_handle_down`)
    checks the surface for NULL only, not its user data, so a touch on the
    guest's window makes GDK deliver a touch event without a surface (and
    clear its pointer's focus). Whether GTK then only warns or crashes is
    not known (not tried: no touchscreen here). That is GTK's to fix;
    vitrine does not work around it. Before this change such a touch took
    the guest's `Object` for a GdkSurface.

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
