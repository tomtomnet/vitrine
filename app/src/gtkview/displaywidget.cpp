// SPDX-License-Identifier: GPL-2.0-or-later
#include "displaywidget.h"

#include "displaysurface.h"
#include "pointerconstraints.h"
#include "shortcutsinhibit.h"
#include "viewkeys.h"
#include "widgetrelease.h"
#include "core/physicalsize.h"
#include "vmview/dbusdisplay.h"
#include "vmview/renderer.h"

#include <QImage>
#include <QRect>
#include <QWindow>

#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerlegacy.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>

#include <gdk/wayland/gdkwayland.h>

#include <cmath>

namespace {

/* GDK's buttons (evdev BTN_LEFT... on Wayland) as QEMU's */
bool qemuButton(guint button, uint32_t *out)
{
    switch (button) {
    case GDK_BUTTON_PRIMARY: *out = DisplayInput::Left; return true;
    case GDK_BUTTON_MIDDLE: *out = DisplayInput::Middle; return true;
    case GDK_BUTTON_SECONDARY: *out = DisplayInput::Right; return true;
    case 8: *out = DisplayInput::Side; return true;
    case 9: *out = DisplayInput::Extra; return true;
    default: return false;
    }
}

/* The guest's cursor: its pixels are the screen's when the guest's mode
   matches the view, so GTK gets it at each scale it shows it at, its size
   in logical pixels the image's over the scale */
struct GuestCursor {
    GdkTexture *texture;
    int hotX, hotY;
};

GdkTexture *cursorTexture(GdkCursor *, int, double scale, int *width, int *height, int *hotX,
                          int *hotY, gpointer data)
{
    const auto *c = static_cast<const GuestCursor *>(data);
    scale = scale > 0 ? scale : 1;
    *width = std::max(1, int(std::lround(gdk_texture_get_width(c->texture) / scale)));
    *height = std::max(1, int(std::lround(gdk_texture_get_height(c->texture) / scale)));
    *hotX = int(std::lround(c->hotX / scale));
    *hotY = int(std::lround(c->hotY / scale));
    return GDK_TEXTURE(g_object_ref(c->texture));
}

} // namespace

DisplayWidget::DisplayWidget(DBusDisplay *display, DisplaySurface *surface, const Options &opts)
    : Glib::ObjectBase("VitrineDisplay"), m_display(display), m_surface(surface), m_opts(opts),
      m_window(std::make_unique<QWindow>()), m_input(this, this)
{
    set_focusable(true);
    set_overflow(Gtk::Overflow::HIDDEN);
    set_hexpand(true);
    set_vexpand(true);

    QSurfaceFormat format;
    format.setAlphaBufferSize(0); // opaque: the compositor needs no blending
    /* 0 = present each guest frame at once, like QEMU's SDL display */
    format.setSwapInterval(opts.vsync ? 1 : 0);
    m_window->setFormat(format);

    m_input.grabChanged = [this]() { grabChanged.emit(); };
    /* the guest's tablet driver switches the mouse to absolute after boot */
    m_modeWatch = QObject::connect(m_display, &DBusDisplay::mouseModeChanged,
                                   [this](bool) { m_input.mouseModeChanged(); });
    m_input.cursorChanged = [this]() { updateCursor(); };

    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(sigc::mem_fun(*this, &DisplayWidget::onKeyPressed), false);
    key->signal_key_released().connect(sigc::mem_fun(*this, &DisplayWidget::onKeyReleased));
    add_controller(key);

    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_enter().connect([this]() { focusChanged.emit(); });
    focus->signal_leave().connect([this]() { focusChanged.emit(); });
    add_controller(focus);

    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_enter().connect([this](double x, double y) {
        m_input.pointerEnter(x, y);
        inputSeen.emit();
    });
    motion->signal_leave().connect([this]() { m_input.pointerLeave(); });
    motion->signal_motion().connect([this](double x, double y) { m_input.motion(x, y); });
    add_controller(motion);

    /* the buttons as they come: a gesture would take a drag for its own */
    auto legacy = Gtk::EventControllerLegacy::create();
    legacy->signal_event().connect(sigc::mem_fun(*this, &DisplayWidget::onEvent), false);
    add_controller(legacy);

    m_scroll = Gtk::EventControllerScroll::create();
    m_scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    m_scroll->signal_scroll().connect(sigc::mem_fun(*this, &DisplayWidget::onScroll), false);
    add_controller(m_scroll);
}

