// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

#include <functional>

class QObject;
class Vm;

namespace UsbAccess {

/*
 * Before starting @vm: gives this user access to the plugged in USB devices
 * it passes through whose node the user cannot open, as QEMU must.  QEMU
 * would fail on them silently, and stop trying after a few seconds, so the
 * VM starts after.  pkexec asks polkit, which shows the password dialog,
 * then setfacl gives the user access to those nodes, until the devices are
 * unplugged; the same as the menu of qemu-gui does.  Then @done is called,
 * in @context, with a warning if some devices stay out of reach.  @sysfs
 * and @dev are "/sys" and "/dev" but for tests.
 */
void request(Vm *vm, QObject *context, const std::function<void(const QString &warning)> &done,
             const QString &sysfs = "/sys", const QString &dev = "/dev");

}
