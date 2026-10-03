// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostdevices.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>

#include <algorithm>

#include <sys/resource.h>
#include <unistd.h>

QString PciDevice::displayName() const
{
    return QString("%1 %2 %3").arg(address.mid(5), vendorName, deviceName).simplified();
}

QString UsbDevice::devNode() const
{
    return QString::asprintf("/dev/bus/usb/%03d/%03d", bus, device);
}

QString UsbDevice::displayName() const
{
    return QString("%1 %2 (%3:%4)")
        .arg(manufacturer, product)
        .arg(vendorId, 4, 16, QChar('0'))
        .arg(productId, 4, 16, QChar('0'))
        .simplified();
}

namespace {

/* The names in pci.ids or usb.ids */
struct IdNames
{
    QHash<quint16, QString> vendors;
    QHash<quint32, QString> devices;        // vendor << 16 | device
    QHash<quint8, QString> classes;
    QHash<quint16, QString> subclasses;     // class << 8 | subclass

    explicit IdNames(const QStringList &paths);
};

IdNames::IdNames(const QStringList &paths)
{
    QFile f;

    for (const QString &path : paths) {
        f.setFileName(path);
        if (f.open(QIODevice::ReadOnly)) {
            break;
        }
    }
    if (!f.isOpen()) {
        return;
    }

    int vendor = -1, cls = -1;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw);
        bool ok = false;

        if (line.isEmpty() || line.startsWith('#') || line.startsWith("\t\t")) {
            continue;
        }
        if (line.startsWith('\t')) {
            if (vendor >= 0) {
                const quint16 id = line.mid(1, 4).toUShort(&ok, 16);
                if (ok) {
                    devices.insert(quint32(vendor) << 16 | id, line.mid(5).trimmed());
                }
            } else if (cls >= 0) {
                const quint8 id = quint8(line.mid(1, 2).toUShort(&ok, 16));
                if (ok) {
                    subclasses.insert(quint16(cls << 8 | id), line.mid(3).trimmed());
                }
            }
            continue;
        }
        vendor = cls = -1;
        if (line.startsWith("C ")) {
            cls = line.mid(2, 2).toUShort(&ok, 16);
            if (ok) {
                classes.insert(quint8(cls), line.mid(4).trimmed());
            } else {
                cls = -1;
            }
        } else if (line.size() > 6 && line[4] == ' ') {
            /* other sections start with a word, not an ID */
            vendor = line.left(4).toUShort(&ok, 16);
            if (ok) {
                vendors.insert(quint16(vendor), line.mid(5).trimmed());
            } else {
                vendor = -1;
            }
        }
    }
}

const IdNames &pciNames()
{
    static const IdNames names({"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"});
    return names;
}

const IdNames &usbNames()
{
    static const IdNames names({"/usr/share/hwdata/usb.ids", "/usr/share/misc/usb.ids"});
    return names;
}

QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

quint32 readHex(const QString &path)
{
    QString text = readText(path);

    if (text.startsWith("0x")) {
        text.remove(0, 2);
    }
    return text.toUInt(nullptr, 16);
}

/* The name of what the symlink @path points to */
QString linkName(const QString &path)
{
    const QFileInfo fi(path);
    return fi.isSymLink() ? QFileInfo(fi.symLinkTarget()).fileName() : QString();
}

PciDevice readPci(const QString &sysfs, const QString &address)
{
    const QString path = sysfs + "/bus/pci/devices/" + address;
    const IdNames &names = pciNames();
    PciDevice dev;

    dev.address = address;
    dev.vendorId = quint16(readHex(path + "/vendor"));
    dev.deviceId = quint16(readHex(path + "/device"));
    dev.classCode = readHex(path + "/class");
    dev.driver = linkName(path + "/driver");
    dev.vendorName = names.vendors.value(dev.vendorId);
    dev.deviceName = names.devices.value(quint32(dev.vendorId) << 16 | dev.deviceId);
    dev.className = names.subclasses.value(quint16(dev.classCode >> 8),
                                           names.classes.value(quint8(dev.classCode >> 16)));
    if (dev.vendorName.isEmpty()) {
        dev.vendorName = QString::asprintf("%04x", dev.vendorId);
    }
    if (dev.deviceName.isEmpty()) {
        dev.deviceName = QString::asprintf("%04x", dev.deviceId);
    }

    const QString group = linkName(path + "/iommu_group");
    bool ok = false;
    dev.iommuGroup = group.toInt(&ok);
    if (!ok) {
        dev.iommuGroup = -1;
        return dev;
    }
    const QDir peers(sysfs + "/kernel/iommu_groups/" + group + "/devices");
    for (const QString &peer : peers.entryList(QDir::AllEntries | QDir::System |
                                                   QDir::NoDotAndDotDot,
                                               QDir::Name)) {
        if (peer != address) {
            dev.groupPeers << peer;
        }
    }
    return dev;
}

QString formatBytes(qint64 bytes)
{
    return bytes >= (1LL << 30) ? QString("%1 GiB").arg(double(bytes) / (1LL << 30), 0, 'f', 1)
                                : QString("%1 MiB").arg(double(bytes) / (1LL << 20), 0, 'f', 0);
}

}

namespace HostDevices {

bool iommuEnabled(const QString &sysfs)
{
    return !QDir(sysfs + "/kernel/iommu_groups")
                .entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)
                .isEmpty();
}

