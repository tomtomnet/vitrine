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
| `gtkclipboard` | the host clipboard shared with the guest, with GdkClipboard |
| `pointerconstraints` | pointer lock with relative motion, and confinement, on GTK's Wayland connection |
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
  `gdk_toplevel_inhibit_system_shortcuts` (GNOME asks the user once per
  application ID, `org.vitrine.Vitrine`, and remembers an Allow; the state
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
  it changes, and QEMU's Request answered from that.
- Full screen moves the widget to a window of its own, as the Qt view does.
