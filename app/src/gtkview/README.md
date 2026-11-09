# gtkview

The VM display in the GTK app: QEMU's D-Bus display (`-display
dbus,p2p=yes,gl=on`) as a GTK4 widget, with the frame path of the Qt view
(`src/vmview`): the guest's buffers attached as they are when the sizes match
(zero copy) or copied, and QEMU told when each frame reached the screen.

The research side's client files in `src/vmview` (D-Bus display, listener,
render thread, swapchain, QMP, statistics) build here as they are, byte for
byte, so that the research's changes keep coming over by copying its files
(`src/vmview/README.md`).

## The Qt GUI stand-ins (`qtgui/`)

Those files use a few Qt GUI classes: the render thread draws through a
QOpenGLContext and its QOpenGLFunctions, reads the format and the full-screen
state of a QWindow, and the listener hands the guest's cursor over as a QImage.
A real QOpenGLContext needs a QGuiApplication, which the GTK app does not have,
and the GTK app links no Qt GUI library. `qtgui/` has headers named after Qt's
(`QOpenGLContext`, `QOpenGLFunctions`, `QOpenGLExtraFunctions`, `QWindow`,
`QSurfaceFormat`, `QImage`) that the build puts before anything else on the
include path, all in `qtguicompat.h`:

- QOpenGLContext: an EGL context of desktop OpenGL (as Fedora's Qt makes it:
  the renderer's GL_BGRA uploads need it) on the EGL display of GTK's Wayland
  connection; its native interface gives the EGL display, config and context
  to the render target (`displaysurface`), which makes it current itself;
- QOpenGLFunctions, QOpenGLExtraFunctions: the GL calls the files make,
  resolved through `eglGetProcAddress`;
- QSurfaceFormat: the alpha size and the swap interval;
- QWindow: no window, the format and the window state the renderer reads
  (full screen: QEMU may then scan out the guest's buffers late);
- QImage: the cursor's pixels (ARGB32) and their copy.

The rules:

- **Exactly what the files call**, with Qt's names and signatures, nothing
  more: no class grows into a toolkit.
- **Never with Qt6::Gui.** Two definitions of Qt's classes in one program
  would break it in ways no compiler reports, so `qtguicompat.h` stops the
  build (`#error`) when QT_GUI_LIB is defined, which every target linking
  Qt6::Gui gets. The GTK targets (and `vitrine_core` for them) must not
  link Qt6::Gui, and the GTK app's sources use no Qt GUI type.
- **After a research sync** (its files copied into `src/vmview`): build
  `vitrine-gtk-view`. A Qt GUI call the research added shows as a compile
  error in `vitrine_gtkview` (an unknown member of one of these classes, or
  an unknown header): add that call to `qtguicompat.h` the way Qt behaves,
  and no more. Nothing to do when it compiles.

| file | what it does |
|---|---|
| `vmdisplay` | attaches to a running QEMU (QMP getfd + add_client) and shows its screen: VmView's calls |
| `displaywidget` | the widget: places the frames' surface, input, cursor, grab, guest size |
| `displaysurface` | the frames' own wl_surface, a desync subsurface of GTK's window that the render thread draws and commits |
| `displayinput` | DisplayWindow's handling of keys, buttons, wheel and grab, without a toolkit |
| `framestill` | the guest's frame as a GTK texture, for the widget while it is covered |
| `fullscreenorder` | a full-screen window maps after the window the view leaves: the switch waits while that window is shown but not yet active |
| `gtkclipboard` | the host clipboard shared with the guest, with GdkClipboard |
| `nesteddisplay` | the nested desktop: the guest's KDE session through src/nested's library on GDK's connection, in the console's Desktop view or full screen (src/nested's NestedView, in GTK) |
| `pointerconstraints` | pointer lock with relative motion, and confinement, on GTK's Wayland connection |
| `shortcutsinhibit` | the window's one shortcuts inhibitor, shared by the screen's grab and the nested desktop |
| `viewkeys` | the app's own keys on the views (the console's Ctrl+Alt+1/2), full screen too |
| `widgetrelease` | a view's widget out of focus before it leaves the window, its controllers off as its C++ side goes (GTK may keep the widget, and emit its controllers' signals, after that) |
| `qtgui/` | the Qt GUI classes the research files use, on EGL |
| `main.cpp` | `vitrine-gtk-view SOCKET`: a running VM's screen in a window of its own, to try the view |

