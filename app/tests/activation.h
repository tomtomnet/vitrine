// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <qpa/qwindowsysteminterface.h>

/*
 * The user goes to another app, then comes back to this one: the
 * application's state as the compositor's activation sets it, through
 * Qt's window system events (Qt6::GuiPrivate)
 */
inline void comeBack()
{
    QWindowSystemInterface::handleApplicationStateChanged<
        QWindowSystemInterface::SynchronousDelivery>(Qt::ApplicationInactive);
    QWindowSystemInterface::handleApplicationStateChanged<
        QWindowSystemInterface::SynchronousDelivery>(Qt::ApplicationActive);
}
