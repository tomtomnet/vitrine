// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * GTK's headers, first in every file of the GTK app.  Qt's keywords are
 * macros (signals, slots, emit, foreach), and GIO, sigc++ and gtkmm use
 * those words as names: a header of theirs read after Qt's breaks.  Qt's
 * and the core's headers come after these, never before; and the GTK
 * app's own code says neither emit() of sigc++ (its signals are called
 * with operator()) nor Qt's keywords where GTK's headers might follow.
 */
#if defined(signals) || defined(slots) || defined(emit) || defined(foreach)
#error "gtk/gtkbase.h comes before Qt's headers: GTK's headers use Qt's keywords as names"
#endif

#include <adwaita.h>
#include <gtkmm.h>
/* for objects made in C, as libadwaita's are: not in gtkmm.h */
#include <gtkmm/init.h>
