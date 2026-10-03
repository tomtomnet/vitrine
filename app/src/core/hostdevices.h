// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

    /* /dev/bus/usb/001/004 */
    QString devNode() const;
    /* "Logitech USB Receiver (046d:c52b)" */
    QString displayName() const;
};

namespace HostDevices {

bool iommuEnabled(const QString &sysfs = "/sys");
QList<PciDevice> pciDevices(const QString &sysfs = "/sys");
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
