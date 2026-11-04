// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <gdk/gdk.h>

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

}
