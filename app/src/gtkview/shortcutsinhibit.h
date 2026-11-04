// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

typedef struct _GdkSurface GdkSurface;

/*
 * The desktop's shortcuts (Alt+Tab, Super...) to the guest: GDK's one
 * inhibitor of a toplevel (gdk_toplevel_inhibit_system_shortcuts()), shared
 * by the views on it - the VM's screen (DisplayWidget's grab) and the
 * guest's nested desktop (NestedDisplay, while its session asks for them) -
 * as src/vmview's WaylandExtras shares one per surface in the Qt app.  It
 * stays while one of them at least wants it: GDK's restore takes the
 * inhibitor away whoever asked for it.
 */
namespace ShortcutsInhibit {

/* @owner wants the inhibitor on @toplevel, or no longer */
void set(GdkSurface *toplevel, const void *owner, bool on);

}
