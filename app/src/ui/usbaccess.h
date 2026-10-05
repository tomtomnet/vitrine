// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;
class Vm;
struct UsbDevice;

namespace UsbAccess {

/*
 * Gives this user access to the nodes of @devices it cannot open, as QEMU
 * must to pass them through: pkexec asks polkit, which shows the password
 * dialog, then setfacl gives the user access to those nodes, until the
 * devices are unplugged; the same as the menu of qemu-gui does.  Then
 * @done is called, in @context, with the names of the devices still out
 * of reach and why, both empty when all can be opened.  @dev is "/dev"
 * but for tests.
 */
void grant(const QList<UsbDevice> &devices, QObject *context,
           const std::function<void(const QStringList &names, const QString &why)> &done,
           const QString &dev = "/dev");

/*
 * Before starting @vm: grant() for the plugged in USB devices it passes
 * through whose node the user cannot open.  QEMU would fail on them
 * silently, and stop trying after a few seconds, so the VM starts after.
 * @done gets a warning if some devices stay out of reach.  @sysfs and @dev
 * are "/sys" and "/dev" but for tests.
 */
void request(Vm *vm, QObject *context, const std::function<void(const QString &warning)> &done,
             const QString &sysfs = "/sys", const QString &dev = "/dev");

}
