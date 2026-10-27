// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostgpus.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cstring>

#include <sys/sysmacros.h>

#include "core/hostdevices.h"

static QString s_sysfs = "/sys";
static QString s_dev = "/dev";
static std::optional<dev_t> s_desktopDevice;

/*
 * The native contexts of Vitrine's virglrenderer (host/build.sh's
 * drm-renderers), in the order of its table: without x-render-node, the
 * first render node of the first driver it finds
 */
static const QStringList kNativeContextDrivers = {"amdgpu", "i915", "xe"};

static QString readText(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

/* @path, a path of this host's /dev, under the root of setRoots() */
static QString underDev(const QString &path)
{
    return path.startsWith("/dev/") ? s_dev + path.mid(4) : path;
}

/* Where @path leads, every link followed; empty when nothing is there */
static QString resolved(const QString &path)
{
    return QFileInfo(underDev(path)).canonicalFilePath();
}

/* renderD129: 129, for the order of the nodes */
static int nodeNumber(const HostGpu &gpu)
{
    return QFileInfo(gpu.node).fileName().mid(7).toInt();
}

/* The PCI address of the device of DRM device @device, from sysfs; empty if none */
static QString addressOf(dev_t device)
{
    const QString path =
        QString("%1/dev/char/%2:%3").arg(s_sysfs).arg(major(device)).arg(minor(device));
    const QString pci = QFileInfo(path + "/device").canonicalFilePath();

    return pci.isEmpty() ? QString() : QFileInfo(pci).fileName();
}

QString HostGpu::path() const
{
    return byPath.isEmpty() ? node : byPath;
}

QString HostGpu::name() const
{
    /* pci.ids keeps the names people know in brackets: "Battlemage G21 [Arc Pro B50]" */
    static const QRegularExpression bracket("\\[([^\\]]+)\\]");
    static const char *const suffixes[] = {" Corporation", " Corp.", ", Inc.", " Inc.",
                                           " Co., Ltd.", ", Ltd.", " Ltd.", " Limited",
                                           " GmbH", " AG"};
    QString vendor = vendorName;
    QString model = deviceName;

    /* PciDevice names what pci.ids does not know by its ID */
    if (vendor.isEmpty() || vendor == QString::asprintf("%04x", vendorId)) {
        vendor = QStringLiteral("GPU");
    } else if (const QRegularExpressionMatch m = bracket.match(vendor); m.hasMatch()) {
        /* "Advanced Micro Devices, Inc. [AMD/ATI]" */
        vendor = m.captured(1).section('/', 0, 0);
    } else {
        for (const char *suffix : suffixes) {
            if (vendor.endsWith(QLatin1String(suffix))) {
                vendor.chop(qsizetype(strlen(suffix)));
                break;
            }
        }
    }
    if (model.isEmpty() || model == QString::asprintf("%04x", deviceId)) {
        model = QString::asprintf("%04x:%04x", vendorId, deviceId);
    } else if (const QRegularExpressionMatch m = bracket.match(model); m.hasMatch()) {
        model = m.captured(1);
    }
    return QString("%1 %2").arg(vendor, model).simplified();
}

QString HostGpu::shortAddress() const
{
    return address.mid(5);
}

bool HostGpu::hasNativeContext() const
{
    return kNativeContextDrivers.contains(driver);
}

namespace HostGpus {

QList<HostGpu> list()
{
    const QDir drm(s_sysfs + "/class/drm");
    const QDir::Filters entries = QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot;
    QHash<QString, QString> links;      // a node, links followed: udev's link to it
    QList<HostGpu> gpus;

    for (const QFileInfo &link :
         QDir(s_dev + "/dri/by-path").entryInfoList({"*-render"}, entries, QDir::Name)) {
        const QString node = link.canonicalFilePath();
        if (!node.isEmpty() && !links.contains(node)) {
            links.insert(node, link.fileName());
        }
    }
    for (const QString &name : drm.entryList({"renderD*"}, entries)) {
        const QString device = QFileInfo(drm.filePath(name) + "/device").canonicalFilePath();

        /* on PCI: vgem, for one, is a device of no bus */
        if (device.isEmpty() ||
            QFileInfo(QFileInfo(device + "/subsystem").symLinkTarget()).fileName() != "pci") {
            continue;
        }
        const PciDevice pci = HostDevices::pciDevice(QFileInfo(device).fileName(), s_sysfs);
        /* a display controller: VGA, 3D or other */
        if ((pci.classCode >> 16) != 0x03) {
            continue;
        }
        HostGpu gpu;
        gpu.node = "/dev/dri/" + name;
        const QString node = resolved(gpu.node);
        if (!node.isEmpty() && links.contains(node)) {
            gpu.byPath = "/dev/dri/by-path/" + links.value(node);
        }
        gpu.address = pci.address;
        gpu.driver = pci.driver;
        gpu.vendorId = pci.vendorId;
        gpu.deviceId = pci.deviceId;
        gpu.vendorName = pci.vendorName;
        gpu.deviceName = pci.deviceName;
        gpu.bootVga = readText(device + "/boot_vga") == "1";
        gpus << gpu;
    }
    std::sort(gpus.begin(), gpus.end(), [](const HostGpu &a, const HostGpu &b) {
        return a.address != b.address ? a.address < b.address : nodeNumber(a) < nodeNumber(b);
    });
    return gpus;
}

std::optional<HostGpu> ofDevice(dev_t device)
{
    const QString address = addressOf(device);

    if (!address.isEmpty()) {
        for (const HostGpu &gpu : list()) {
            if (gpu.address == address) {
                return gpu;
            }
        }
    }
    return std::nullopt;
}

void setDesktopDevice(std::optional<dev_t> device)
{
    s_desktopDevice = device;
}

std::optional<HostGpu> desktop(const QList<HostGpu> &gpus)
{
    if (s_desktopDevice) {
        const QString address = addressOf(*s_desktopDevice);
        for (const HostGpu &gpu : gpus) {
            if (!address.isEmpty() && gpu.address == address) {
                return gpu;
            }
        }
    }
    for (const HostGpu &gpu : gpus) {
        if (gpu.bootVga) {
            return gpu;
        }
    }
    return std::nullopt;
}

std::optional<HostGpu> rendererChoice(const QList<HostGpu> &gpus)
{
    for (const QString &driver : kNativeContextDrivers) {
        std::optional<HostGpu> first;
        for (const HostGpu &gpu : gpus) {
            if (gpu.driver == driver && (!first || nodeNumber(gpu) < nodeNumber(*first))) {
                first = gpu;
            }
        }
        if (first) {
            return first;
        }
    }
    return std::nullopt;
}

std::optional<HostGpu> automatic(const QList<HostGpu> &gpus)
{
    if (gpus.size() < 2) {
        return std::nullopt;
    }
    const std::optional<HostGpu> gpu = desktop(gpus);
    return gpu && gpu->hasNativeContext() ? gpu : std::nullopt;
}

QString automaticNode()
{
    const std::optional<HostGpu> gpu = automatic(list());
    return gpu ? gpu->node : QString();
}

QString displayNode()
{
    const QList<HostGpu> gpus = list();
    const std::optional<HostGpu> gpu = gpus.size() < 2 ? std::nullopt : desktop(gpus);
    return gpu ? gpu->node : QString();
}

std::optional<HostGpu> find(const QList<HostGpu> &gpus, const QString &path)
{
    const QString target = resolved(path);

    if (!target.isEmpty()) {
        for (const HostGpu &gpu : gpus) {
            if (resolved(gpu.node) == target) {
                return gpu;
            }
        }
    }
    return std::nullopt;
}

PathState pathState(const QString &path)
{
    const QString target = resolved(path);

    if (target.isEmpty()) {
        return PathState::Missing;
    }
    return target.startsWith(QFileInfo(s_dev).canonicalFilePath() + "/dri/renderD")
               ? PathState::RenderNode
               : PathState::NotRenderNode;
}

void setRoots(const QString &sysfs, const QString &dev)
{
    s_sysfs = sysfs;
    s_dev = dev;
}

}