How it fits GTK:

- The frames go to a surface of vitrine's over the widget, which GTK knows
  nothing of. The widget moves it where GTK laid the widget out, in the frame
  clock's layout phase, and the move takes effect with GTK's commit of that
  frame (`gdk_wayland_surface_force_next_commit`). Hidden (the widget
  unmapped, e.g. another page of a stack), the surface leaves the window.
- What GTK draws in the window over the widget (an in-window dialog, a
  toast) is under the frames' surface. While something covers it, the app
  calls `VmDisplay::setCovered(true)`: the frames' surface leaves the window
  and the widget draws a still of the guest's last frame (its buffer
  imported by GTK and read back at once, `framestill`), or black when GTK
  cannot import it; the guest runs on. Toasts belong outside the display.
- Keys come through the widget's key controller while it has the focus, as
  evdev codes (Wayland's keycode - 8), and go to QEMU as its qnum codes; the
  pointer through a motion controller, the buttons as they come (a legacy
  controller: a gesture would take drags for its own), the wheel through a
  scroll controller. The desktop's shortcuts go to the guest through GDK's
  `gdk_toplevel_inhibit_system_shortcuts` (how GNOME asks: below; the state
  line says when the desktop keeps its shortcuts); the pointer lock and
  confinement are vitrine's own Wayland objects, on the window's main
  surface, and work without the inhibitor.
- GTK runs the window's capture-phase shortcut controllers (the
  application's accelerators, F10) before the focused widget: while the
  view has the keyboard they are off, as the Qt view takes every key before
  the window's shortcuts. Input off (the VM paused), they are back, and the
  view keeps only Ctrl+Alt+F.
- No input method while the view has the keyboard: its key controller has
  no GtkIMContext, so GTK never enables Wayland's text input for it (a text
  field focused before disables it as the view takes the focus). A
  compositor routes the keys of a client with text input on through the
  input method first: on GNOME, QEMU's SDL window had its keys 1-3 ms late
  that way (the research's run of 2026-11-04). Checked on the nested KWin
  with WAYLAND_DEBUG: `zwp_text_input_v3.disable` when the view takes the
  focus from a text field, no text input request while keys go to the view.
- The clipboard (`gtkclipboard`) is VmClipboard's D-Bus side with
  GdkClipboard, which reads only asynchronously: the host's text is read as
  it changes, and QEMU's Request answered from that. When KWin refuses the
  guest's text (another client copied after vitrine's last input), GTK keeps
  the clipboard as its own and says nothing: the guest's text is set once
  more at vitrine's next input, unless the host copied something since.
