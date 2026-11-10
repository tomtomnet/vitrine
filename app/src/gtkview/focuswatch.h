// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>

typedef struct _GtkWidget GtkWidget;
typedef struct _GtkWindow GtkWindow;

/*
 * Whether a widget has the keyboard: it is the focus of its window, and the
 * window is active.  Known once GTK has made a change, as the window's
 * focus-widget and is-active are notified, never from inside the change:
 * GTK 4's gtk_window_root_set_focus() clears the window's focus, sends the
 * focus controllers' leave and enter, and only then stores the new focus,
 * so in the new focus's "enter" gtk_widget_is_focus() is still false.  The
 * VM's screen read it there: given the focus by the list of VMs, or back
 * from the Details view, it took no grab until the window was deactivated
 * and active again (GNOME's overview, opened by the first Super, did that:
 * the next Super reached the guest).
 *
 * It follows the window the widget is realized in.  hasKeyboard() holds
 * the value as of the last notification: stable while GTK is between the
 * crossings and the change.
 */
class FocusWatch
{
public:
    /* @changed: after hasKeyboard() changed */
    FocusWatch(GtkWidget *widget, std::function<void()> changed);
    ~FocusWatch();
    FocusWatch(const FocusWatch &) = delete;
    FocusWatch &operator=(const FocusWatch &) = delete;

    bool hasKeyboard() const { return m_keyboard; }

private:
    void watch(GtkWindow *window);
    void update();

    GtkWidget *m_widget;
    std::function<void()> m_changed;
    GtkWindow *m_window = nullptr;      // weak: the widget's, while it is realized
    unsigned long m_realizeId = 0, m_unrealizeId = 0, m_focusId = 0, m_activeId = 0;
    bool m_keyboard = false;
};
