// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* GTK's first: Qt's keywords are macros, which sigc++'s names would meet
   (src/gtk/gtkbase.h) */
#include <sigc++/connection.h>

#include <functional>

namespace Gtk {
class Window;
}

/*
 * A view's full-screen window maps after the window the view leaves, never
 * before.  A compositor focuses and raises a window as it maps it - GNOME's
 * Mutter does so with every new window that comes without startup
 * information, which GTK gives none for a program's other windows - so of
 * two windows a program shows at once, the one mapped last ends on top with
 * the keys.  A program started in full screen made both at once, and its
 * full-screen window, quicker to draw, mapped first: the main window, mapped
 * 180 ms later, covered it and kept the keyboard (vitrine-gtk-view
 * --full-screen on GNOME Shell 51, 2026-11-05).
 *
 * So the switch to full screen waits while the window the view is in is
 * shown but not active yet (the compositor has not mapped and focused it),
 * until it is, or for two seconds at most: it may never be (another
 * window kept the focus), and full screen was asked for.  A window not
 * shown, or already active, has nothing to wait for.
 */
class FullScreenOrder
{
public:
    FullScreenOrder() = default;
    /* Cancels */
    ~FullScreenOrder();
    FullScreenOrder(const FullScreenOrder &) = delete;
    FullScreenOrder &operator=(const FullScreenOrder &) = delete;

    /*
     * Whether the full-screen window may be made now, @from being the window
     * the view is in (null: none).  If not, @retry runs later, from an idle
     * of the main loop, once @from is active or the wait is over, and this
     * then says yes; while a wait is under way, it keeps its own retry.
     */
    bool ready(Gtk::Window *from, std::function<void()> retry);
    bool waiting() const { return m_waiting; }
    /* The wait ends without its retry: full screen was asked off meanwhile */
    void cancel();

    /* The longest wait, in milliseconds (tests) */
    static void setTimeout(unsigned ms);

private:
    void go();

    bool m_waiting = false;
    bool m_go = false;
    std::function<void()> m_retry;
    sigc::connection m_active, m_timeout, m_idle;
};
