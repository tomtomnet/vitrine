// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <gtk/gtk.h>

#include <vector>

/*
 * The views' widgets are C++ objects of gtkmm whose event controllers call
 * lambdas holding the object.  GTK may keep the C widget past the C++
 * object: the window holds its focus widget, and moves the focus away at
 * its next frame, emitting the old focus widget's controllers' signals -
 * into a deleted object (vitrine-gtk crashed so as a VM shut down while the
 * guest's desktop had the keys, 2026-11-05).  sigc disconnects the member
 * slots of a trackable object as it goes, never a lambda.
 */
namespace WidgetRelease {

/* Before @widget leaves its window: the focus off it, at once, while its
   C++ side is still there to take the focus-out */
inline void unfocus(GtkWidget *widget)
{
    GtkRoot *root = widget ? gtk_widget_get_root(widget) : nullptr;
    GtkWidget *focus = root ? gtk_root_get_focus(root) : nullptr;

    if (focus && (focus == widget || gtk_widget_is_ancestor(focus, widget))) {
        gtk_root_set_focus(root, nullptr);
    }
}

/* As @widget's C++ side goes: its controllers off, so that no event reaches it any more */
inline void dropControllers(GtkWidget *widget)
{
    std::vector<GtkEventController *> controllers;
    GListModel *list = gtk_widget_observe_controllers(widget);

    for (guint i = 0, n = g_list_model_get_n_items(list); i < n; i++) {
        controllers.push_back(GTK_EVENT_CONTROLLER(g_list_model_get_item(list, i)));
    }
    g_object_unref(list);
    for (GtkEventController *controller : controllers) {
        gtk_widget_remove_controller(widget, controller);
        g_object_unref(controller);
    }
}

}