DisplayWidget::~DisplayWidget()
{
    /* GTK may keep the widget past this (the window's focus): no event to this object any more */
    WidgetRelease::dropControllers(GTK_WIDGET(gobj()));
    QObject::disconnect(m_modeWatch);
    m_uiInfoTimer.disconnect();
    if (m_cursorTexture) {
        g_object_unref(m_cursorTexture);
    }
    if (m_still) {
        g_object_unref(m_still);
    }
}

Gtk::SizeRequestMode DisplayWidget::get_request_mode_vfunc() const
{
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
}

void DisplayWidget::measure_vfunc(Gtk::Orientation orientation, int, int &minimum, int &natural,
                                  int &minimumBaseline, int &naturalBaseline) const
{
    minimum = natural = orientation == Gtk::Orientation::HORIZONTAL ? 160 : 100;
    minimumBaseline = naturalBaseline = -1;
}

void DisplayWidget::size_allocate_vfunc(int width, int height, int)
{
    m_width = width;
    m_height = height;
    updateSize();
    updateExposed();
}

void DisplayWidget::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot> &snapshot)
{
    /* under the frames, and where they are not yet after a resize */
    const GdkRGBA black = {0, 0, 0, 1};
    const graphene_rect_t r = GRAPHENE_RECT_INIT(0, 0, float(get_width()), float(get_height()));
    gtk_snapshot_append_color(snapshot->gobj(), &black, &r);
    /* covered: the guest's last frame, letterboxed as the renderer draws it */
    if (m_covered && m_still) {
        const QRect fit = fitRect(QSize(get_width(), get_height()),
                                  QSize(gdk_texture_get_width(m_still),
                                        gdk_texture_get_height(m_still)));
        if (!fit.isEmpty()) {
            const graphene_rect_t at = GRAPHENE_RECT_INIT(float(fit.x()), float(fit.y()),
                                                          float(fit.width()), float(fit.height()));
            gtk_snapshot_append_scaled_texture(snapshot->gobj(), m_still,
                                               GSK_SCALING_FILTER_LINEAR, &at);
        }
    }
}

GdkSurface *DisplayWidget::toplevelSurface() const
{
    GtkNative *native = gtk_widget_get_native(const_cast<GtkWidget *>(gobj()));
    return native ? gtk_native_get_surface(native) : nullptr;
}

void DisplayWidget::on_realize()
{
    Gtk::Widget::on_realize();
    m_constraints = PointerConstraints::create(gtk_widget_get_display(gobj()));
    if (m_constraints) {
        m_constraints->relativeMotion = [this](double dx, double dy) {
            m_input.relativeMotion(dx, dy);
        };
        m_constraints->lockActive = [this](bool active) { m_input.setPointerLockActive(active); };
    }
    /* after GTK's own layout of the window, before its paint and commit */
    m_clock = GDK_FRAME_CLOCK(g_object_ref(gtk_widget_get_frame_clock(gobj())));
    m_layoutId = g_signal_connect_after(m_clock, "layout", G_CALLBACK(onLayout), this);
    if ((m_watched = toplevelSurface())) {
        g_object_ref(m_watched);
        m_scaleId = g_signal_connect_swapped(m_watched, "notify::scale", G_CALLBACK(+[](gpointer self) {
            auto *w = static_cast<DisplayWidget *>(self);
            w->updateSize();
            w->screenChanged.emit();
        }), this);
        m_enterMonitorId = g_signal_connect_swapped(m_watched, "enter-monitor", G_CALLBACK(+[](gpointer self) {
            static_cast<DisplayWidget *>(self)->screenChanged.emit();
        }), this);
        if (GDK_IS_TOPLEVEL(m_watched)) {
            m_inhibitId = g_signal_connect_swapped(m_watched, "notify::shortcuts-inhibited",
                                                   G_CALLBACK(+[](gpointer self) {
                auto *w = static_cast<DisplayWidget *>(self);
                gboolean active = FALSE;
                g_object_get(w->m_watched, "shortcuts-inhibited", &active, nullptr);
                w->m_inhibitActive = active;
                w->grabChanged.emit();
            }), this);
        }
    }
    if ((m_root = dynamic_cast<Gtk::Window *>(get_root()))) {
        m_activeWatch = m_root->property_is_active().signal_changed().connect(
            [this]() { focusChanged.emit(); });
    }
    updateSize();
}

