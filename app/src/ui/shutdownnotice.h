// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

#include "core/guestshutdown.h"

class QStatusBar;
class Vm;

/*
 * What Shut Down does, in the status bar, as the VM's runner tries each way
 * (GuestShutdown): through an agent the guest shuts down by itself; after
 * the power button it may ask what to do on its screen, as Plasma does with
 * its logout screen, and only an answer there shuts it down.
 */
namespace ShutdownNotice {

/* What the status bar says when @name is asked to shut down @way; empty for None */
QString text(const QString &name, GuestShutdown::Way way);
/* How long it says it, in ms */
int timeout(GuestShutdown::Way way);
/* Says it in @bar for each way @vm's runner tries, and takes it back once
   the request is over: the guest shuts down, or the VM stopped */
void follow(Vm *vm, QStatusBar *bar);

}
