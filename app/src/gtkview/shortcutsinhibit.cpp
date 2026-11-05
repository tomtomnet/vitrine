// SPDX-License-Identifier: GPL-2.0-or-later
#include "shortcutsinhibit.h"

#include <gdk/gdk.h>

#include <set>

namespace {

const char kOwners[] = "vitrine-shortcuts-inhibit";

using Owners = std::set<const void *>;

}

namespace ShortcutsInhibit {

void set(GdkSurface *toplevel, const void *owner, bool on)
{
    /* VITRINE_VIEW_INHIBIT=0: never ask for the inhibitor, for scripted runs on
       GNOME, which answers a window it knows no .desktop file for with its
       "allow" dialog at every request, and that dialog takes the keyboard.  The
       keys still go to the guest while its view has the focus; the grab's state
       says the desktop keeps its shortcuts. */
    static const bool never = g_strcmp0(g_getenv("VITRINE_VIEW_INHIBIT"), "0") == 0;
    if (never || !toplevel || !GDK_IS_TOPLEVEL(toplevel)) {
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
    }
    const bool before = !owners->empty();
    if (on) {
        owners->insert(owner);
    } else {
        owners->erase(owner);
    }
    const bool after = !owners->empty();
    if (after && !before) {
        gdk_toplevel_inhibit_system_shortcuts(GDK_TOPLEVEL(toplevel), nullptr);
    } else if (before && !after) {
        gdk_toplevel_restore_system_shortcuts(GDK_TOPLEVEL(toplevel));
    }
}

}
