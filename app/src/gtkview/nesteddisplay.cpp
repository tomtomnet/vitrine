// SPDX-License-Identifier: GPL-2.0-or-later
#include "nesteddisplay.h"

#include "focuswatch.h"
#include "fullscreenorder.h"

#include "nested/vmview.h"
#include "shortcutsinhibit.h"
#include "viewkeys.h"
#include "widgetrelease.h"

#include <QFile>

#include <gtkmm/application.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerlegacy.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace {

class GuestDesktop;

/*
 * Where the guest's window shows: in the console, with a word over black
 * where the guest's window is not, or alone in the full-screen window.
 * The guest's window is not GTK's: it hangs from a surface of vitrine's
 * over the widget (GuestDesktop's), which takes no input, so the pointer
 * over it goes to the guest and beside it to the widget, whose click gives
 * the guest the keys.  The keys themselves reach both the guest (the
 * library reads the window's keyboard) and GTK: the widget takes them,
 * while the guest has them, before vitrine's shortcuts and widgets do.
 */
class DesktopWidget : public Gtk::Widget
{
public:
    explicit DesktopWidget(GuestDesktop *owner);
    ~DesktopWidget() override;

    void setMessage(const QString &message);
    /* Something GTK draws covers it: the guest's window leaves */
    void setCovered(bool covered);
    /* Mapped and not covered: the guest's window may show over it */
    bool isExposed() const { return m_exposed; }
    /* Its window's surface */
    GdkSurface *toplevelSurface() const;
    /* Where GTK put it on its window's surface, in logical pixels */
    bool position(int *x, int *y) const;
    /* The window's shortcut controllers off while the guest has the keys */
    void holdWindowShortcuts(bool hold);
    /* The focus in the active window, as of GTK's last change of either
       (FocusWatch: not from inside a focus change) */
    bool hasKeyboard() const { return m_focus.hasKeyboard(); }

protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation, int forSize, int &minimum, int &natural,
                       int &minimumBaseline, int &naturalBaseline) const override;
    void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot> &snapshot) override;
    void on_realize() override;
    void on_unrealize() override;
    void on_map() override;
    void on_unmap() override;

private:
    bool onKeyPressed(guint keyval, guint keycode, Gdk::ModifierType state);
    void onKeyReleased(guint keyval, guint keycode, Gdk::ModifierType state);
    bool onEvent(const Glib::RefPtr<const Gdk::Event> &event);
    static void onLayout(GdkFrameClock *clock, gpointer self);
    void updateExposed();

    GuestDesktop *m_owner;
    QString m_message;
    bool m_mapped = false;
    bool m_covered = false;
    bool m_exposed = false;
    /* the keys down: their repeats are not new presses */
    std::set<guint> m_down;
    std::vector<GtkEventController *> m_heldShortcuts;
    GdkFrameClock *m_clock = nullptr;
    gulong m_layoutId = 0;
    Gtk::Window *m_root = nullptr;
    FocusWatch m_focus;
};

class GuestDesktop : public NestedDisplay
{
public:
    GuestDesktop();
    ~GuestDesktop() override;
    bool init(const QString &socket, QString *error);

    QString socket() const override { return m_socket; }
    GtkWidget *widget() const override { return GTK_WIDGET(m_embedded->gobj()); }
    bool isFullScreen() const override { return m_fullWindow; }
    void setFullScreen(bool on) override;
    void setInputEnabled(bool on) override;
    bool hasKeyboard() const override { return m_keyboard; }
    void focus() override;
    int connections() const override { return m_connections; }
    bool isMapped() const override { return m_mapped; }
    void setCovered(bool on) override;

