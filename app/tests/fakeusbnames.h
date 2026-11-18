// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>

/*
 * The strings of the USB device at @bus and @address as udev's database has
 * them, under the fake /sys @sysfs, where HostDevices::usbDevices() looks;
 * none for a device without
 */
inline void writeUsbNames(const QString &sysfs, int bus, int address,
                          const QByteArray &manufacturer, const QByteArray &product)
{
    const QString dir = sysfs + "/run/udev/data";
    const QString path = QString("%1/c189:%2").arg(dir).arg((bus - 1) * 128 + address - 1);
    const auto encoded = [](QByteArray text) {
        return text.replace('\\', "\\x5c").replace(' ', "\\x20");
    };

    QDir().mkpath(dir);
    QFile::remove(path);
    if (manufacturer.isEmpty() && product.isEmpty()) {
        return;
    }
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write("I:1\nE:ID_VENDOR_ENC=" + encoded(manufacturer) + "\nE:ID_MODEL_ENC=" +
                   encoded(product) + "\nE:ID_BUS=usb\n");
    }
}