QList<PciDevice> pciDevices(const QString &sysfs)
{
    QList<PciDevice> list;
    const QDir dir(sysfs + "/bus/pci/devices");

    for (const QString &address : dir.entryList(QDir::AllEntries | QDir::System |
                                                    QDir::NoDotAndDotDot,
                                                QDir::Name)) {
        list << readPci(sysfs, address);
    }
    return list;
}

QList<UsbDevice> usbDevices(const QString &sysfs)
{
    QList<UsbDevice> list;
    const QDir dir(sysfs + "/bus/usb/devices");
    const IdNames &names = usbNames();

    for (const QString &name : dir.entryList(QDir::AllEntries | QDir::System |
                                                 QDir::NoDotAndDotDot)) {
        const QString path = dir.filePath(name) + '/';
        UsbDevice dev;

        /* 1-2:1.0 is an interface of device 1-2 */
        if (name.contains(':')) {
            continue;
        }
        dev.port = name;
        dev.bus = readText(path + "busnum").toInt();
        dev.device = readText(path + "devnum").toInt();
        dev.vendorId = quint16(readHex(path + "idVendor"));
        dev.productId = quint16(readHex(path + "idProduct"));
        dev.manufacturer = readText(path + "manufacturer");
        dev.product = readText(path + "product");
        dev.serial = readText(path + "serial");
        dev.isHub = readHex(path + "bDeviceClass") == 0x09;
        if (dev.manufacturer.isEmpty()) {
            dev.manufacturer = names.vendors.value(dev.vendorId);
        }
        if (dev.product.isEmpty()) {
            dev.product = names.devices.value(quint32(dev.vendorId) << 16 | dev.productId);
        }
        list << dev;
    }
    std::sort(list.begin(), list.end(), [](const UsbDevice &a, const UsbDevice &b) {
        return a.bus != b.bus ? a.bus < b.bus : a.device < b.device;
    });
    return list;
}

QStringList pciProblems(const PciDevice &dev, qint64 guestMiB, const QString &sysfs)
{
    QStringList problems;

    if (dev.iommuGroup < 0) {
        return {QObject::tr("The IOMMU is off: enable VT-d (Intel) or AMD-Vi (AMD) in the "
                            "firmware settings, and on Intel add intel_iommu=on to the "
                            "kernel command line")};
    }
    if (dev.driver != "vfio-pci") {
        problems << (dev.driver.isEmpty()
                         ? QObject::tr("%1 is not bound to vfio-pci").arg(dev.address)
                         : QObject::tr("%1 is bound to %2 instead of vfio-pci")
                               .arg(dev.address, dev.driver));
    }
    /* the whole group goes to the guest, save bridges and unbound devices */
    for (const QString &address : dev.groupPeers) {
        const PciDevice peer = readPci(sysfs, address);
        const QStringList harmless{"", "vfio-pci", "pci-stub", "pcieport"};

        if ((peer.classCode >> 8) == 0x0604 || harmless.contains(peer.driver)) {
            continue;
        }
        problems << QObject::tr("%1 shares IOMMU group %2 and is bound to %3: pass it "
                                "through too, or bind it to vfio-pci")
                        .arg(peer.displayName())
                        .arg(dev.iommuGroup)
                        .arg(peer.driver);
    }
    /* /dev next to /sys */
    const QString node = QDir::cleanPath(QFileInfo(sysfs).absolutePath() + "/dev/vfio/" +
                                         QString::number(dev.iommuGroup));
    if (dev.driver == "vfio-pci" && ::access(QFile::encodeName(node), R_OK | W_OK) != 0) {
        problems << QObject::tr("No access to %1: let your user open it, e.g. with a "
                                "udev rule")
                        .arg(node);
    }
    /* VFIO pins all guest memory, and some more for the devices */
    const qint64 limit = memlockLimit();
    const qint64 needed = (qMax<qint64>(guestMiB, 0) + 1024) << 20;
    if (limit >= 0 && limit < needed) {
        problems << QObject::tr("The locked memory limit (%1) is below the guest memory "
                                "plus 1 GiB (%2): raise it, e.g. with LimitMEMLOCK=infinity "
                                "for your session or a memlock line in "
                                "/etc/security/limits.conf")
                        .arg(formatBytes(limit), formatBytes(needed));
    }
    return problems;
}

qint64 memlockLimit()
{
    struct rlimit limit;

    if (getrlimit(RLIMIT_MEMLOCK, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY) {
        return -1;
    }
    return qint64(limit.rlim_cur);
}


QList<UsbDevice> usbWithoutAccess(const QList<std::pair<quint16, quint16>> &ids,
                                  const QString &sysfs, const QString &dev)
{
    QList<UsbDevice> out;

    for (const UsbDevice &d : usbDevices(sysfs)) {
        /* /dev/bus/usb/001/004 */
        const QString node = dev + d.devNode().mid(4);
        if (!d.isHub && ids.contains({d.vendorId, d.productId}) &&
            ::access(QFile::encodeName(node).constData(), R_OK | W_OK) != 0) {
            out << d;
        }
    }
    return out;
}

bool cpuHasFlag(const QString &flag, const QString &cpuinfo)
{
    QFile f(cpuinfo);

    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    /* "flags\t\t: fpu vme ...", the same for every CPU */
    for (const QByteArray &line : f.readAll().split('\n')) {
        if (line.startsWith("flags")) {
            return QString::fromLatin1(line.mid(line.indexOf(':') + 1))
                .split(' ', Qt::SkipEmptyParts)
                .contains(flag);
        }
    }
    return false;
}
}
