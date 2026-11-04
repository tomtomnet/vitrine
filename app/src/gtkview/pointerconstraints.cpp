// SPDX-License-Identifier: GPL-2.0-or-later
#include "pointerconstraints.h"

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include <cstring>

namespace {

struct Globals {
    zwp_pointer_constraints_v1 *constraints = nullptr;
    zwp_relative_pointer_manager_v1 *relative = nullptr;
};

const wl_registry_listener kRegistry = {
    [](void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
        auto *g = static_cast<Globals *>(data);
        if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) {
            g->constraints = static_cast<zwp_pointer_constraints_v1 *>(
                wl_registry_bind(reg, name, &zwp_pointer_constraints_v1_interface, 1));
        } else if (!strcmp(iface, zwp_relative_pointer_manager_v1_interface.name)) {
            g->relative = static_cast<zwp_relative_pointer_manager_v1 *>(
                wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

const zwp_locked_pointer_v1_listener kLocked = {
    [](void *data, zwp_locked_pointer_v1 *) { static_cast<PointerConstraints *>(data)->onLocked(true); },
    [](void *data, zwp_locked_pointer_v1 *) { static_cast<PointerConstraints *>(data)->onLocked(false); },
};

const zwp_confined_pointer_v1_listener kConfined = {
    [](void *, zwp_confined_pointer_v1 *) {},
    [](void *, zwp_confined_pointer_v1 *) {},
};

const zwp_relative_pointer_v1_listener kRelative = {
    [](void *data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t, wl_fixed_t,
       wl_fixed_t dxUnaccel, wl_fixed_t dyUnaccel) {
        /* unaccelerated: the guest applies its own acceleration */
        static_cast<PointerConstraints *>(data)->onRelative(wl_fixed_to_double(dxUnaccel),
                                                            wl_fixed_to_double(dyUnaccel));
    },
};

} // namespace

std::unique_ptr<PointerConstraints> PointerConstraints::create(GdkDisplay *display)
{
    if (!GDK_IS_WAYLAND_DISPLAY(display)) {
        return nullptr;
    }
    std::unique_ptr<PointerConstraints> self(new PointerConstraints);
    self->m_gdkDisplay = display;
    self->m_display = gdk_wayland_display_get_wl_display(display);
    /* bound on a queue of their own, then given to the default one, which
       GDK dispatches on the GUI thread */
    Globals g;
    wl_event_queue *queue = wl_display_create_queue(self->m_display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(self->m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *reg = wl_display_get_registry(wrapped);
    wl_registry_add_listener(reg, &kRegistry, &g);
    wl_display_roundtrip_queue(self->m_display, queue);
    for (auto *p : {reinterpret_cast<wl_proxy *>(g.constraints),
                    reinterpret_cast<wl_proxy *>(g.relative)}) {
        if (p) {
            wl_proxy_set_queue(p, nullptr);
        }
    }
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    self->m_constraints = g.constraints;
    self->m_relativeManager = g.relative;
    if (!self->m_constraints) {
        return nullptr;
    }
    return self;
}

PointerConstraints::~PointerConstraints()
{
    if (m_relative) {
        zwp_relative_pointer_v1_destroy(m_relative);
    }
    if (m_locked) {
        zwp_locked_pointer_v1_destroy(m_locked);
    }
    if (m_confined) {
        zwp_confined_pointer_v1_destroy(m_confined);
    }
    if (m_relativeManager) {
        zwp_relative_pointer_manager_v1_destroy(m_relativeManager);
    }
    if (m_constraints) {
        zwp_pointer_constraints_v1_destroy(m_constraints);
    }
    wl_display_flush(m_display);
}

wl_pointer *PointerConstraints::pointer() const
{
    GdkSeat *seat = gdk_display_get_default_seat(m_gdkDisplay);
    GdkDevice *device = seat ? gdk_seat_get_pointer(seat) : nullptr;
    return device ? gdk_wayland_device_get_wl_pointer(device) : nullptr;
}

void PointerConstraints::onRelative(double dx, double dy)
{
    if (relativeMotion) {
        relativeMotion(dx, dy);
    }
}

void PointerConstraints::onLocked(bool active)
{
    if (lockActive) {
        lockActive(active);
    }
}

bool PointerConstraints::setLocked(GdkSurface *toplevel, bool on)
{
    if (!on) {
        if (m_relative) {
            zwp_relative_pointer_v1_destroy(m_relative);
            m_relative = nullptr;
        }
        if (m_locked) {
            zwp_locked_pointer_v1_destroy(m_locked);
            m_locked = nullptr;
            wl_display_flush(m_display);
            onLocked(false);
        }
        return true;
    }
    wl_surface *surface = toplevel ? gdk_wayland_surface_get_wl_surface(toplevel) : nullptr;
    wl_pointer *ptr = pointer();
    if (!canLock() || !surface || !ptr || m_locked || m_confined) {
        return m_locked;
    }
    m_locked = zwp_pointer_constraints_v1_lock_pointer(
        m_constraints, surface, ptr, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(m_locked, &kLocked, this);
    m_relative = zwp_relative_pointer_manager_v1_get_relative_pointer(m_relativeManager, ptr);
    zwp_relative_pointer_v1_add_listener(m_relative, &kRelative, this);
    wl_display_flush(m_display);
    return true;
}

bool PointerConstraints::setConfined(GdkSurface *toplevel, bool on)
{
    if (!on) {
        if (m_confined) {
            zwp_confined_pointer_v1_destroy(m_confined);
            m_confined = nullptr;
            wl_display_flush(m_display);
        }
        return true;
    }
    wl_surface *surface = toplevel ? gdk_wayland_surface_get_wl_surface(toplevel) : nullptr;
    wl_pointer *ptr = pointer();
    if (!m_constraints || !surface || !ptr || m_confined || m_locked) {
        return m_confined;
    }
    /* no region: the surface's input region, the whole window */
    m_confined = zwp_pointer_constraints_v1_confine_pointer(
        m_constraints, surface, ptr, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_confined_pointer_v1_add_listener(m_confined, &kConfined, this);
    wl_display_flush(m_display); // before the button press the user is about to make
    return true;
}
