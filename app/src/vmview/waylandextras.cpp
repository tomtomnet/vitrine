#include "waylandextras.h"

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <wayland-client.h>
#include <cstring>

namespace {
const wl_registry_listener kRegistry = {
    [](void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
        auto *self = static_cast<WaylandExtras *>(data);
        if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
            self->m_inhibitManager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1 *>(
                wl_registry_bind(reg, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
        } else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) {
            self->m_constraints = static_cast<zwp_pointer_constraints_v1 *>(
                wl_registry_bind(reg, name, &zwp_pointer_constraints_v1_interface, 1));
        } else if (!strcmp(iface, zwp_relative_pointer_manager_v1_interface.name)) {
            self->m_relativeManager = static_cast<zwp_relative_pointer_manager_v1 *>(
                wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

const zwp_keyboard_shortcuts_inhibitor_v1_listener kInhibitor = {
    [](void *data, zwp_keyboard_shortcuts_inhibitor_v1 *) {
        static_cast<WaylandExtras *>(data)->onInhibit(true);
    },
    [](void *data, zwp_keyboard_shortcuts_inhibitor_v1 *) {
        static_cast<WaylandExtras *>(data)->onInhibit(false);
    },
};

const zwp_locked_pointer_v1_listener kLocked = {
    [](void *data, zwp_locked_pointer_v1 *) { static_cast<WaylandExtras *>(data)->onLocked(true); },
    [](void *data, zwp_locked_pointer_v1 *) { static_cast<WaylandExtras *>(data)->onLocked(false); },
};

const zwp_relative_pointer_v1_listener kRelative = {
    [](void *data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t, wl_fixed_t,
       wl_fixed_t dxUnaccel, wl_fixed_t dyUnaccel) {
        // unaccelerated deltas: the guest applies its own acceleration
        static_cast<WaylandExtras *>(data)->onRelative(wl_fixed_to_double(dxUnaccel),
                                                       wl_fixed_to_double(dyUnaccel));
    },
};
} // namespace

WaylandExtras::WaylandExtras(QObject *parent) : QObject(parent) {}

WaylandExtras::~WaylandExtras()
{
    if (m_relative) {
        zwp_relative_pointer_v1_destroy(m_relative);
    }
    if (m_locked) {
        zwp_locked_pointer_v1_destroy(m_locked);
    }
    if (m_inhibitor) {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_inhibitor);
    }
}

bool WaylandExtras::init()
{
    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!app) {
        return false; // not a Wayland session
    }
    m_display = app->display();
    m_seat = app->seat();
    m_pointer = app->pointer();
    // Bind on a private queue, then hand the globals to the default queue,
    // which Qt dispatches on the GUI thread.
    wl_event_queue *queue = wl_display_create_queue(m_display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *reg = wl_display_get_registry(wrapped);
    wl_registry_add_listener(reg, &kRegistry, this);
    wl_display_roundtrip_queue(m_display, queue);
    for (auto *p : {reinterpret_cast<wl_proxy *>(m_inhibitManager),
                    reinterpret_cast<wl_proxy *>(m_constraints),
                    reinterpret_cast<wl_proxy *>(m_relativeManager)}) {
        if (p) {
            wl_proxy_set_queue(p, nullptr);
        }
    }
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    return true;
}

wl_surface *WaylandExtras::surfaceOf(QWindow *window)
{
    QPlatformNativeInterface *ni = QGuiApplication::platformNativeInterface();
    if (!ni || !window || !window->handle()) {
        return nullptr;
    }
    return static_cast<wl_surface *>(ni->nativeResourceForWindow("surface", window));
}

bool WaylandExtras::setShortcutsInhibited(QWindow *topLevel, bool on)
{
    if (!on) {
        if (m_inhibitor) {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_inhibitor);
            m_inhibitor = nullptr;
            onInhibit(false);
        }
        return true;
    }
    wl_surface *surface = surfaceOf(topLevel);
    if (!m_inhibitManager || !surface || !m_seat || m_inhibitor) {
        return m_inhibitor;
    }
    m_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m_inhibitManager,
                                                                              surface, m_seat);
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(m_inhibitor, &kInhibitor, this);
    wl_display_flush(m_display);
    return true;
}

bool WaylandExtras::setPointerLocked(QWindow *window, bool on)
{
    if (!on) {
        if (m_relative) {
            zwp_relative_pointer_v1_destroy(m_relative);
            m_relative = nullptr;
        }
        if (m_locked) {
            zwp_locked_pointer_v1_destroy(m_locked);
            m_locked = nullptr;
            onLocked(false);
        }
        return true;
    }
    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    m_pointer = app ? app->pointer() : m_pointer;
    wl_surface *surface = surfaceOf(window);
    if (!hasPointerLock() || !surface || !m_pointer || m_locked) {
        return m_locked;
    }
    m_locked = zwp_pointer_constraints_v1_lock_pointer(
        m_constraints, surface, m_pointer, nullptr,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(m_locked, &kLocked, this);
    m_relative = zwp_relative_pointer_manager_v1_get_relative_pointer(m_relativeManager, m_pointer);
    zwp_relative_pointer_v1_add_listener(m_relative, &kRelative, this);
    wl_display_flush(m_display);
    return true;
}
