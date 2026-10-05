// SPDX-License-Identifier: GPL-2.0-or-later
#include "shutdownnotice.h"

#include <QPointer>
#include <QStatusBar>

#include "core/vmrunner.h"
#include "core/vmstore.h"

namespace ShutdownNotice {

QString text(const QString &name, GuestShutdown::Way way)
{
    switch (way) {
    case GuestShutdown::Way::None:
        break;
    case GuestShutdown::Way::ToolsAgent:
        return QObject::tr("Asked %1 to shut down through its guest tools").arg(name);
    case GuestShutdown::Way::GuestAgent:
        return QObject::tr("Asked %1 to shut down through its guest agent").arg(name);
    case GuestShutdown::Way::PowerButton:
        return QObject::tr("Pressed the power button of %1. If the guest asks what to do, "
                           "answer it on its screen.")
            .arg(name);
    }
    return {};
}

int timeout(GuestShutdown::Way way)
{
    /* the question may wait on the guest's screen a while */
    return way == GuestShutdown::Way::PowerButton ? 120000 : 10000;
}

void follow(Vm *vm, QStatusBar *bar)
{
    QObject::connect(vm->runner(), &VmRunner::shutdownWayChanged, bar,
                     [vm = QPointer<Vm>(vm), bar](GuestShutdown::Way way) {
        if (!vm) {
            return;
        }
        if (way != GuestShutdown::Way::None) {
            bar->showMessage(text(vm->name(), way), timeout(way));
            return;
        }
        /* over: what was said of this request goes, not what came after it */
        for (const auto said : {GuestShutdown::Way::ToolsAgent, GuestShutdown::Way::GuestAgent,
                                GuestShutdown::Way::PowerButton}) {
            if (bar->currentMessage() == text(vm->name(), said)) {
                bar->clearMessage();
            }
        }
    });
}

}
