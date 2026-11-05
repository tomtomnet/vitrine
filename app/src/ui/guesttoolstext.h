// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

#include "core/guesttools.h"

class Vm;

/*
 * What Install Guest Tools says of a VM, in both apps: whom the tools are
 * for and what the guest runs, what the medium installs and how, and what
 * keeps the tools from the VM now.  Rich text; Qt Core only.
 */
namespace GuestToolsText {

/*
 * A build under way makes the medium of Fedora @release again (the Builds
 * window remakes it after any of its packages): installing now would put
 * in the tools it replaces
 */
bool mediumBeingBuilt(const QString &release);
/* The release whose tools @vm gets (GuestToolsMonitor::release()), and its medium if built */
QString toolsRelease(Vm *vm);
GuestTools::Medium mediumOf(Vm *vm);
/* What obstacle() says while the medium is not built: the build banner tells it */
QString notBuilt();
/* Why the tools cannot go into @vm now; empty if they can */
QString obstacle(Vm *vm, const GuestTools::Medium &medium);
/* What the tools of @medium install in @vm, and how (rich text) */
QString what(Vm *vm, const GuestTools::Medium &medium);

}