void DisplayWidget::on_unrealize()
{
    /* the keys held up, the grab gone, the window's shortcuts back, while
       the window is still there */
    m_input.setHostActive(false);
    holdWindowShortcuts(false);
    setShortcutsInhibited(false);
    if (m_constraints) {
        m_constraints->setLocked(nullptr, false);
        m_constraints->setConfined(nullptr, false);
    }
    m_constraints.reset();
    m_activeWatch.disconnect();
    m_root = nullptr;
    if (m_watched) {
        g_signal_handler_disconnect(m_watched, m_scaleId);
        g_signal_handler_disconnect(m_watched, m_enterMonitorId);
        if (m_inhibitId) {
            g_signal_handler_disconnect(m_watched, std::exchange(m_inhibitId, 0));
        }
        m_inhibitActive = false;
        g_object_unref(std::exchange(m_watched, nullptr));
    }
    if (m_clock) {
        g_signal_handler_disconnect(m_clock, m_layoutId);
        g_object_unref(std::exchange(m_clock, nullptr));
    }
    Gtk::Widget::on_unrealize();
}

void DisplayWidget::on_map()
{
    Gtk::Widget::on_map();
    m_mapped = true;
    updateExposed();
}

void DisplayWidget::on_unmap()
{
    m_mapped = false;
    updateExposed();
    Gtk::Widget::on_unmap();
}

void DisplayWidget::setCovered(bool covered, GdkTexture *still)
{
    if (m_still) {
        g_object_unref(m_still);
    }
    m_still = covered ? still : nullptr;
    if (!covered && still) {
        g_object_unref(still);
    }
    m_covered = covered;
    updateExposed();
    queue_draw();
}

/*
 * The frames show while the widget is mapped, has its size and nothing
 * covers it.  GTK maps a window's widgets before it lays them out: exposed
 * at the map, the render thread made its EGL window at 1x1, and its first
 * frame - the only one while the guest's screen does not change - was a
 * pixel stretched over the view (a full-screen switch over a still screen
 * stayed black until the guest drew again).
 */
void DisplayWidget::updateExposed()
{
    setExposed(m_mapped && !m_covered && m_width > 0 && m_height > 0);
}

void DisplayWidget::setExposed(bool exposed)
{
    if (exposed == m_exposed) {
        return;
    }
    m_exposed = exposed;
    if (exposed) {
        m_surface->attachTo(toplevelSurface());
        place();
        /* GTK commits the window for this frame: the subsurface shows */
        queue_draw();
    } else {
        /* not shown: the render thread takes the guest's buffer off the
           surface, and the surface leaves the window */
        m_surface->detach();
    }
    if (m_renderer) {
        m_renderer->setExposed(exposed);
    }
    exposedChanged.emit(exposed);
}

void DisplayWidget::onLayout(GdkFrameClock *, gpointer self)
{
    static_cast<DisplayWidget *>(self)->place();
}

/* Where GTK put the widget, on the window's surface (its shadow aside):
   the subsurface goes there with the commit of the frame being made */
void DisplayWidget::place()
{
    GtkNative *native = gtk_widget_get_native(gobj());
    if (!native || !m_exposed) {
        return;
    }
    graphene_point_t origin = GRAPHENE_POINT_INIT(0, 0), out;
    if (!gtk_widget_compute_point(gobj(), GTK_WIDGET(native), &origin, &out)) {
        return;
    }
    double tx = 0, ty = 0;
    gtk_native_get_surface_transform(native, &tx, &ty);
    if (m_surface->setPosition(int(std::lround(out.x + tx)), int(std::lround(out.y + ty)))) {
        queue_draw();
    }
}