    /* the widgets' */
    bool inputEnabled() const { return m_inputEnabled; }
    void exposedChanged(DesktopWidget *widget);
    void laidOut(DesktopWidget *widget);
    void focusChanged();
    void fullScreenKey();

private:
    /* Where the guest's window goes: the full-screen window's, else the console's */
    DesktopWidget *host() const { return m_fullWidget ? m_fullWidget : m_embedded; }
    /* The surface the guest's window hangs from, on @toplevel's, shown */
    void attach(GdkSurface *toplevel);
    /* Hidden: no longer on a window */
    void detach();
    /* The surface where GTK put the host widget, the guest's window at its size */
    void place(bool always = false);
    /* The surface's commit, for the guest's window's place to apply */
    void commitSurface();
    void updateMessage();
    void updateKeyboard();
    /* The host's shortcuts to the guest while it asks for them and has the keys */
    void updateShortcuts();
    /* @fn on the GUI thread, from the library's, unless the view is gone by then */
    void later(std::function<void(GuestDesktop *)> fn);

    vmview *m_view = nullptr;
    QString m_socket;
    wl_display *m_display = nullptr;
    wl_subcompositor *m_subcompositor = nullptr;
    wl_shm *m_shm = nullptr;
    wl_surface *m_surface = nullptr;        // the library's parent
    wl_buffer *m_pixel = nullptr;           // its buffer
    wl_subsurface *m_subsurface = nullptr;  // on m_parent's, while shown
    GdkSurface *m_parent = nullptr;
    int m_x = 0, m_y = 0;
    bool m_placed = false;
    int m_width = 0, m_height = 0;
    bool m_sentFullScreen = false;
    bool m_geometrySent = false;
    sigc::connection m_commitAgain;

    DesktopWidget *m_embedded = nullptr;
    Gtk::Window *m_fullWindow = nullptr;
    FullScreenOrder m_fullScreenOrder;
    DesktopWidget *m_fullWidget = nullptr;
    bool m_inputEnabled = true;
    bool m_covered = false;
    bool m_keyboard = false;
    bool m_guestShortcuts = false;          // the guest asks for them (the library's shortcuts)
    GdkSurface *m_inhibited = nullptr;      // the toplevel they are inhibited on
    int m_connections = 0;
    bool m_mapped = false;
    QString m_title;
    /* what the library's thread queued for this one: done only while it lives */
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::weak_ptr<bool> m_weak = m_alive;
};

struct Globals {
    wl_subcompositor *subcompositor = nullptr;
    wl_shm *shm = nullptr;
};

