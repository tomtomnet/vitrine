// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include "core/hostkwin.h"

class QWidget;

/*
 * What Vitrine says before it installs its KWin on the host, or puts the
 * distribution's back, then the install or revert itself
 * (HostKwinMonitor: install.sh with pkexec, the desktop's password dialog).
 * Plain, and nothing left out: it replaces the compositor as root, it must
 * match Plasma and Qt, it is version-locked and must be built again after
 * their updates, reverting means the distribution's packages, and it takes
 * effect at the next login.  A KWin that is not Vitrine's gets a warning of
 * its own, naming it, before it is replaced.
 */
namespace HostKwinDialog {

/* "kwin, kwin-common and kwin-libs": those of @packages, in words */
QString names(const QStringList &packages);
/* The packages an install replaces: the build's that are installed */
QStringList replaced(const HostKwin::Status &status, const HostKwin::System &system);

QString installText(const HostKwin::Status &status, const QStringList &packages);
/* The warning before a KWin that is not Vitrine's goes; empty if it is not one */
QString foreignText(const HostKwin::Status &status);
QString revertText(const QStringList &packages);
/* The commands that put the distribution's KWin back from a text console */
QString consoleCommands(const QStringList &packages);

/* Asks, then starts the install of the build for this Plasma; false if it
   did not start (not possible, or not wanted) */
bool install(QWidget *parent);
/* The same for a revert */
bool revert(QWidget *parent);

}
