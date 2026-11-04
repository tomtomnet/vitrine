# gtkview

The VM display in the GTK app: QEMU's D-Bus display (`-display
dbus,p2p=yes,gl=on`) as a GTK4 widget, with the frame path of the Qt view
(`src/vmview`): the guest's buffers attached as they are when the sizes match
(zero copy) or copied, and QEMU told when each frame reached the screen.

The research side's client files in `src/vmview` (D-Bus display, listener,
render thread, swapchain, QMP, statistics) build here as they are, without
Qt GUI: there is no QGuiApplication in the GTK app, and they use a few Qt GUI
classes (QOpenGLContext and its functions, QWindow, QImage). `qtgui/` stands in
for those, with Qt's names and only what the files call: an EGL context of
desktop OpenGL on GTK's Wayland connection, GL entry points through
`eglGetProcAddress`, the window state the renderer reads, the cursor image.
A target that uses it must not link Qt6::Gui (`qtguicompat.h` stops the build
if it does). The research's changes keep coming over by copying its files.

| file | what it does |
|---|---|
| `vmdisplay` | attaches to a running QEMU (QMP getfd + add_client) and shows its screen: VmView's calls |
| `displaywidget` | the widget: places the frames' surface, input, cursor, grab, guest size |
| `displaysurface` | the frames' own wl_surface, a desync subsurface of GTK's window that the render thread draws and commits |
| `displayinput` | DisplayWindow's handling of keys, buttons, wheel and grab, without a toolkit |
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
  toast) is under the frames' surface. Dialogs over the VM's screen need
  windows of their own, or the view hidden while they show.
- Keys come through the widget's key controller while it has the focus, as
  evdev codes (Wayland's keycode - 8), and go to QEMU as its qnum codes; the
  pointer through a motion controller, the buttons as they come (a legacy
  controller: a gesture would take drags for its own), the wheel through a
  scroll controller. The desktop's shortcuts go to the guest through GDK's
  `gdk_toplevel_inhibit_system_shortcuts` (GNOME asks for that per
  application ID: `org.vitrine.Vitrine`); the pointer lock and confinement
  are vitrine's own Wayland objects, on the window's main surface.
- Full screen moves the widget to a window of its own, as the Qt view does.
