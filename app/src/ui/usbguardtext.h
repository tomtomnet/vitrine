// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QList>
#include <QString>

#include "core/usbguard.h"

/*
 * The words of the notice of a USB device that a program of this computer
 * holds while a VM passes it through (UsbGuard), for both apps: what holds
 * it, and the fix that fits - adb's server, GNOME's MTP or camera mounts
 * (gvfs), another QEMU, any other program, or one this user cannot see.
 * Rich text as Qt's labels take it: the GTK app makes Pango markup of it.
 * Qt Core only.
 */
class UsbGuardText
{
    Q_DECLARE_TR_FUNCTIONS(UsbGuardText)

public:
    /* The notice's title, and its explanation's */
    static QString title();
    /* What holds what, plain: a sentence per issue, a line each */
    static QString lines(const QList<UsbGuard::Issue> &issues);
    /* The same, each with its fix, rich text */
    static QString explanation(const QList<UsbGuard::Issue> &issues);
    /* The udev rule that keeps gvfs and libmtp's probe away from a device of these IDs */
    static QString udevRule(quint16 vendor, quint16 product);
};