void DisplayWidget::updateSize()
{
    GdkSurface *surface = toplevelSurface();
    const double scale = surface ? gdk_surface_get_scale(surface) : 1;
    m_surface->setSize(m_width, m_height, scale);
    m_input.setView(m_surface->physicalSize(), scale);
    if (m_renderer) {
        m_renderer->requestRedraw();
    }
    scheduleUiInfo();
}

QSize DisplayWidget::physicalSize() const
{
    return m_surface->physicalSize();
}

/* Resize storms: the guest is told once they settle */
void DisplayWidget::scheduleUiInfo()
{
    if (m_opts.guestWidth) {
        return;
    }
    m_uiInfoTimer.disconnect();
    m_uiInfoTimer = Glib::signal_timeout().connect([this]() {
        sendUiInfo();
        return false;
    }, 100);
}

uint32_t DisplayWidget::refreshMilliHz() const
{
    if (m_opts.refreshOverride > 0) {
        return uint32_t(std::lround(m_opts.refreshOverride * 1000));
    }
    GdkSurface *surface = toplevelSurface();
    GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(
                                        gtk_widget_get_display(const_cast<GtkWidget *>(gobj())),
                                        surface)
                                  : nullptr;
    const int mhz = monitor ? gdk_monitor_get_refresh_rate(monitor) : 0;
    return mhz > 0 ? uint32_t(mhz) : 60000;
}

QSize DisplayWidget::physicalMm(GtkWidget *widget, const QSize &view)
{
    GtkNative *native = widget ? gtk_widget_get_native(widget) : nullptr;
    GdkSurface *surface = native ? gtk_native_get_surface(native) : nullptr;
    GdkMonitor *monitor =
        surface ? gdk_display_get_monitor_at_surface(gtk_widget_get_display(widget), surface)
                : nullptr;
    if (!monitor) {
        return {};
    }
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    const double scale = gdk_monitor_get_scale(monitor);
    return PhysicalSize::ofView(view,
                                QSize(int(std::lround(geometry.width * scale)),
                                      int(std::lround(geometry.height * scale))),
                                QSizeF(gdk_monitor_get_width_mm(monitor),
                                       gdk_monitor_get_height_mm(monitor)));
}

void DisplayWidget::sendUiInfo()
{
    const QSize size = m_opts.guestWidth ? QSize(m_opts.guestWidth, m_opts.guestHeight)
                                         : physicalSize();
    if (size.isEmpty()) {
        return;
    }
    /* the view's share of its monitor's physical size: the guest's EDID
       says it, and the guest picks its scale from it (not QEMU's 100 DPI) */
    const QSize mm = physicalMm(gobj(), physicalSize());
    m_display->applyUiInfo(uint32_t(size.width()), uint32_t(size.height()),
                           m_opts.sendRefreshRate ? refreshMilliHz() : 0, uint16_t(mm.width()),
                           uint16_t(mm.height()));
}

void DisplayWidget::setGuestSize(uint32_t width, uint32_t height)
{
    m_input.setGuestSize(QSize(int(width), int(height)));
}

void DisplayWidget::setGuestCursor(const QImage &image, int hotX, int hotY)
{
    if (image.isNull()) {
        return;
    }
    /* ARGB32 in memory order on a little-endian host: B, G, R, A */
    GBytes *bytes = g_bytes_new(image.constBits(), gsize(image.bytesPerLine()) * gsize(image.height()));
    GdkTexture *texture = gdk_memory_texture_new(image.width(), image.height(),
                                                 GDK_MEMORY_B8G8R8A8, bytes,
                                                 gsize(image.bytesPerLine()));
    g_bytes_unref(bytes);
    if (m_cursorTexture) {
        g_object_unref(m_cursorTexture);
    }
    m_cursorTexture = texture;
    m_cursorHotX = hotX;
    m_cursorHotY = hotY;
    updateCursor();
}

void DisplayWidget::setGuestCursorVisible(bool visible)
{
    m_input.setGuestCursorVisible(visible);
    updateCursor();
}

