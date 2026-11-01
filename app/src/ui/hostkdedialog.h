// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "core/hostkde.h"

class QWidget;

/*
 * What Vitrine says before it installs one of its host packages (KWin,
 * KSvg, Kirigami, Qt base), or puts the distribution's back, then the
 * install or revert itself (HostKdeMonitor: install.sh with pkexec, the
 * desktop's password dialog).  Plain, and nothing left out: what it
 * replaces, as root; that it is version-locked and what the lock holds
 * back; when it must be built again (KWin: Plasma and Qt updates; KSvg and
 * Kirigami: the KDE Frameworks'; Qt base holds every Qt update); that
 * reverting means the distribution's packages; when it takes effect.  A
 * package that is not Vitrine's gets a warning of its own, naming it,
 * before it is replaced.
 */
namespace HostKdeDialog {

/* "kwin, kwin-common and kwin-libs": those of @packages, in words */
QString names(const QStringList &packages);
/* The packages an install replaces: the source package's installed ones */
QStringList replaced(const HostKde::Status &status);

QString installText(const HostKde::Status &status, const QStringList &packages);
/* The same for several packages installed at once (Install All…) */
QString installAllText(const QList<HostKde::Status> &statuses);
/* The warning before a package that is not Vitrine's goes; empty if it is not one */
QString foreignText(const HostKde::Status &status);
QString revertText(const QList<HostKde::Status> &statuses);
/* The commands that put the distribution's packages back from a text console */
QString consoleCommands(const QStringList &packages);

/* Asks, then starts the install of @id's build for this computer; false if
   it did not start (not possible, or not wanted) */
bool install(QWidget *parent, const QString &id);
/* The same for those of @ids that can be installed, at once (one password, one
   transaction): the Host desktop group's Install All… */
bool installAll(QWidget *parent, const QStringList &ids);
/* The same for a revert of @ids, at once (one password) */
bool revert(QWidget *parent, const QStringList &ids);

}