const wl_registry_listener kRegistry = {
    [](void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
        auto *g = static_cast<Globals *>(data);
        if (!strcmp(iface, wl_subcompositor_interface.name)) {
            g->subcompositor = static_cast<wl_subcompositor *>(
                wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
        } else if (!strcmp(iface, wl_shm_interface.name)) {
            g->shm = static_cast<wl_shm *>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

/* ---- DesktopWidget ---- */

DesktopWidget::DesktopWidget(GuestDesktop *owner)
    : Glib::ObjectBase("VitrineDesktop"), m_owner(owner),
      m_focus(GTK_WIDGET(gobj()), [this]() { m_owner->focusChanged(); })
{
    set_focusable(true);
    set_hexpand(true);
    set_vexpand(true);

    /* no input method: keys a compositor routes through one come late */
    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(sigc::mem_fun(*this, &DesktopWidget::onKeyPressed), false);
    key->signal_key_released().connect(sigc::mem_fun(*this, &DesktopWidget::onKeyReleased));
    add_controller(key);

    /* the keys down go with the focus (whether the guest still has the keys
       is the FocusWatch's to say, once GTK stored the change) */
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_leave().connect([this]() { m_down.clear(); });
    add_controller(focus);

    /* a click beside the guest's window: the keys to it */
    auto legacy = Gtk::EventControllerLegacy::create();
    legacy->signal_event().connect(sigc::mem_fun(*this, &DesktopWidget::onEvent), false);
    add_controller(legacy);
}

DesktopWidget::~DesktopWidget()
{
    holdWindowShortcuts(false);
    /* GTK may keep the widget past this (the window's focus): no event to this object any more */
    WidgetRelease::dropControllers(GTK_WIDGET(gobj()));
}

void DesktopWidget::setMessage(const QString &message)
{
    if (message != m_message) {
        m_message = message;
        queue_draw();
    }
}

void DesktopWidget::setCovered(bool covered)
{
    m_covered = covered;
    updateExposed();
}

void DesktopWidget::updateExposed()
{
    const bool exposed = m_mapped && !m_covered;
    if (exposed != m_exposed) {
        m_exposed = exposed;
        m_owner->exposedChanged(this);
        queue_draw();
    }
}

GdkSurface *DesktopWidget::toplevelSurface() const
{
    GtkNative *native = gtk_widget_get_native(const_cast<GtkWidget *>(gobj()));
    return native ? gtk_native_get_surface(native) : nullptr;
}

bool DesktopWidget::position(int *x, int *y) const
{
    GtkNative *native = gtk_widget_get_native(const_cast<GtkWidget *>(gobj()));
    if (!native) {
        return false;
    }
    graphene_point_t origin = GRAPHENE_POINT_INIT(0, 0), out;
    if (!gtk_widget_compute_point(const_cast<GtkWidget *>(gobj()), GTK_WIDGET(native), &origin,
                                  &out)) {
        return false;
    }
    /* the window's surface has its shadow around what GTK lays out */
    double tx = 0, ty = 0;
    gtk_native_get_surface_transform(native, &tx, &ty);
    *x = int(std::lround(out.x + tx));
    *y = int(std::lround(out.y + ty));
    return true;
}

/*
 * Every key to the guest while it has the keyboard, as the D-Bus display's
 * widget takes them (DisplayWidget): GTK runs a window's capture-phase
 * shortcut controllers - the application's accelerators, F10 for its
 * primary menu - before the focused widget sees the key.  Those of the
 * widget's window are off meanwhile, and back as the guest loses the keys.
 */
void DesktopWidget::holdWindowShortcuts(bool hold)
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

Gtk::SizeRequestMode DesktopWidget::get_request_mode_vfunc() const
{
    return Gtk::SizeRequestMode::CONSTANT_SIZE;
}

void DesktopWidget::measure_vfunc(Gtk::Orientation orientation, int, int &minimum, int &natural,
                                  int &minimumBaseline, int &naturalBaseline) const
{
    minimum = natural = orientation == Gtk::Orientation::HORIZONTAL ? 160 : 100;
    minimumBaseline = naturalBaseline = -1;
}

void DesktopWidget::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot> &snapshot)
{
    const int width = get_width(), height = get_height();
    const GdkRGBA black = {0, 0, 0, 1};
    const graphene_rect_t all = GRAPHENE_RECT_INIT(0, 0, float(width), float(height));

    /* under the guest's window, and where it is not */
    gtk_snapshot_append_color(snapshot->gobj(), &black, &all);
    if (m_message.isEmpty() || width <= 48) {
        return;
    }
    PangoLayout *layout = gtk_widget_create_pango_layout(gobj(), m_message.toUtf8().constData());
    pango_layout_set_width(layout, (width - 48) * PANGO_SCALE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    int textWidth = 0, textHeight = 0;
    pango_layout_get_pixel_size(layout, &textWidth, &textHeight);
    const graphene_point_t at = GRAPHENE_POINT_INIT(24.f, float(std::max(0, (height - textHeight) / 2)));
    const GdkRGBA grey = {0xc8 / 255.f, 0xcd / 255.f, 0xd2 / 255.f, 1};
    gtk_snapshot_save(snapshot->gobj());
    gtk_snapshot_translate(snapshot->gobj(), &at);
    gtk_snapshot_append_layout(snapshot->gobj(), layout, &grey);
    gtk_snapshot_restore(snapshot->gobj());
    g_object_unref(layout);
}

void DesktopWidget::on_realize()
{
    Gtk::Widget::on_realize();
    /* after GTK's own layout of the window, before its paint and commit */
    m_clock = GDK_FRAME_CLOCK(g_object_ref(gtk_widget_get_frame_clock(gobj())));
    m_layoutId = g_signal_connect_after(m_clock, "layout", G_CALLBACK(onLayout), this);
    m_root = dynamic_cast<Gtk::Window *>(get_root());
}

void DesktopWidget::on_unrealize()
{
    /* the window's shortcuts back while the window is still there */
    holdWindowShortcuts(false);
    m_root = nullptr;
    if (m_clock) {
        g_signal_handler_disconnect(m_clock, m_layoutId);
        g_object_unref(std::exchange(m_clock, nullptr));
    }
    Gtk::Widget::on_unrealize();
}

void DesktopWidget::on_map()
{
    Gtk::Widget::on_map();
    m_mapped = true;
    updateExposed();
}

void DesktopWidget::on_unmap()
{
    m_mapped = false;
    updateExposed();
    Gtk::Widget::on_unmap();
}

void DesktopWidget::onLayout(GdkFrameClock *, gpointer self)
{
    auto *widget = static_cast<DesktopWidget *>(self);
    widget->m_owner->laidOut(widget);
}

/* Ctrl+Alt+F, as on the VM's screen */
static bool isFullScreenKey(guint keyval, Gdk::ModifierType state)
{
    const auto mods = static_cast<GdkModifierType>(state) &
                      (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK);
    return mods == (GDK_CONTROL_MASK | GDK_ALT_MASK) && gdk_keyval_to_lower(keyval) == GDK_KEY_f;
}

bool DesktopWidget::onKeyPressed(guint keyval, guint keycode, Gdk::ModifierType state)
{
    /* off (the VM paused): vitrine's shortcuts, Resume among them */
    if (!m_owner->inputEnabled()) {
        return false;
    }
    const bool repeat = !m_down.insert(keycode).second;
    /* the app's own keys (the console's views), in the full-screen window too */
    if (!repeat && ViewKeys::take(keyval, keycode, static_cast<GdkModifierType>(state))) {
        return true;
    }
    /* the guest has the key, through the library; Ctrl+Alt+F its own once its window is there */
    if (!repeat && isFullScreenKey(keyval, state)) {
        m_owner->fullScreenKey();
    }
    return true;
}

void DesktopWidget::onKeyReleased(guint, guint keycode, Gdk::ModifierType)
{
    m_down.erase(keycode);
}

bool DesktopWidget::onEvent(const Glib::RefPtr<const Gdk::Event> &event)
{
    if (gdk_event_get_event_type(const_cast<GdkEvent *>(event->gobj())) == GDK_BUTTON_PRESS) {
        grab_focus();
    }
    return false;
}

/* ---- GuestDesktop ---- */

GuestDesktop::GuestDesktop() = default;

bool GuestDesktop::init(const QString &socket, QString *error)
{
    GdkDisplay *gdk = gdk_display_get_default();

    m_socket = socket;
    if (!gdk || !GDK_IS_WAYLAND_DISPLAY(gdk)) {
        *error = "The nested desktop shows in Vitrine's window on a Wayland desktop only";
        return false;
    }
    m_display = gdk_wayland_display_get_wl_display(gdk);
    wl_compositor *compositor = gdk_wayland_display_get_wl_compositor(gdk);
    if (!m_display || !compositor) {
        *error = "The nested desktop shows in Vitrine's window on a Wayland desktop only";
        return false;
    }
    /* bound on a queue of their own, then given to the default one, which
       GDK dispatches (what they make has no events vitrine reads) */
    Globals g;
    wl_event_queue *queue = wl_display_create_queue(m_display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *reg = wl_display_get_registry(wrapped);
    wl_registry_add_listener(reg, &kRegistry, &g);
    wl_display_roundtrip_queue(m_display, queue);
    for (auto *p : {reinterpret_cast<wl_proxy *>(g.subcompositor), reinterpret_cast<wl_proxy *>(g.shm)}) {
        if (p) {
            wl_proxy_set_queue(p, nullptr);
        }
    }
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    m_subcompositor = g.subcompositor;
    m_shm = g.shm;
    if (!m_subcompositor || !m_shm) {
        *error = "The compositor lacks what the nested desktop needs (wl_subcompositor, wl_shm)";
        return false;
    }

    /* the guest's window's parent: one transparent pixel, no input */
    m_surface = wl_compositor_create_surface(compositor);
    wl_region *none = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(m_surface, none);
    wl_region_destroy(none);
    const int fd = memfd_create("vitrine-nested-desktop", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 4) < 0) {
        *error = QString("memfd: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    wl_shm_pool *pool = wl_shm_create_pool(m_shm, fd, 4);
    m_pixel = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(m_surface, m_pixel, 0, 0);
    wl_surface_damage(m_surface, 0, 0, 1, 1);
    wl_surface_commit(m_surface);
    wl_display_flush(m_display);

    /* the library's thread: its callbacks to this one */
    vmview_callbacks cb{};
    cb.data = this;
    cb.title = [](void *data, const char *title) {
        static_cast<GuestDesktop *>(data)->later([text = QString::fromUtf8(title)](GuestDesktop *self) {
            self->m_title = text;
            if (self->m_fullWindow) {
                self->m_fullWindow->set_title(text.toStdString());
            }
        });
    };
    cb.fullscreen = [](void *data, bool on) {
        static_cast<GuestDesktop *>(data)->later([on](GuestDesktop *self) {
            self->fullScreenRequested.emit(on);
        });
    };
    cb.connection = [](void *data, int count) {
        static_cast<GuestDesktop *>(data)->later([count](GuestDesktop *self) {
            if (count != self->m_connections) {
                self->m_connections = count;
                self->connectionsChanged.emit(count);
            }
        });
    };
    cb.mapped = [](void *data, bool mapped) {
        static_cast<GuestDesktop *>(data)->later([mapped](GuestDesktop *self) {
            if (mapped != self->m_mapped) {
                self->m_mapped = mapped;
                self->updateMessage();
                self->place(true);
                self->mappedChanged.emit(mapped);
            }
        });
    };
    /* a click on the guest's window, which GTK never sees: the keys to it */
    cb.pressed = [](void *data) {
        static_cast<GuestDesktop *>(data)->later([](GuestDesktop *self) { self->focus(); });
    };
    cb.shortcuts = [](void *data, bool captured) {
        static_cast<GuestDesktop *>(data)->later([captured](GuestDesktop *self) {
            self->m_guestShortcuts = captured;
            self->updateShortcuts();
        });
    };
    m_view = vmview_new(m_display, m_surface, QFile::encodeName(socket).constData(), &cb);
    if (!m_view) {
        *error = QString("Cannot serve the nested desktop's socket %1: %2")
                     .arg(socket, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    m_embedded = new DesktopWidget(this);
    updateMessage();
    /* the library gives the guest the keys of the window by default: none until it has the focus */
    updateKeyboard();
    return true;
}

GuestDesktop::~GuestDesktop()
{
    m_fullScreenOrder.cancel();
    /* first: the guest's objects on the surface go, and its callbacks stop */
    vmview_destroy(std::exchange(m_view, nullptr));
    m_alive.reset();
    m_commitAgain.disconnect();
    m_guestShortcuts = false;
    m_keyboard = false;
    updateShortcuts();
    detach();
    if (m_fullWindow) {
        m_fullWindow->unset_child();
        delete std::exchange(m_fullWidget, nullptr);
        delete std::exchange(m_fullWindow, nullptr);
    }
    /* out of the console by now (VmConsole) */
    delete std::exchange(m_embedded, nullptr);
    if (m_surface) {
        wl_surface_destroy(m_surface);
    }
    if (m_pixel) {
        wl_buffer_destroy(m_pixel);
    }
    if (m_shm) {
        wl_shm_destroy(m_shm);
    }
    if (m_subcompositor) {
        wl_subcompositor_destroy(m_subcompositor);
    }
    if (m_display) {
        wl_display_flush(m_display);
    }
}

void GuestDesktop::later(std::function<void(GuestDesktop *)> fn)
{
    struct Call {
        std::weak_ptr<bool> alive;
        GuestDesktop *self;
        std::function<void(GuestDesktop *)> fn;
    };
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto *call = static_cast<Call *>(data);
        if (!call->alive.expired()) {
            call->fn(call->self);
        }
        return G_SOURCE_REMOVE;
    }, new Call{m_weak, this, std::move(fn)}, [](gpointer data) { delete static_cast<Call *>(data); });
}

void GuestDesktop::exposedChanged(DesktopWidget *widget)
{
    if (widget != host()) {
        return;
    }
    if (widget->isExposed()) {
        attach(widget->toplevelSurface());
    } else {
        detach();
    }
}

void GuestDesktop::laidOut(DesktopWidget *widget)
{
    if (widget == host()) {
        place();
    }
}

void GuestDesktop::attach(GdkSurface *toplevel)
{
    wl_surface *parent = toplevel ? gdk_wayland_surface_get_wl_surface(toplevel) : nullptr;

    if (!parent || (toplevel == m_parent && m_subsurface)) {
        return;
    }
    detach();
    m_parent = GDK_SURFACE(g_object_ref(toplevel));
    m_subsurface = wl_subcompositor_get_subsurface(m_subcompositor, m_surface, parent);
    /* the guest's frames show as they come, not with the window's commits */
    wl_subsurface_set_desync(m_subsurface);
    m_placed = false;
    place(true);
    commitSurface();
    /* a new subsurface, like its position, counts from its parent's next
       commit: GTK's, which it makes for its next frame */
    gdk_wayland_surface_force_next_commit(toplevel);
    host()->queue_draw();
}

void GuestDesktop::detach()
{
    if (m_subsurface) {
        /* unmapped at once, the guest's window with it */
        wl_subsurface_destroy(m_subsurface);
        m_subsurface = nullptr;
        wl_display_flush(m_display);
    }
    if (m_parent) {
        g_object_unref(std::exchange(m_parent, nullptr));
    }
}

void GuestDesktop::place(bool always)
{
    DesktopWidget *widget = host();
    int x = 0, y = 0;

    if (m_subsurface && widget->position(&x, &y) && (!m_placed || x != m_x || y != m_y)) {
        m_placed = true;
        m_x = x;
        m_y = y;
        wl_subsurface_set_position(m_subsurface, x, y);
        wl_display_flush(m_display);
        gdk_wayland_surface_force_next_commit(m_parent);
        widget->queue_draw();
    }
    /* the guest's window at the view's size, told it is full screen if so;
       not before GTK gave the view a size (a full-screen window just made) */
    const int width = widget->get_width();
    const int height = widget->get_height();
    const bool full = m_fullWindow;
    if (!m_view || width <= 0 || height <= 0 ||
        (!always && m_geometrySent && width == m_width && height == m_height &&
         full == m_sentFullScreen)) {
        return;
    }
    m_geometrySent = true;
    m_width = width;
    m_height = height;
    m_sentFullScreen = full;
    vmview_set_fullscreen(m_view, full);
    vmview_set_geometry(m_view, 0, 0, width, height);
    commitSurface();
    /* the library's thread places the guest's window after this: once more then */
    m_commitAgain.disconnect();
    m_commitAgain = Glib::signal_timeout().connect([this]() {
        commitSurface();
        return false;
    }, 50);
}

void GuestDesktop::commitSurface()
{
    if (m_surface) {
        wl_surface_commit(m_surface);
        wl_display_flush(m_display);
    }
}

void GuestDesktop::updateMessage()
{
    m_embedded->setMessage(
        m_fullWindow ? QString("The guest's desktop is in full screen. Ctrl+Alt+F brings it back "
                               "here.")
        : m_mapped   ? QString()
                     : QString("The guest's desktop is not running.\nLog in at the guest's login "
                               "screen, in the Screen view (Ctrl+Alt+1)."));
}

void GuestDesktop::setFullScreen(bool on)
{
    if (!on) {
        /* a full screen still waiting for its turn does not come */
        m_fullScreenOrder.cancel();
    }
    if (on == isFullScreen()) {
        return;
    }
    /* the full-screen window maps after the window the view leaves, or that
       window, mapped after it, takes the focus and covers it (FullScreenOrder) */
    if (on && !m_fullScreenOrder.ready(dynamic_cast<Gtk::Window *>(m_embedded->get_root()),
                                       [this]() { setFullScreen(true); })) {
        return;
    }
    detach();
    if (on) {
        m_fullWidget = new DesktopWidget(this);
        m_fullWindow = new Gtk::Window;
        m_fullWindow->set_title(m_title.isEmpty() ? std::string("Desktop") : m_title.toStdString());
        if (auto *top = dynamic_cast<Gtk::Window *>(m_embedded->get_root())) {
            if (auto app = top->get_application()) {
                m_fullWindow->set_application(app);
            }
        }
        m_fullWindow->set_child(*m_fullWidget);
        /* closed by the desktop, e.g. from the overview: back in the console */
        const std::weak_ptr<bool> alive = m_weak;
        m_fullWindow->signal_close_request().connect([this, alive]() {
            Glib::signal_idle().connect_once([this, alive]() {
                if (!alive.expired()) {
                    setFullScreen(false);
                }
            });
            return true;
        }, false);
        m_fullWindow->property_is_active().signal_changed().connect([this]() { updateKeyboard(); });
        m_fullWindow->fullscreen();
        m_fullWindow->present();
        m_fullWidget->grab_focus();
    } else {
        /* the shortcuts off its window before the window goes */
        const bool keyboard = std::exchange(m_keyboard, false);
        updateShortcuts();
        m_keyboard = keyboard;
        Gtk::Window *full = std::exchange(m_fullWindow, nullptr);
        DesktopWidget *widget = std::exchange(m_fullWidget, nullptr);
        full->unset_child();
        delete widget;
        delete full;
        if (m_embedded->isExposed()) {
            attach(m_embedded->toplevelSurface());
        }
        focus();
    }
    updateMessage();
    place(true);
    updateKeyboard();
    fullScreenChanged.emit(on);
}

void GuestDesktop::setInputEnabled(bool on)
{
    if (on != m_inputEnabled) {
        m_inputEnabled = on;
        updateKeyboard();
    }
}

void GuestDesktop::focus()
{
    if (m_fullWindow) {
        m_fullWindow->present();
        m_fullWidget->grab_focus();
    } else {
        m_embedded->grab_focus();
    }
}

void GuestDesktop::setCovered(bool on)
{
    if (on != m_covered) {
        m_covered = on;
        m_embedded->setCovered(on);
    }
}

void GuestDesktop::focusChanged()
{
    updateKeyboard();
}

void GuestDesktop::fullScreenKey()
{
    /* the guest's window takes its Ctrl+Alt+F itself (its session asks) */
    if (!m_mapped) {
        fullScreenToggled.emit();
    }
}

/* The keys to the guest while its view has GTK's focus in the active window, or is full screen and active */
void GuestDesktop::updateKeyboard()
{
    bool want = false;

    if (m_inputEnabled && m_fullWindow) {
        want = m_fullWindow->is_active();
    } else if (m_inputEnabled) {
        want = m_embedded->hasKeyboard();
    }
    if (m_view) {
        vmview_set_keyboard_focus(m_view, want);
    }
    m_embedded->holdWindowShortcuts(want && !m_fullWindow);
    if (m_fullWidget) {
        m_fullWidget->holdWindowShortcuts(want && m_fullWindow);
    }
    if (want != m_keyboard) {
        m_keyboard = want;
        keyboardChanged.emit();
    }
    updateShortcuts();
}

/*
 * The guest's capture on the window that has the keyboard: the full-screen
 * one, else vitrine's.  The library puts the guest's inhibitor on its parent
 * surface, which never has the keyboard focus, so the compositor would
 * apply none (Meta reached both desktops).
 */
void GuestDesktop::updateShortcuts()
{
    /* not on GNOME for an app it does not know: it would ask the user at every
       new window (ShortcutsInhibit::automatic()) */
    GdkSurface *top = m_keyboard && m_guestShortcuts && ShortcutsInhibit::automatic()
                          ? host()->toplevelSurface()
                          : nullptr;

    if (top == m_inhibited) {
        return;
    }
    if (m_inhibited) {
        ShortcutsInhibit::set(m_inhibited, this, false);
        g_object_unref(std::exchange(m_inhibited, nullptr));
    }
    if (top) {
        m_inhibited = GDK_SURFACE(g_object_ref(top));
        ShortcutsInhibit::set(top, this, true);
    }
}

} // namespace

NestedDisplay *NestedDisplay::create(const QString &socket, QString *error)
{
    auto *display = new GuestDesktop;
    if (!display->init(socket, error)) {
        delete display;
        return nullptr;
    }
    return display;
}
