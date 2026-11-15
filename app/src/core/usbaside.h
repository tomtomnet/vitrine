// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

#include <utility>

/*
 * USB devices set aside for VMs: those a VM keeps by their IDs (its USB
 * Devices settings, -device usb-host,vendorid=,productid=) that
 * vitrine-helper wrote into /etc/udev/rules.d/70-vitrine-usb-aside.rules
 * (usb-aside add, an administrator's password once): the session's user
 * gets their node at every connection (uaccess), so QEMU opens them again
 * after each reconnect, on any port.  While a VM that keeps them runs, the
 * helper also has the desktop leave them alone (usb: no automount, no sound
 * output, no phone mount).  README.md, USB.
 */
namespace UsbAside {

using Ids = std::pair<quint16, quint16>;

/* The helper's file; elsewhere for the tests */
QString path();
void setPath(const QString &path);
/* The devices it sets aside, in its order */
QList<Ids> read(const QString &file = path());
/* "18d1:4ee7,0781:5567": as the helper's requests take them */
QString text(const QList<Ids> &ids);
/* Those of @ids not set aside in @aside */
QList<Ids> missing(const QList<Ids> &ids, const QList<Ids> &aside);

}
