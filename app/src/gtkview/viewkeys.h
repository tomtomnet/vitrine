// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <gdk/gdk.h>

#include <cstdio>
#include <functional>
#include <utility>

/*
 * The app's own keys on the VM's screens, before the guest gets them: the
 * console's Ctrl+Alt+1/2 (its views), in vitrine's window and in the
 * full-screen windows alike, whose key controllers the app does not own.
 * The views (DisplayWidget, NestedDisplay) ask the filter at each key
 * press they would give the guest; a key it takes, its release too, stays
 * the app's.
 */
namespace ViewKeys {

/* @keyval and @keycode as GDK gives them (XKB's keycode: evdev's + 8); true: taken */
using Filter = std::function<bool(guint keyval, guint keycode, GdkModifierType state)>;

inline Filter &filter()
{
    static Filter f;
    return f;
}

inline void setFilter(Filter f)
{
    filter() = std::move(f);
}

inline bool take(guint keyval, guint keycode, GdkModifierType state)
{
    return filter() && filter()(keyval, keycode, state);
}

/*
 * Where the views' keys go: to the guest only while the view has the
 * keyboard and the pointer is on it, or a grab holds (src/gtkview/README.md).
 * VITRINE_VIEW_KEYS=focus keeps the rule before 2026-11-11 for scripted runs
 * whose pointer may not be on the view (the research's key benches): every
 * key goes to the guest while the view has the keyboard.  Said once on
 * stderr when set.
 */
inline bool followFocus()
{
    static const bool focus = [] {
        const bool on = g_strcmp0(g_getenv("VITRINE_VIEW_KEYS"), "focus") == 0;
        if (on) {
            fprintf(stderr, "vitrine: display: keys: to the guest whenever the view has the keyboard, "
                            "wherever the pointer is (VITRINE_VIEW_KEYS=focus)\n");
        }
        return on;
    }();
    return focus;
}

}
