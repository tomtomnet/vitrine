// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

/*
 * The window of a running VM, which belongs to QEMU.  An app may not bring
 * the window of another to the front on Wayland; KWin, the window manager
 * of KDE Plasma, runs a script that does, so it takes that.
 */
namespace VmWindow {

/* Brings the window of process @pid to the front, unminimized: false,
   with @error, where the desktop does not allow it */
bool raise(qint64 pid, QString *error);

}
