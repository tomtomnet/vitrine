// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbguardtext.h"

#include <QStringList>

#include <utility>

using Holder = UsbGuard::Holder;
using Kind = Holder::Kind;

/* Before 69-libmtp.rules, whose probe it stops */
static const char kRulesFile[] = "/etc/udev/rules.d/60-vitrine-usb.rules";

QString UsbGuardText::title()
{
    return tr("USB Device in Use on This Computer");
}

QString UsbGuardText::lines(const QList<UsbGuard::Issue> &issues)
{
    QStringList out;

    for (const UsbGuard::Issue &issue : issues) {
        out << issue.text() + '.';
    }
    return out.join('\n');
}

QString UsbGuardText::udevRule(quint16 vendor, quint16 product)
{
    return QString("SUBSYSTEM==\"usb\", ATTR{idVendor}==\"%1\", ATTR{idProduct}==\"%2\", "
                   "ENV{MTP_NO_PROBE}=\"1\", ENV{ID_MTP_DEVICE}=\"\", ENV{ID_GPHOTO2}=\"\"")
        .arg(vendor, 4, 16, QChar('0'))
        .arg(product, 4, 16, QChar('0'));
}

QString UsbGuardText::explanation(const QList<UsbGuard::Issue> &issues)
{
    QString text;

    for (const UsbGuard::Issue &issue : issues) {
        const UsbDevice &d = issue.device;
        bool gvfs = false;

        text += "<p><b>" + (issue.text() + '.').toHtmlEscaped() + "</b></p>";
        for (const Holder &who : issue.holders) {
            switch (who.kind) {
            case Kind::Adb:
                text += "<p>" +
                        tr("adb's server on this computer takes the device's adb interface, so "
                           "adb in the guest finds no device. Stop it with <nobr>adb "
                           "kill-server</nobr> (the next adb command here starts it again), or "
                           "with <nobr>kill %1</nobr> where adb is no longer installed.")
                            .arg(who.pid) +
                        "</p>";
                break;
            case Kind::Mtp:
            case Kind::Gphoto2:
                /* the same fix for both, once */
                if (std::exchange(gvfs, true)) {
                    break;
                }
                text += "<p>" +
                        tr("gvfs (GNOME's Files) mounts it each time it connects, as a phone "
                           "(MTP) or a camera (PTP): this computer and the guest talking to it "
                           "at once can make it reset. For adb alone, set the phone's USB mode "
                           "to No data transfer. Or keep gvfs from mounting phones and cameras, "
                           "every one of them (<nobr>systemctl --user unmask</nobr> with the "
                           "same names undoes it):") +
                        "</p><pre>systemctl --user mask --now gvfs-mtp-volume-monitor "
                        "gvfs-gphoto2-volume-monitor</pre><p>" +
                        tr("or from this device alone, with a udev rule in %1 (as root):")
                            .arg(kRulesFile) +
                        "</p><pre>" + udevRule(d.vendorId, d.productId).toHtmlEscaped() +
                        "</pre>";
                break;
            case Kind::Qemu:
                if (who.vitrine) {
                    text += "<p>" +
                            tr("%1 passes it through too: the two QEMUs race for it at each of "
                               "its reconnects. Take it back there (USB Devices), or shut %1 "
                               "down.")
                                .arg(who.vmName.toHtmlEscaped()) +
                            "</p>";
                } else if (!who.vmName.isEmpty()) {
                    text += "<p>" +
                            tr("%1, a VM this Vitrine does not run, has it too (pid %2): stop "
                               "it, or take the device back there.")
                                .arg(who.vmName.toHtmlEscaped())
                                .arg(who.pid) +
                            "</p>";
                } else {
                    text += "<p>" +
                            tr("A QEMU this Vitrine does not run has it too (pid %1): stop that "
                               "VM, or take the device back there.")
                                .arg(who.pid) +
                            "</p>";
                }
                break;
            case Kind::Other:
                text += "<p>" +
                        tr("Quit %1, or end it with <nobr>kill %2</nobr>.")
                            .arg(who.program.toHtmlEscaped())
                            .arg(who.pid) +
                        "</p>";
                break;
            }
        }
        if (issue.claimedElsewhere) {
            text += "<p>" +
                    tr("This user cannot see which program it is: <nobr>sudo lsof %1</nobr> "
                       "names it.")
                        .arg(d.devNode()) +
                    "</p>";
        }
    }
    if (!issues.isEmpty()) {
        text += "<p>" +
                tr("Then unplug the device and plug it in again, for the guest to have all of "
                   "it. A device given from USB Devices comes back to this computer as it is "
                   "unplugged: give it again there.") +
                "</p>";
    }
    return text;
}
