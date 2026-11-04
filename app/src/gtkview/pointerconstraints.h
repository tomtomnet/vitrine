// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>

struct wl_display;
struct wl_pointer;
struct zwp_confined_pointer_v1;
struct zwp_locked_pointer_v1;
struct zwp_pointer_constraints_v1;
struct zwp_relative_pointer_manager_v1;
struct zwp_relative_pointer_v1;
typedef struct _GdkDisplay GdkDisplay;
typedef struct _GdkSurface GdkSurface;

/*
 * The pointer grab GTK has no API for, on its Wayland connection: the
 * pointer locked with relative motion (guests without an absolute pointer),
 * or confined (KWin's Meta + drag kept for the guest), on the window's
 * main surface - KWin honours constraints there only, not on a subsurface
 * (src/vmview/README.md).  Its objects are on the default queue, which GDK
 * dispatches: the callbacks come on the GUI thread.  The keyboard's grab
 * is GDK's own (gdk_toplevel_inhibit_system_shortcuts).
 */
class PointerConstraints
{
public:
    /* Null when @display is not Wayland or lacks the protocols */
    static std::unique_ptr<PointerConstraints> create(GdkDisplay *display);
    ~PointerConstraints();

    std::function<void(double dx, double dy)> relativeMotion; // unaccelerated
    std::function<void(bool active)> lockActive;

    bool setLocked(GdkSurface *toplevel, bool on);
    bool setConfined(GdkSurface *toplevel, bool on);
    bool canLock() const { return m_constraints && m_relativeManager; }

    /* protocol callbacks */
    void onRelative(double dx, double dy);
    void onLocked(bool active);

private:
    PointerConstraints() = default;
    wl_pointer *pointer() const;

    GdkDisplay *m_gdkDisplay = nullptr;
    wl_display *m_display = nullptr;
    zwp_pointer_constraints_v1 *m_constraints = nullptr;
    zwp_relative_pointer_manager_v1 *m_relativeManager = nullptr;
    zwp_locked_pointer_v1 *m_locked = nullptr;
    zwp_confined_pointer_v1 *m_confined = nullptr;
    zwp_relative_pointer_v1 *m_relative = nullptr;
};
