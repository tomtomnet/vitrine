// SPDX-License-Identifier: GPL-2.0-or-later
#include "focuswatch.h"

#include <gtk/gtk.h>

#include <utility>

namespace {

GtkWindow *windowOf(GtkWidget *widget)
{
    GtkRoot *root = gtk_widget_get_root(widget);
    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr;
}

} // namespace

FocusWatch::FocusWatch(GtkWidget *widget, std::function<void()> changed)
    : m_widget(widget), m_changed(std::move(changed))
{
    /* "realize" runs its class handler first: the widget is on its window by
       then; "unrealize" last: the window is still there */
    m_realizeId = g_signal_connect_swapped(widget, "realize", G_CALLBACK(+[](FocusWatch *self) {
        self->watch(windowOf(self->m_widget));
    }), this);
    m_unrealizeId = g_signal_connect_swapped(widget, "unrealize", G_CALLBACK(+[](FocusWatch *self) {
        self->watch(nullptr);
    }), this);
    if (gtk_widget_get_realized(widget)) {
        watch(windowOf(widget));
    }
}

FocusWatch::~FocusWatch()
{
    g_signal_handler_disconnect(m_widget, m_realizeId);
    g_signal_handler_disconnect(m_widget, m_unrealizeId);
    if (m_window) {
        g_signal_handler_disconnect(m_window, m_focusId);
        g_signal_handler_disconnect(m_window, m_activeId);
        g_object_remove_weak_pointer(G_OBJECT(m_window), reinterpret_cast<gpointer *>(&m_window));
    }
}

void FocusWatch::watch(GtkWindow *window)
{
    if (window != m_window) {
        if (m_window) {
            g_signal_handler_disconnect(m_window, m_focusId);
            g_signal_handler_disconnect(m_window, m_activeId);
            g_object_remove_weak_pointer(G_OBJECT(m_window), reinterpret_cast<gpointer *>(&m_window));
        }
        m_window = window;
        if (window) {
            g_object_add_weak_pointer(G_OBJECT(window), reinterpret_cast<gpointer *>(&m_window));
            /* both notified once GTK stored the change */
            m_focusId = g_signal_connect_swapped(window, "notify::focus-widget",
                                                 G_CALLBACK(+[](FocusWatch *self) { self->update(); }),
                                                 this);
            m_activeId = g_signal_connect_swapped(window, "notify::is-active",
                                                  G_CALLBACK(+[](FocusWatch *self) { self->update(); }),
                                                  this);
        }
    }
    update();
}

void FocusWatch::update()
{
    const bool keyboard = m_window && gtk_widget_is_focus(m_widget) && gtk_window_is_active(m_window);

    if (keyboard != m_keyboard) {
        m_keyboard = keyboard;
        if (m_changed) {
            m_changed();
        }
    }
}