- Full screen moves the widget to a window of its own, as the Qt view does.
  That window maps after the one the view leaves (`fullscreenorder`): a
  compositor focuses and raises the window it maps last - GNOME's Mutter
  does so with every new window that comes without startup information,
  which GTK sends none of for a second window - so a program started in
  full screen, whose two windows came up at once, had its full-screen
  window covered by the main window, mapped 180 ms later, which kept the
  keys (vitrine-gtk-view --full-screen on GNOME Shell 51).  The switch
  waits while the window the view is in is shown but not yet active, for
  two seconds at most.  In full screen, the window's surface (GTK's,
  black, opaque) has the frames' subsurface over all of it, opaque, with
  the guest's buffer attached as it is when zero copy holds: the topmost
  surface, which a compositor can put on the screen directly.  That
  buffer then comes back only after the compositor's next flip, and a
  guest whose flips wait for it draws a frame only once the one before is
  on the screen: a GNOME guest ran at half rate at 240 Hz while a window
  was dragged (an Arc Pro B50, the research's run of 2026-11-08). So for
  a guest whose flips wait (`blob_flush_fence` 1: any compositor but
  vitrine's KWin), and while the guest's agent has not said since the
  boot (`VmDisplay::setFlipsWaitForRelease`, which the console sets from
  the agent's report), full screen copies the guest's frames into the
  view's own buffers, which the compositor scans out as it does QEMU's
  SDL window, and the guest's buffer comes back at once. At QEMU's 6 ms
  swap target every frame was on time that way on an Arc Pro B50 and on
  a Radeon 780M at 4K 240, as in SDL's window, and keys reached the
  screen as quickly (the research's runs of 2026-11-09, lgd3, bfs2 and
  dvk3, GNOME 51 guests on GNOME 51.1 hosts: 0.2-2.4 stutters a second at
  rest on the 780M). Composited, under a nearly transparent pixel over
  the frames' corner (`DisplaySurface::setOverlay`, the default until
  then), 3-5 % of the 780M's frames came a refresh late. Under a compile
  the B50 still stuttered 1.5-7 times a second, 0.6-1.1 with the view's
  threads real-time, as the GTK app now makes them while the VM is in
  front (below). A guest whose flips
  don't wait keeps the direct scanout. QEMU hears of a copy or a
  composite as of a window (`Presentation.ZeroCopy` false,
  `Renderer::setFullScreenComposited`): it then aims the guest's frames
  at `x-vblank-swap-target`, not at the direct scanout's
  `x-vblank-swap-target-zc`. In a window the compositor composites
  anyway: zero copy stays.
- With each frame's presentation the view passes QEMU the compositor
  lead (the presentation less the frame callback of the same commit),
  for the frames it copies in full screen only; 0, not known, for the
  others (`Feedback::passLead`). From it a QEMU with
  `x-vblank-deadline-callback` estimates the host's deadline for those
  frames: their target is then the host's lead plus 1 ms and its margin
  (700 us), up at once and down after 8 calm windows
  (`x-vblank-deadline-hold=8`), which vitrine sets for guests other than
  KDE's in its window (`VmRunner::withComputedProperties`). The copied
  full screen came 0.35 ms sooner than at the fixed 6 ms, as smoothly, on
  a Radeon 780M and an Arc Pro B50 at rest and under CPU and GPU loads
  (the research's lgt4 and bcb3, 2026-11-09); without the margin the
  view's own copy came late under a GPU load on the 780M. A window of
  presentations without a lead (144) drops the estimate, so the window
  keeps the fixed 6 ms, and the direct scanout its own target. A user's
  swap target in `vm.args` keeps it fixed.
- The guest is told its screen's physical size with each size (UIInfo's
  `width_mm` and `height_mm`): the view's share of its monitor's, from
  GdkMonitor's width_mm and height_mm, through `core/physicalsize.h` as the Qt
  view does; none where the monitor does not say. QEMU puts it in the EDID,
  and a GNOME guest picks its scale from it (QEMU's 100 DPI without).

## The view's threads, real-time while its VM is in front

With host tuning on, the GTK app has vitrine-helper make the view's threads
SCHED_FIFO 1 while its VM is in front, as it does the VM's QEMU threads
(`view-rt`, docs/host-tuning.md): the app's main thread, GTK's, which
passes the keyboard and mouse to QEMU and draws the window (its frame
clock); the render thread (`vmview-render`), its Wayland reader
(`vmview-events`), the listener (`dbus-listener`) and GLib's D-Bus worker
(`gdbus`, which carries the view's calls). Not GTK's other threads (GLib's
pool workers, `gmain`, dconf's). What they start begins ordinary
(SCHED_RESET_ON_FORK): the main thread starts QEMU and builds, and a new
render thread must start ordinary for its driver threads to be found.
The view also finds the threads the GL driver started for the render
thread's context - Mesa's glthread and threaded context, which do the
render thread's GL work (radeonsi's `<process>:gl0` and `:gdrv0`) - but
makes them real-time only with `VITRINE_VIEW_RT=all`: on a B50 under a
compile they did not help, and beside an ordinary main thread they made
it worse (below). Mesa names its threads after the process, alike for
GTK's contexts, so the view tells its own apart by who started them
(`viewthreads`): the render thread runs at SCHED_BATCH
while it creates its context and first makes it current, the threads it
starts inherit that, and those that appeared at SCHED_BATCH with its nice
value are its own (Mesa's low-priority queues, its disk cache writer and
u_trace's, are SCHED_BATCH at nice 19 of their own: left alone). The
render thread's and its reader's ids come from `RenderTarget::threadStarted`,
the listener's from `Listener::Callbacks::threadStarted` (vmview's local
changes). `VmDisplay::threads()` has the set, once the render thread's
reader runs, and again after each full-screen switch, which makes the render
thread, its reader and its context anew; VmConsole hands it to HostSettings.
On a B50 under a compile, the copied full screen of a GNOME guest lost
3.0 frames a second with the view's threads at normal priority (6 boots),
0.9-1.5 with the render thread, its reader, the listener and GDBus's
worker real-time (9, in two runs), 1.8 with the driver thread too (7),
0.8 with the main thread as well (3) and 0.8 with every thread of
`vitrine-gtk-view` (11); the default, all but the driver thread, was not
run on its own (the research's bfs3, brt1, brt2 and brt4, 2026-11-09;
every boot had 99-100 % of its frames on time).

## The nested desktop (`nesteddisplay`)

The guest's session (NestedDesktop::Mode::Integrated) goes through
src/nested's library as in the Qt app: it serves the socket QEMU's
cross-domain connects to, and passes the guest's Wayland protocol on to the
compositor over GDK's own connection, on a queue and a thread of its own
(GDK reads the same connection; libwayland's prepare/read protocol lets
both). The guest's window hangs from a surface of vitrine's - one
transparent pixel, no input - a desync subsurface of the window's, placed
in the frame clock's layout phase where GTK laid the view out, as the
screen's own surface is (`displaysurface`).

- The pointer over the guest's window goes to the guest; beside it, to the
  view, whose click gives the guest the keys, as a click on the guest's
  window does (the library's `pressed`). GDK gets the compositor's events
  for the guest's surfaces too, and passes over them: they carry no user
  data (src/nested/README.md, Local changes).
- The keys reach the guest through the library, which reads the window's
  keyboard, while the view has the focus in the active window and input is
  on (`vmview_set_keyboard_focus`). GTK gets them too: the view takes them
  before the window's shortcuts and widgets, holding the window's capture
  shortcut controllers meanwhile, as DisplayWidget does, with no input
  method. Ctrl+Alt+F is the guest's own once its window is there.
- The guest's shortcuts capture (its session asks: the library's
  `shortcuts`) goes to the window that has the keys, through GDK's
  inhibitor (`shortcutsinhibit`): the library puts the guest's on its
  parent surface, which never has the keyboard focus. Not on GNOME for an
  app it does not know (below).
- Full screen: a window of its own, the parent surface moved there.
- Covered by an in-window dialog: the parent surface leaves the window
  (the guest's window with it, over anything GTK draws) and the view is
  black until uncovered; the session runs on. No still: the guest's
  buffers are the compositor's to see, not vitrine's.
- Its geometry goes to the library only once GTK gave the view a size: a
  full-screen window just made has none yet, and the guest's desktop would
  follow a 1x1 view.

## How GNOME asks for the desktop's shortcuts

GNOME Shell answers an inhibitor request with its "Allow inhibiting
shortcuts" dialog, which takes the keyboard and the pointer from the VM
until answered (checked on GNOME Shell 51.1, 2026-11-05):

- For an app it knows by its desktop file (`org.vitrine.Vitrine.desktop`
  installed, its program in PATH), the answer goes to its permission store
  (table `gnome`, id `shortcuts-inhibitor`, key
  `org.vitrine.Vitrine.desktop`): one dialog in the app's life, then the
  same answer for all its windows, the full-screen ones too, and for
  vitrine-gtk-view (the same application ID). GNOME's Settings change it:
  Apps, the app's Inhibit Shortcuts. After Deny the keys still reach the guest; the desktop keeps its
  shortcuts, and Super then opens GNOME's overview as well as the guest's
  menu (GNOME acts on the key's release, the guest got its press: QEMU's
  own displays behave the same).
- For a window it cannot match ("window-backed": vitrine run from its
  build tree, or not installed), Mutter keeps the answer for that surface
  only: the dialog came at every run and again at every full-screen window,
  as soon as the pointer was over the screen with the keyboard, and a
  script that did not answer it never had the keys.

So on GNOME (`XDG_CURRENT_DESKTOP` lists `GNOME`) for an app it does not
know, the views ask only when the user grabs with Ctrl+Alt+G: no automatic
grab of the pointer over the screen, nor the nested desktop's session
asking, and the state line reads "not grabbed (Ctrl+Alt+G grabs; GNOME asks
first)" (`ShortcutsInhibit::automatic()`). KWin grants without asking, and
other desktops and a known app keep the automatic grab.
`VITRINE_VIEW_INHIBIT=0` never asks at all, Ctrl+Alt+G included (scripted
runs).

## Switches for A/B runs

Environment variables of the views, for measurements only; the defaults are
what vitrine ships:

- `VITRINE_VIEW_ZERO_COPY=0`: the view copies the guest's frames into its
  own buffers instead of showing them as they are.
- `VITRINE_VIEW_FULL_SCREEN_ZERO_COPY=1` or `0`: in full screen, the
  guest's buffers go on the frames' surface as they are, for the
  compositor to scan out (1), or are copied (0), whatever the guest's
  driver says; unset, they are copied for a guest whose flips wait for
  the release (above). `VITRINE_VIEW_COMPOSITE=1`, alone or with `1`,
  has the compositor composite them under the pixel. The view says on
  stderr which of the three full screen takes, and why, at its start and
  at each change ("vitrine: display: full screen: ...").
  `vitrine-gtk-view` has no agent to ask: `--flips-wait 0` says what the
  agent of a guest with `blob_flush_fence` 3 would, and
  `--flips-wait-toggle SECONDS` turns it over while the view shows.
- `VITRINE_VIEW_INHIBIT=0`: never asks for the desktop's shortcuts (above).
- `VITRINE_VIEW_RT`: which of the view's threads the GTK app has made
  real-time while its VM is in front (above): unset or `named`, all but
  the driver threads (main, render, events, listener, gdbus); `all` or
  `1`, all of them; `0`, none. Said on stderr once when set.
  `vitrine-gtk-view` makes none real-time, whatever it says: a bench
  raises the threads of its thread line (below).
- `VITRINE_VIEW_COMPOSITE=1`: the pixel over the frames
  (`DisplaySurface::setOverlay`) whatever the guest, windowed too, so
  that the compositor composites the view instead of scanning its frames
  out directly: GNOME's Mutter scans out only a topmost surface that
  covers the screen. `vitrine-gtk-view --stats` counts the frames scanned
  out ("direct"): with the pixel, none should be.

### The thread line (a stable interface)

The view, in the app and in `vitrine-gtk-view`, writes its threads to
stderr as one line, for benches to raise exactly those (`chrt`):

```
vitrine: display: threads: main=TID render=TID events=TID listener=TID gdbus=TID driver=TID:NAME ...
```

- One line per change of the set, once the render thread's set is complete
  (its Wayland reader started): at the view's start, after each full-screen
  switch (a new render thread, reader and driver threads; the listener and
  gdbus stay), and when the listener's thread shows after that. Never
  wrapped; nothing else on the line.
- After the prefix `vitrine: display: threads:`, tokens separated by one
  space: `main=TID` (the process's main thread, GTK's: its TID is the
  process id), `render=TID`, `events=TID`, `listener=TID`, `gdbus=TID` in
  that order, each left out when its thread is not there; then `driver=TID:NAME`
  for each thread the GL driver started for the render thread's context, by
  id. TID: a Linux thread id of the view's process (`/proc/PID/task/TID`),
  digits only, ended by a space or, for a driver thread, by the first `:`.
  NAME: the thread's name as Linux has it (`/proc/PID/task/TID/comm`, 15
  characters at most; Mesa's `<process>:<queue>N`), any character but
  letters, digits and `._:/+$-` turned into `_`.
- After a full-screen switch, the next line is the new set (its `render=`
  is new); the last line printed is the set in use.
- Example (`vitrine-gtk-view` on a Radeon 780M, radeonsi):

  ```
  vitrine: display: threads: main=3869880 render=3870319 events=3870327 listener=3869904 gdbus=3869892 driver=3870321:vitrine-:gdrv0 driver=3870322:vitrine-gt:gl0
  ```

- Read in a shell, raised as the app raises them by default: not the
  driver threads, and with SCHED_RESET_ON_FORK (`-R`), so that what they
  start begins ordinary (a new render thread inheriting real time from
  the main thread would hide its driver threads):

  ```
  line=$(grep '^vitrine: display: threads: ' view.log | tail -1)
  for tok in ${line#vitrine: display: threads: }; do
      case $tok in driver=*) continue ;; esac
      tid=${tok#*=}; tid=${tid%%:*}
      sudo chrt -f -R -p 1 "$tid"
  done
  ```
