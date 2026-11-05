// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QString>
#include <QStringList>

#include <functional>

/*
 * What the GTK app asks before it installs one of Vitrine's host packages
 * (KWin, KSvg, Kirigami, Qt base) or puts the distribution's back: the Qt
 * app's HostKdeDialog questions, with their texts (ui/hostkdetext.h), as
 * alerts; then the install or revert itself (HostKdeMonitor: install.sh
 * with pkexec, the desktop's password dialog).  A package that is not
 * Vitrine's gets a warning of its own, naming it, before it is replaced.
 * What cannot be done now is said, with why.
 */
namespace HostKdeAlerts {

/* How an alert is shown: over the window that asks */
using Present = std::function<void(AdwDialog *dialog)>;

/* Asks, then starts the install of @id's build for this computer */
void install(const Present &present, const QString &id);
/* The same for those of @ids that can be installed, at once (one password, one
   transaction): the Host Desktop group's Install All… */
void installAll(const Present &present, const QStringList &ids);
/* The same for a revert of @ids, at once (one password) */
void revert(const Present &present, const QStringList &ids);

/* The alert of @heading with the rich text @html; the commands of its <pre> part apart, to copy */
AdwDialog *alert(const QString &heading, const QString &html);

}