void DisplayWidget::updateCursor()
{
    if (m_input.cursorHidden()) {
        gtk_widget_set_cursor_from_name(gobj(), "none");
        return;
    }
    if (!m_cursorTexture) {
        gtk_widget_set_cursor(gobj(), nullptr);
        return;
    }
    auto *data = new GuestCursor{GDK_TEXTURE(g_object_ref(m_cursorTexture)), m_cursorHotX,
                                 m_cursorHotY};
    GdkCursor *cursor = gdk_cursor_new_from_callback(cursorTexture, data, [](gpointer p) {
        auto *c = static_cast<GuestCursor *>(p);
        g_object_unref(c->texture);
        delete c;
    }, nullptr);
    gtk_widget_set_cursor(gobj(), cursor);
    g_object_unref(cursor);
}

QString DisplayWidget::grabState() const
{
    QString state = m_input.grabState();
    if (m_inhibited && !m_inhibitActive) {
        state += QStringLiteral(", but the desktop keeps its shortcuts");
    }
    return state;
}

void DisplayWidget::setGrab(bool on)
{
    m_input.setGrab(on);
}

void DisplayWidget::setHostActive(bool active)
{
    m_input.setHostActive(active);
    holdWindowShortcuts(active);
}

/*
 * Every key to the guest while it has the keyboard, as the Qt view takes
 * them before the window's shortcuts (ShortcutOverride): GTK runs a
 * window's capture-phase shortcut controllers - the application's
 * accelerators, F10 for its primary menu, the shortcuts its widgets hand to
 * the window - before the focused widget sees the key.  Those of the
 * widget's window are off meanwhile, and back as it loses the keyboard
 * (input off, the focus elsewhere, the window not active); the bubble
 * phase's never get the keys the widget takes.
 */
void DisplayWidget::holdWindowShortcuts(bool hold)
{
    if (!hold) {
        for (GtkEventController *controller : std::exchange(m_heldShortcuts, {})) {
            gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
            g_object_unref(controller);
        }
        return;
    }
    if (!m_heldShortcuts.empty() || !m_root) {
        return;
    }
    GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(m_root->gobj()));
    const guint n = g_list_model_get_n_items(controllers);
    for (guint i = 0; i < n; i++) {
        auto *controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
        if (GTK_IS_SHORTCUT_CONTROLLER(controller) &&
            gtk_event_controller_get_propagation_phase(controller) == GTK_PHASE_CAPTURE) {
            m_heldShortcuts.push_back(controller);  // the item's reference
        } else {
            g_object_unref(controller);
        }
    }
    g_object_unref(controllers);
    for (GtkEventController *controller : m_heldShortcuts) {
        gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_NONE);
    }
}

void DisplayWidget::setFullScreenWindow(bool on)
{
    m_window->setWindowStates(on ? Qt::WindowFullScreen : Qt::WindowNoState);
}

bool DisplayWidget::onKeyPressed(guint keyval, guint keycode, Gdk::ModifierType state)
{
    inputSeen.emit();
    /* the app's own keys (the console's views), its release and repeats with it */
    if (!m_hotkeys.count(keycode) &&
        ViewKeys::take(keyval, keycode, static_cast<GdkModifierType>(state))) {
        m_hotkeys.insert(keycode);
        return true;
    }
    /* the hotkeys of QEMU's SDL and GTK displays: Ctrl+Alt+G grab, Ctrl+Alt+F
       full screen.  Ctrl and Alt still reach the guest, the letter does not */
    const auto mods = static_cast<GdkModifierType>(state);
    const DisplayInput::Hotkey hotkey = DisplayInput::hotkey(
        gdk_keyval_to_lower(keyval), mods & GDK_CONTROL_MASK, mods & GDK_ALT_MASK,
        mods & GDK_SHIFT_MASK, mods & GDK_SUPER_MASK);
    if (!m_inputEnabled) {
        /* the window's shortcuts (Resume among them), but full screen's way
           back: the full-screen window has none */
        if (hotkey == DisplayInput::Hotkey::FullScreen && m_hotkeys.insert(keycode).second) {
            fullScreenToggled.emit();
            return true;
        }
        return false;
    }
    if (hotkey != DisplayInput::Hotkey::None || m_hotkeys.count(keycode)) {
        if (m_hotkeys.insert(keycode).second) {   // not its repeat
            if (hotkey == DisplayInput::Hotkey::Grab) {
                setGrab(!grabbed());
            } else if (hotkey == DisplayInput::Hotkey::FullScreen) {
                fullScreenToggled.emit();
            }
        }
        return true;
    }
    /* Wayland gives XKB keycodes: evdev + 8 */
    m_input.key(keycode >= 8 ? keycode - 8 : 0, true);
    return true;
}

