// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

/*
 * The host's PCI and USB devices, from sysfs, named from the hwdata
 * pci.ids and usb.ids.  @sysfs is "/sys" but for tests.
 */
struct PciDevice
{
    QString address;                // 0000:03:00.0
    quint16 vendorId = 0;
    quint16 deviceId = 0;
    quint32 classCode = 0;          // e.g. 0x030000
    QString vendorName;
    QString deviceName;
    QString className;              // e.g. VGA compatible controller
    QString driver;                 // bound kernel driver, if any
    int iommuGroup = -1;
    QStringList groupPeers;         // the other devices of the group

    /* "03:00.0 NVIDIA Corporation AD102 [GeForce RTX 4090]" */
    QString displayName() const;
};

struct UsbDevice
{
    int bus = 0;
    int device = 0;                 // devnum
    QString port;                   // e.g. 1-2.3
    quint16 vendorId = 0;
    quint16 productId = 0;
    QString manufacturer;
    QString product;
    QString serial;
    bool isHub = false;
    quint8 deviceClass = 0;         // bDeviceClass, 0 when its interfaces tell
    QList<quint8> interfaceClasses; // bInterfaceClass of each, e.g. 0x03 HID

    /* /dev/bus/usb/001/004 */
    QString devNode() const;
    /* "Logitech USB Receiver (046d:c52b)" */
    QString displayName() const;
    /* A keyboard, mouse or other input device: it has an HID interface */
    bool isInput() const;
    /* A wireless controller: Bluetooth adapters, and what connects through them */
    bool isWireless() const;
};

namespace HostDevices {

bool iommuEnabled(const QString &sysfs = "/sys");
QList<PciDevice> pciDevices(const QString &sysfs = "/sys");
/* The one at @address, e.g. 0000:03:00.0; its IDs 0 when there is none */
PciDevice pciDevice(const QString &address, const QString &sysfs = "/sys");
/* Hubs and root hubs included, flagged isHub */
QList<UsbDevice> usbDevices(const QString &sysfs = "/sys");
/*
 * What keeps @dev from being passed through, empty when it is ready: no
 * IOMMU, the group's devices not all bound to vfio-pci (or pci-stub, or
 * bridges), /dev/vfio/<group> not accessible to the user, a locked memory
 * limit below @guestMiB.
 */
QStringList pciProblems(const PciDevice &dev, qint64 guestMiB,
                        const QString &sysfs = "/sys");
/*
 * The device nodes processes of this user hold open (/proc/PID/fd: other
 * users' cannot be read), taken once for many devices: by number, "c
 * 226:1" or "b 259:2", who - "gnome-shell (pid 2345)"
 */
using OpenNodes = QHash<QString, QStringList>;
OpenNodes openNodes(const QString &proc = "/proc");
/*
 * What the host uses of @dev, as vitrine-helper refuses to take it from its
 * driver (it checks again as root): the GPU this computer started on, a
 * display connected to it, a process holding one of its nodes (@open; the
 * sound servers' hold on ALSA nodes aside, as they let go of a card that
 * goes), a disk on it mounted, swapped on or under the device mapper, a
 * network interface of it that is up.  Sentences; empty when none.  @proc:
 * where mountinfo and swaps are read (/proc but for tests).
 */
QStringList pciInUse(const PciDevice &dev, const OpenNodes &open, const QString &sysfs = "/sys",
                     const QString &proc = "/proc");
/*
 * What keeps @dev from being handed to a VM that passes @named through:
 * no IOMMU, a platform device (bridge, SMBus...), a device of its IOMMU
 * group on a driver of the host's that is not passed through too, and for
 * a device on a host driver pciInUse().  Sentences; empty when none.
 */
QStringList pciBlockers(const PciDevice &dev, const QStringList &named, const OpenNodes &open,
                        const QString &sysfs = "/sys", const QString &proc = "/proc");
/* /dev/vfio/<group> of @dev exists (the group is on vfio-pci) and this user opens it */
bool pciNodeOpen(const PciDevice &dev, const QString &sysfs = "/sys");
/* RLIMIT_MEMLOCK of this process in bytes, -1 for unlimited */
qint64 memlockLimit();
/* Whether the CPU of this computer has @flag, e.g. topoext */
bool cpuHasFlag(const QString &flag, const QString &cpuinfo = "/proc/cpuinfo");
/*
 * The plugged in devices matching @ids (vendor, product) whose node this
 * user cannot open, as QEMU must to pass them through.  @dev is "/dev" but
 * for tests.
 */
QList<UsbDevice> usbWithoutAccess(const QList<std::pair<quint16, quint16>> &ids,
                                  const QString &sysfs = "/sys", const QString &dev = "/dev");

}
