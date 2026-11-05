// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include "core/hostkde.h"
#include "ui/hostkdetext.h"

class QWidget;

/*
 * The questions Vitrine asks before it installs one of its host packages
 * (KWin, KSvg, Kirigami, Qt base), or puts the distribution's back, with
 * the texts of ui/hostkdetext.h, then the install or revert itself
 * (HostKdeMonitor: install.sh with pkexec, the desktop's password dialog).
 * A package that is not Vitrine's gets a warning of its own, naming it,
 * before it is replaced.
 */
namespace HostKdeDialog {

/* Asks, then starts the install of @id's build for this computer; false if
   it did not start (not possible, or not wanted) */
bool install(QWidget *parent, const QString &id);
/* The same for those of @ids that can be installed, at once (one password, one
   transaction): the Host desktop group's Install All… */
bool installAll(QWidget *parent, const QStringList &ids);
/* The same for a revert of @ids, at once (one password) */
bool revert(QWidget *parent, const QStringList &ids);

}
