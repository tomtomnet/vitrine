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
/* Whether set() asks at all: not with VITRINE_VIEW_INHIBIT=0 (scripted runs) */
bool asks();

/*
 * Whether the views may ask for the inhibitor on their own - the screen's
 * automatic grab (an absolute pointer over it with the keyboard), the
 * nested desktop as its session asks - or only on the user's Ctrl+Alt+G.
 * GNOME Shell answers each request with its "allow" dialog, which takes
 * the keyboard and the pointer from the VM; it stores the answer for an
 * app it knows by its desktop file (one dialog in the app's life, for all
 * its windows), but for a window it cannot match ("window-backed": vitrine
 * not installed) Mutter keeps the answer for that surface only, so the
 * dialog came at every run and every full-screen window, unasked for.
 */
bool automatic();
/* That rule: @desktop is XDG_CURRENT_DESKTOP (a list split by ':'), @known
   whether GLib finds org.vitrine.Vitrine.desktop */
bool automaticFor(const char *desktop, bool known);

}
