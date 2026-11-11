// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostdevices.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QSet>

#include <algorithm>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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

bool UsbDevice::isInput() const
{
    return deviceClass == 0x03 || interfaceClasses.contains(quint8(0x03));
}

bool UsbDevice::isWireless() const
{
    return deviceClass == 0xe0 || interfaceClasses.contains(quint8(0xe0));
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

/* "c 226:1" for a character device's number, "b 259:2" for a block device's */
QString nodeKey(bool block, const QString &number)
{
    return QString("%1 %2").arg(QLatin1Char(block ? 'b' : 'c'), number);
}

/* The number of the node @path, "" when it is none */
QString nodeKeyOf(const QString &path)
{
    struct stat st;

    if (::stat(QFile::encodeName(path).constData(), &st) != 0 ||
        !(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))) {
        return {};
    }
    return nodeKey(S_ISBLK(st.st_mode),
                   QString("%1:%2").arg(major(st.st_rdev)).arg(minor(st.st_rdev)));
}

/* "pipewire (pid 1234)": a sound server, which lets go of a card that goes */
bool soundServer(const QString &holder)
{
    for (const char *server : {"pipewire (", "wireplumber (", "pipewire-pulse (", "pulseaudio ("}) {
        if (holder.startsWith(QLatin1String(server))) {
            return true;
        }
    }
    return false;
}

