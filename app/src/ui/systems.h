// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QIcon>
#include <QString>
#include <QStringList>

#include "core/guestos.h"
#include "core/vmconfig.h"

/*
 * The system a VM runs, as the windows show it: its icon in the list of
 * VMs, on the console and the Details, and its name.
 *
 * The icon is the theme's distributor logo when it has one
 * (distributor-logo-NAME, or a distribution's own, such as Fedora's
 * fedora-logo-icon), else a logo of app/data/os (its README: sources and
 * licenses) in white on a square of the brand's colour, or a letter where
 * there is no logo to bundle (Fedora), drawn at the size and pixel ratio
 * asked; the family's for a system without one of its own (Tux, Windows);
 * the theme's computer for the others and while a VM's system is not
 * known.
 */
namespace Systems {

/* The logo of @os: fedora, windows, linux...; of @family (#guest's) for a
   null @os; empty for the computer */
QString key(const GuestOs::Os &os, const QString &family = {});
/* The names looked up in the icon theme for @key, in order */
QStringList themeNames(const QString &key);

QIcon icon(const GuestOs::Os &os, const QString &family = {});
/* For a VM's #guest directive */
QIcon icon(const VmConfig::Guest &guest);
/* The icons looked up again in the theme, after it changed: until then a
   logo has the same icon, which the list's badges are made from once */
void reloadIcons();

/* "Fedora Linux 44", or its family: "Linux"; empty when not known */
QString name(const VmConfig::Guest &guest);
/* kde: "KDE Plasma"; empty for none */
QString desktopName(const QString &desktop);

}
