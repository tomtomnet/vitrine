// SPDX-License-Identifier: GPL-2.0-or-later
#include "shortcutsinhibit.h"

#include "displayinput.h"

#include <gdk/gdk.h>
#include <gio/gdesktopappinfo.h>

#include <cstring>
#include <set>

namespace {

const char kOwners[] = "vitrine-shortcuts-inhibit";

using Owners = std::set<const void *>;

/* VITRINE_VIEW_INHIBIT=0: never ask (scripted runs on GNOME) */
bool never()
{
    static const bool never = g_strcmp0(g_getenv("VITRINE_VIEW_INHIBIT"), "0") == 0;
    return never;
}

}

namespace ShortcutsInhibit {

void set(GdkSurface *toplevel, const void *owner, bool on)
{
    /* VITRINE_VIEW_INHIBIT=0: never ask for the inhibitor, for scripted runs on
       GNOME, which answers a window it knows no .desktop file for with its
       "allow" dialog at every request, and that dialog takes the keyboard.  The
       keys still go to the guest while its view has the focus; the grab's state
       says the desktop keeps its shortcuts. */
    if (never() || !toplevel || !GDK_IS_TOPLEVEL(toplevel)) {
        return;
    }
    auto *owners = static_cast<Owners *>(g_object_get_data(G_OBJECT(toplevel), kOwners));
    if (!owners) {
        if (!on) {
            return;
        }
        owners = new Owners;
        g_object_set_data_full(G_OBJECT(toplevel), kOwners, owners,
                               [](gpointer p) { delete static_cast<Owners *>(p); });
        /* the compositor's answer, once for the window (vitrine.grab) */
        g_signal_connect(toplevel, "notify::shortcuts-inhibited",
                         G_CALLBACK(+[](GObject *surface, GParamSpec *, gpointer) {
            gboolean on = FALSE;
            g_object_get(surface, "shortcuts-inhibited", &on, nullptr);
            qCDebug(lcGrab) << "the compositor" << (on ? "inhibits" : "restores")
                            << "the desktop's shortcuts";
        }), nullptr);
    }
    const bool before = !owners->empty();
    if (on) {
        owners->insert(owner);
    } else {
        owners->erase(owner);
    }
    const bool after = !owners->empty();
    if (after && !before) {
        qCDebug(lcGrab) << "shortcuts inhibitor asked for";
        gdk_toplevel_inhibit_system_shortcuts(GDK_TOPLEVEL(toplevel), nullptr);
        /* now, not with the window's next frame: GDK flushes its connection
           once the main loop waits, after the frame GTK may be rendering for
           the same change (the window's activation repaint), and the
           desktop's shortcuts stay until the compositor reads the request -
           7.5 ms after a click that brought the window back, on GNOME */
        gdk_display_flush(gdk_surface_get_display(toplevel));
    } else if (before && !after) {
        qCDebug(lcGrab) << "shortcuts inhibitor dropped";
        gdk_toplevel_restore_system_shortcuts(GDK_TOPLEVEL(toplevel));
    }
}

bool asks()
{
    return !never();
}

bool automaticFor(const char *desktop, bool known)
{
    if (known || !desktop) {
        return true;
    }
    /* GNOME, GNOME Classic ("GNOME-Classic:GNOME"), Ubuntu's ("ubuntu:GNOME") */
    gchar **names = g_strsplit(desktop, ":", -1);
    const bool gnome = g_strv_contains(names, "GNOME");
    g_strfreev(names);
    return !gnome;
}

bool automatic()
{
    /* never asking (VITRINE_VIEW_INHIBIT=0), the grab behaves as before: it asks nothing */
    static const bool automatic = [] {
        if (never()) {
            return true;
        }
        GDesktopAppInfo *info = g_desktop_app_info_new("org.vitrine.Vitrine.desktop");
        const bool known = info;
        if (info) {
            g_object_unref(info);
        }
        return automaticFor(g_getenv("XDG_CURRENT_DESKTOP"), known);
    }();
    return automatic;
}

}