void DisplayWidget::onKeyReleased(guint, guint keycode, Gdk::ModifierType)
{
    if (m_hotkeys.erase(keycode) || !m_inputEnabled) {
        return;
    }
    m_input.key(keycode >= 8 ? keycode - 8 : 0, false);
}

bool DisplayWidget::onEvent(const Glib::RefPtr<const Gdk::Event> &event)
{
    auto *ev = const_cast<GdkEvent *>(event->gobj());
    const GdkEventType type = gdk_event_get_event_type(ev);
    if (type != GDK_BUTTON_PRESS && type != GDK_BUTTON_RELEASE) {
        return false;
    }
    uint32_t button;
    if (!qemuButton(gdk_button_event_get_button(ev), &button)) {
        return true;
    }
    if (type == GDK_BUTTON_RELEASE) {
        m_input.release(button);
        return true;
    }
    /* the event's position, from the window's surface to the widget */
    double x = 0, y = 0, tx = 0, ty = 0;
    gdk_event_get_position(ev, &x, &y);
    GtkNative *native = gtk_widget_get_native(gobj());
    gtk_native_get_surface_transform(native, &tx, &ty);
    graphene_point_t p = GRAPHENE_POINT_INIT(float(x - tx), float(y - ty)), out;
    if (!gtk_widget_compute_point(GTK_WIDGET(native), gobj(), &p, &out)) {
        out = p;
    }
    /* a click: the keys are the guest's */
    grab_focus();
    inputSeen.emit();
    m_input.press(button, out.x, out.y);
    return true;
}

bool DisplayWidget::onScroll(double dx, double dy)
{
    /* in 120ths of a notch, positive up and left as Qt's angle delta: a
       wheel's notches as they are, a touchpad's surface pixels as Qt takes
       them (QtWayland: 12 per pixel) */
    const double k =
        gtk_event_controller_scroll_get_unit(m_scroll->gobj()) == GDK_SCROLL_UNIT_WHEEL ? 120 : 12;
    m_input.scroll(-dx * k, -dy * k);
    return true;
}

/* DisplayInput::Sink */
void DisplayWidget::keyPress(uint32_t qnum)
{
    m_display->keyPress(qnum);
}

void DisplayWidget::keyRelease(uint32_t qnum)
{
    m_display->keyRelease(qnum);
}

void DisplayWidget::mouseAbs(uint32_t x, uint32_t y)
{
    m_display->mouseAbs(x, y);
}

void DisplayWidget::mouseRel(int dx, int dy)
{
    m_display->mouseRel(dx, dy);
}

void DisplayWidget::mouseButton(uint32_t button, bool down)
{
    m_display->mouseButton(button, down);
}

bool DisplayWidget::mouseIsAbsolute() const
{
    return m_display->mouseIsAbsolute();
}

/* DisplayInput::Compositor, on the window's main surface */
void DisplayWidget::setShortcutsInhibited(bool on)
{
    GdkSurface *surface = toplevelSurface();
    if (on == m_inhibited || !surface || !GDK_IS_TOPLEVEL(surface)) {
        return;
    }
    m_inhibited = on;
    /* the window's one inhibitor, which the nested desktop's view may hold too */
    ShortcutsInhibit::set(surface, this, on);
    /* the state line says so until the compositor grants it */
    grabChanged.emit();
}

void DisplayWidget::setPointerLocked(bool on)
{
    if (m_constraints) {
        m_constraints->setLocked(toplevelSurface(), on);
    }
}

void DisplayWidget::setPointerConfined(bool on)
{
    if (m_constraints) {
        m_constraints->setConfined(toplevelSurface(), on);
    }
}
