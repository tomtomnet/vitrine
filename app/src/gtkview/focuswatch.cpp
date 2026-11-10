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
    /* the window's activity at once (above): only what is sure in a crossing */
    m_controller = gtk_event_controller_focus_new();
    g_object_ref(m_controller);
    g_signal_connect_swapped(m_controller, "enter", G_CALLBACK(+[](FocusWatch *self) {
        if (self->now()) {
            self->set(true);
        }
    }), this);
    g_signal_connect_swapped(m_controller, "leave", G_CALLBACK(+[](FocusWatch *self) {
        self->set(self->now());
    }), this);
    gtk_widget_add_controller(widget, m_controller);
    if (gtk_widget_get_realized(widget)) {
        watch(windowOf(widget));
    }
}

FocusWatch::~FocusWatch()
{
    g_signal_handler_disconnect(m_widget, m_realizeId);
    g_signal_handler_disconnect(m_widget, m_unrealizeId);
    g_signal_handlers_disconnect_by_data(m_controller, this);
    /* not if the view dropped it already (WidgetRelease::dropControllers) */
    if (gtk_event_controller_get_widget(m_controller) == m_widget) {
        gtk_widget_remove_controller(m_widget, m_controller);
    }
    g_object_unref(m_controller);
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
            m_focusId = g_signal_connect_swapped(
                window, "notify::focus-widget",
                G_CALLBACK(+[](FocusWatch *self) { self->set(self->now()); }), this);
            m_activeId = g_signal_connect_swapped(
                window, "notify::is-active",
                G_CALLBACK(+[](FocusWatch *self) { self->set(self->now()); }), this);
        }
    }
    set(now());
}

bool FocusWatch::now() const
{
    return m_window && gtk_widget_is_focus(m_widget) && gtk_window_is_active(m_window);
}

void FocusWatch::set(bool keyboard)
{
    if (keyboard != m_keyboard) {
        m_keyboard = keyboard;
        if (m_changed) {
            m_changed();
        }
    }
}
