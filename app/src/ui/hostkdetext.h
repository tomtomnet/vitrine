// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "core/hostkde.h"

/*
 * What Vitrine says before it installs one of its host packages (KWin,
 * KSvg, Kirigami, Qt base), or puts the distribution's back: plain, and
 * nothing left out: what it replaces, as root; that it is version-locked
 * and what the lock holds back; when it must be built again (KWin: Plasma
 * and Qt updates; KSvg and Kirigami: the KDE Frameworks'; Qt base holds
 * every Qt update); that reverting means the distribution's packages; when
 * it takes effect.  A package that is not Vitrine's gets a warning of its
 * own, naming it, before it is replaced.  Rich text, for both apps'
 * questions (HostKdeDialog's in the Qt app); Qt Core only.
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

}