/* What @cls makes a device of the platform's, which stays with the host; "" for another */
QString platformDevice(quint32 cls)
{
    const quint32 base = cls >> 16, sub = (cls >> 8) & 0xff;

    if (base == 0x06) {
        return QObject::tr("a bridge");
    }
    if (base == 0x08 && (sub <= 0x03 || sub == 0x06)) {
        return QObject::tr("a system peripheral of the platform");
    }
    if (base == 0x0c && sub == 0x05) {
        return QObject::tr("the SMBus controller");
    }
    return {};
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

PciDevice pciDevice(const QString &address, const QString &sysfs)
{
    return readPci(sysfs, address);
}

QList<UsbDevice> usbDevices(const QString &sysfs)
{
    QList<UsbDevice> list;
    const QDir dir(sysfs + "/bus/usb/devices");
    const IdNames &names = usbNames();
    const QStringList entries =
        dir.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
    QHash<QString, QList<quint8>> interfaces;
    QHash<QString, QStringList> drivers;

    /* 1-2:1.0 is an interface of device 1-2 (configuration 1, interface 0) */
    for (const QString &name : entries) {
        if (name.contains(':')) {
            interfaces[name.section(':', 0, 0)]
                << quint8(readHex(dir.filePath(name) + "/bInterfaceClass"));
            drivers[name.section(':', 0, 0)] << linkName(dir.filePath(name) + "/driver");
        }
    }
    for (const QString &name : entries) {
        const QString path = dir.filePath(name) + '/';
        UsbDevice dev;

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
        dev.deviceClass = quint8(readHex(path + "bDeviceClass"));
        dev.isHub = dev.deviceClass == 0x09;
        dev.interfaceClasses = interfaces.value(name);
        dev.interfaceDrivers = drivers.value(name);
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

OpenNodes openNodes(const QString &proc)
{
    OpenNodes open;
    const QDir dir(proc);

    for (const QString &pid : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 n = pid.toLongLong(&numeric);
        const QString fds = dir.filePath(pid + "/fd");
        QSet<QString> seen;

        if (!numeric) {
            continue;
        }
        const QString comm = readText(dir.filePath(pid + "/comm"));
        for (const QString &fd : QDir(fds).entryList(QDir::AllEntries | QDir::System |
                                                     QDir::NoDotAndDotDot)) {
            /* nodes are under /dev: nothing else is looked at (a stat of a file on
               a network file system may wait for its server) */
            if (!QFile::symLinkTarget(fds + '/' + fd).startsWith("/dev/")) {
                continue;
            }
            const QString key = nodeKeyOf(fds + '/' + fd);
            if (!key.isEmpty() && !seen.contains(key)) {
                seen.insert(key);
                open[key] << QString("%1 (pid %2)").arg(comm).arg(n);
            }
        }
    }
    return open;
}

QStringList pciInUse(const PciDevice &dev, const OpenNodes &open, const QString &sysfs,
                     const QString &proc)
{
    struct Node {
        QString key;        // "c 226:1"
        QString name;       // its folder: card1, nvme0n1p2
        bool block = false;
        bool sound = false;
        QString holder;     // a block device's first holder: dm-0
    };
    const QString dir = sysfs + "/bus/pci/devices/" + dev.address;
    const QString name = dev.address.mid(5);
    QList<Node> nodes;
    QStringList out, holders;
    QString up;

    if (readText(dir + "/boot_vga") == "1") {
        out << QObject::tr("%1 is the GPU this computer started on: it keeps it for its own screen.")
                   .arg(name);
    }
    const QDir::Filters dirs = QDir::Dirs | QDir::NoDotAndDotDot;
    for (const QString &card : QDir(dir + "/drm").entryList({"card*"}, dirs)) {
        if (card.contains('-')) {
            continue;
        }
        const QDir connectors(dir + "/drm/" + card);
        for (const QString &connector : connectors.entryList({card + "-*"}, dirs)) {
            if (readText(connectors.filePath(connector) + "/status") == "connected") {
                out << QObject::tr("A display is connected to %1 (%2).")
                           .arg(name, connector.mid(card.size() + 1));
            }
        }
    }
    /* its device nodes, its disks, its network interfaces: real folders only */
    QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info(path);

        if (info.dir().dirName() == "net") {
            if (up.isEmpty() && readText(path + "/operstate") == "up") {
                up = info.fileName();
            }
            continue;
        }
        const QString number = readText(path + "/dev");
        if (number.isEmpty()) {
            continue;
        }
        const QString subsystem = QFileInfo(QFileInfo(path + "/subsystem").symLinkTarget()).fileName();
        Node node{nodeKey(subsystem == "block", number), info.fileName(), subsystem == "block",
                  subsystem == "sound", {}};
        if (node.block) {
            node.holder = QDir(path + "/holders")
                              .entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)
                              .value(0);
        }
        nodes << node;
    }
    if (!up.isEmpty()) {
        out << QObject::tr("Its network interface %1 is up: take it down first (sudo ip link set %1 "
                           "down).").arg(up);
    }
    /* disks: built upon, mounted (by number, or by source: btrfs), swapped on */
    const auto block = [&nodes](const QString &key) -> const Node * {
        for (const Node &node : std::as_const(nodes)) {
            if (node.block && node.key == key) {
                return &node;
            }
        }
        return nullptr;
    };
    for (const Node &node : std::as_const(nodes)) {
        if (!node.holder.isEmpty()) {
            out << QObject::tr("%1 on it is in use by %2 (an encrypted or LVM volume, RAID).")
                       .arg(node.name, node.holder);
        }
    }
    QFile mounts(proc + "/self/mountinfo");
    if (std::any_of(nodes.cbegin(), nodes.cend(), [](const Node &n) { return n.block; }) &&
        mounts.open(QIODevice::ReadOnly)) {
        for (const QByteArray &raw : mounts.readAll().split('\n')) {
            const QStringList f = QString::fromUtf8(raw).split(' ');
            const qsizetype dash = f.indexOf("-");
            const QString source = dash >= 0 ? f.value(dash + 2) : QString();
            const Node *node = f.size() > 4 ? block(nodeKey(true, f[2])) : nullptr;

            if (!node && source.startsWith("/dev/")) {
                node = block(nodeKeyOf(source));
            }
            if (node) {
                out << QObject::tr("%1 on it is mounted on %2: unmount it first.")
                           .arg(node->name, f.value(4));
            }
        }
    }
    QFile swaps(proc + "/swaps");
    if (swaps.open(QIODevice::ReadOnly)) {
        for (const QByteArray &raw : swaps.readAll().split('\n')) {
            const QString source = QString::fromUtf8(raw).simplified().section(' ', 0, 0);
            if (const Node *node = source.startsWith("/dev/") ? block(nodeKeyOf(source)) : nullptr) {
                out << QObject::tr("%1 on it is in use as swap.").arg(node->name);
            }
        }
    }
    /* processes holding its nodes, but the sound servers on ALSA's */
    for (const Node &node : std::as_const(nodes)) {
        for (const QString &who : open.value(node.key)) {
            if (!(node.sound && soundServer(who)) && !holders.contains(who)) {
                holders << who;
            }
        }
    }
    if (!holders.isEmpty()) {
        out << QObject::tr("%1 is in use by %2.")
                   .arg(name, holders.mid(0, 3).join(", ") +
                                  (holders.size() > 3 ? QObject::tr(" and others") : QString()));
    }
    return out;
}

QStringList pciBlockers(const PciDevice &dev, const QStringList &named, const OpenNodes &open,
                        const QString &sysfs, const QString &proc)
{
    /* drivers that take nothing from the host: none, vfio's, the stub's */
    static const QStringList idle{"", "vfio-pci", "pci-stub"};
    QStringList out;

    if (dev.iommuGroup < 0) {
        return {QObject::tr("The IOMMU is off: turn it on in the firmware settings of this "
                            "computer (AMD-Vi or IOMMU on AMD, VT-d on Intel, where the kernel may "
                            "also want intel_iommu=on).")};
    }
    if (const QString platform = platformDevice(dev.classCode); !platform.isEmpty()) {
        return {QObject::tr("%1 is %2: it stays with this computer.")
                    .arg(dev.address.mid(5), platform)};
    }
    /* the whole group goes to the guest, save bridges and free devices */
    for (const QString &address : dev.groupPeers) {
        const PciDevice peer = readPci(sysfs, address);

        if ((peer.classCode >> 16) == 0x06 || idle.contains(peer.driver) || named.contains(address)) {
            continue;
        }
        out << QObject::tr("%1 shares IOMMU group %2 and is bound to %3: pass it through too.")
                   .arg(peer.displayName())
                   .arg(dev.iommuGroup)
                   .arg(peer.driver);
    }
    if (!idle.contains(dev.driver)) {
        out << pciInUse(dev, open, sysfs, proc);
    }
    return out;
}

bool pciNodeOpen(const PciDevice &dev, const QString &sysfs)
{
    /* /dev next to /sys */
    const QString node = QDir::cleanPath(QFileInfo(sysfs).absolutePath() + "/dev/vfio/" +
                                         QString::number(dev.iommuGroup));

    return dev.iommuGroup >= 0 && ::access(QFile::encodeName(node).constData(), R_OK | W_OK) == 0;
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
