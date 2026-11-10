// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/vmsummary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>

#include "core/diskinfo.h"
#include "core/hostdevices.h"
#include "core/nesteddesktop.h"
#include "core/paths.h"
#include "core/pcipassthrough.h"
#include "core/qemuinfo.h"
#include "core/stackbuilder.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/uiconfig.h"

namespace VmSummary {

static QString tr(const char *text)
{
    return QCoreApplication::translate("VmSummary", text);
}

bool keptOpen(const Vm *vm)
{
    return vm->runner()->state() == VmRunner::State::Stopping &&
           vm->args().indexOf("no-shutdown") >= 0;
}

QString stateText(const Vm *vm)
{
    if (keptOpen(vm)) {
        return tr("Shut down, QEMU still open");
    }
    switch (vm->runner()->state()) {
    case VmRunner::State::Stopped:
        break;
    case VmRunner::State::Starting:
        return tr("Starting…");
    case VmRunner::State::Running:
        return tr("Running");
    case VmRunner::State::Paused:
        return tr("Paused");
    case VmRunner::State::Stopping:
        return tr("Shutting down…");
    }
    return tr("Powered off");
}

static QString sizeText(qint64 mib)
{
    return mib % 1024 == 0 ? tr("%1 GiB").arg(mib / 1024) : tr("%1 MiB").arg(mib);
}

QString homeText(const ArgsFile &args)
{
    const qint64 mib = VmConfig::memoryMiB(args);
    const VmConfig::Cpus cpus = VmConfig::cpus(args);
    QStringList parts;
    QStringList disks;

    if (mib > 0) {
        parts << tr("%1 of memory").arg(sizeText(mib));
    }
    parts << (cpus.count > 1 ? tr("%1 processors").arg(cpus.count) : tr("1 processor"));
    switch (VmConfig::screen(args)) {
    case VmConfig::Screen::Embedded:
        parts << tr("screen in this window");
        break;
    case VmConfig::Screen::OwnWindow:
        parts << tr("screen in its own window");
        break;
    case VmConfig::Screen::None:
        parts << tr("no screen");
        break;
    }
    for (const VmConfig::Disk &disk : VmConfig::disks(args)) {
        if (!disk.cdrom && !disk.file.isEmpty()) {
            disks << QFileInfo(disk.file).fileName();
        }
    }
    QString text = parts.join(tr(", ")) + '.';
    if (!disks.isEmpty()) {
        text += '\n' + tr("Disks: %1").arg(disks.join(tr(", ")));
    }
    return text;
}

/* "12.4 GiB used of 64 GiB" */
static QString usageText(const DiskInfo::Usage &u)
{
    const QLocale locale;
    auto size = [&locale](qint64 bytes) {
        return locale.formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat);
    };

    return u.capacity > 0 ? tr("%1 used of %2").arg(size(u.used), size(u.capacity))
                          : tr("%1 used").arg(size(u.used));
}

QList<Section> details(const Vm *vm, const QemuInfo *info, GpuContexts::Status contexts,
                       bool venusUsed)
{
    const ArgsFile &args = vm->args();
    QList<Section> sections;
    auto section = [&sections](const QString &title, const QList<Row> &rows) {
        if (!rows.isEmpty()) {
            sections << Section{title, rows};
        }
    };

    /* System */
    const qint64 memory = VmConfig::memoryMiB(args);
    const VmConfig::Cpus cpus = VmConfig::cpus(args);
    const QString machine = VmConfig::machineType(args);
    const QString accel = VmConfig::accel(args);
    QString processors = QString::number(cpus.count);
    QString accelText;

    if (!cpus.model.isEmpty()) {
        processors += " · " + cpus.model;
    }
    if (cpus.sockets > 0 || cpus.cores > 0 || cpus.threads > 0) {
        processors += tr(" (%1 sockets, %2 cores, %3 threads)")
                          .arg(qMax(cpus.sockets, 1))
                          .arg(qMax(cpus.cores, 1))
                          .arg(qMax(cpus.threads, 1));
    }
    if (accel == "kvm") {
        accelText = tr("KVM");
    } else if (accel.isEmpty() || accel == "tcg") {
        accelText = tr("TCG, software emulation");
    } else {
        accelText = accel;
    }
    section(tr("System"),
            {{tr("Memory"), memory > 0 ? sizeText(memory) : tr("QEMU default"), {}, {}},
             {tr("Processors"), processors, {}, {}},
             {tr("Machine"), machine.isEmpty() ? tr("QEMU default") : machine, {}, {}},
             {tr("Acceleration"), accelText, {}, {}},
             {tr("Firmware"), UiConfig::firmwareSummary(args), {}, {}}});

    QList<Row> display{{tr("Graphics"), UiConfig::displaySummary(args, info), {}, {}}};
    const VmConfig::Graphics graphics = VmConfig::graphics(args);
    if (graphics.kind == VmConfig::Graphics::Accelerated) {
        /* what the guest does with them, once it runs */
        const GpuContexts::Status status =
            vm->runner()->isActive() ? contexts : GpuContexts::Status::Unknown;
        QString native = tr("off");
        QString venus = tr("off");

        if (graphics.nativeContext) {
            switch (status) {
            case GpuContexts::Status::InUse:
                native = tr("on, in use");
                break;
            case GpuContexts::Status::Virgl:
                native = tr("on, but the guest draws through virgl");
                break;
            case GpuContexts::Status::NotOffered:
                native = tr("on, but this computer does not offer it");
                break;
            case GpuContexts::Status::Waiting:
                native = tr("on; the guest has not drawn in 3D yet");
                break;
            case GpuContexts::Status::Unknown:
                native = tr("on");
                break;
            }
        }
        if (graphics.venus) {
            venus = status == GpuContexts::Status::Unknown ? tr("on")
                    : venusUsed                            ? tr("on, in use")
                                                           : tr("on, not used yet");
        }
        display << Row{tr("DRM native context"), native, {}, {}}
                << Row{tr("Venus"), venus, {}, {}};
    }
    switch (NestedDesktop::mode(args)) {
    case NestedDesktop::Mode::Integrated:
        display << Row{tr("Nested desktop"), tr("in Vitrine's window"), {}, {}};
        break;
    case NestedDesktop::Mode::OwnWindow:
        display << Row{tr("Nested desktop"), tr("in a window of its own"), {}, {}};
        break;
    case NestedDesktop::Mode::Off:
        break;
    }
    section(tr("Display"), display);

    /* Storage */
    QList<Row> storage;
    for (const VmConfig::Disk &disk : VmConfig::disks(args)) {
        QString kind = disk.cdrom ? tr("CD/DVD") : tr("Disk");
        Row row;

        if (disk.bus != VmConfig::Disk::Other) {
            kind += QString(" (%1)").arg(UiConfig::busName(disk.bus));
        }
        row.key = kind;
        row.value = disk.file.isEmpty() ? tr("empty") : disk.file;
        if (!disk.cdrom && !disk.file.isEmpty()) {
            /* the path is relative to the folder QEMU runs in */
            const DiskInfo::Usage u = DiskInfo::usage(QDir(vm->dir()).absoluteFilePath(disk.file));
            if (u.used >= 0) {
                row.detail = usageText(u);
            }
        }
        storage << row;
    }
    if (storage.isEmpty()) {
        storage << Row{tr("Disks"), tr("none"), {}, {}};
    }
    section(tr("Storage"), storage);

    section(tr("Network and Sound"),
            {{tr("Network"), UiConfig::networkSummary(args, info), {}, {}},
             {tr("Sound"), UiConfig::audioSummary(args, info), {}, {}}});

    /* Shared folders */
    QList<Row> shares;
    for (const VmConfig::Share &s : VmConfig::shares(args)) {
        shares << Row{s.tag,
                      QDir::toNativeSeparators(s.path) + (s.readonly ? tr(" · read only") : QString()),
                      {}, s.path};
    }
    section(tr("Shared Folders"), shares);

    /* Passthrough */
    const QList<VmConfig::UsbId> usbIds = VmConfig::usbPassthrough(args);
    QList<Row> usb;
    if (!usbIds.isEmpty()) {
        const QList<UsbDevice> devices = HostDevices::usbDevices();
        for (const VmConfig::UsbId &id : usbIds) {
            QString name = tr("not connected");
            for (const UsbDevice &dev : devices) {
                if (dev.vendorId == id.vendor && dev.productId == id.product) {
                    name = QString("%1 %2").arg(dev.manufacturer, dev.product).simplified();
                }
            }
            usb << Row{QString("%1:%2")
                           .arg(id.vendor, 4, 16, QChar('0'))
                           .arg(id.product, 4, 16, QChar('0')),
                       name, {}, {}};
        }
        if (!VmConfig::hasUsbController(args)) {
            usb << Row{tr("Warning"), tr("no USB controller"), {}, {}};
        }
    }
    section(tr("USB Devices"), usb);

    const QStringList pciAddresses = VmConfig::pciPassthrough(args);
    QList<Row> pci;
    if (!pciAddresses.isEmpty()) {
        const QList<PciDevice> devices = HostDevices::pciDevices();
        const PciPassthrough *passthrough = PciPassthrough::instance();
        for (const QString &address : pciAddresses) {
            QString name = tr("not found");
            for (const PciDevice &dev : devices) {
                if (dev.address == address) {
                    name = QString("%1 %2").arg(dev.vendorName, dev.deviceName).simplified();
                }
            }
            /* what this run took it from, given back when it stops */
            const QString from = passthrough && vm->runner()->isActive()
                                     ? passthrough->takenFrom(vm->id(), address)
                                     : QString();
            pci << Row{address, name,
                       from.isEmpty() ? QString()
                       : from == "none"
                           ? tr("On vfio-pci until the VM stops")
                           : tr("Taken from %1 until the VM stops").arg(from),
                       {}};
        }
    }
    section(tr("PCI Devices"), pci);

    /* Files */
    QList<Row> files = {{tr("Folder"), QDir::toNativeSeparators(vm->dir()), {}, vm->dir()}};
    if (QFileInfo::exists(vm->runner()->logPath())) {
        files << Row{tr("Log"), QDir::toNativeSeparators(vm->runner()->logPath()), {},
                     vm->runner()->logPath()};
    }
    section(tr("Files"), files);
    return sections;
}

QString nativeContextNote(const Vm *vm, GpuContexts::Status contexts)
{
    if (!VmConfig::graphics(vm->args()).nativeContext || !vm->runner()->isActive()) {
        return {};
    }
    switch (contexts) {
    case GpuContexts::Status::Virgl:
        return tr("DRM native context was enabled, but this guest's Mesa doesn't support it: "
                  "docs/guest-mesa.md in Vitrine's repository tells how to build a Mesa with it.");
    case GpuContexts::Status::NotOffered: {
        /* a build would not help if the VM runs Vitrine's QEMU as host/ builds it */
        const QString own = VmConfig::qemuBinary(vm->args());
        const QString qemu = QFileInfo(own.isEmpty() ? Paths::qemuBinary() : own)
                                 .canonicalFilePath();
        const bool built = !qemu.isEmpty() && qemu == Paths::stackQemu() &&
                           StackBuilder::state() == StackBuilder::State::UpToDate;
        return built ? tr("This computer offers the guest no DRM native context: Vitrine's QEMU "
                          "has it for AMD GPUs, and for Intel GPUs on the i915 or Xe driver, and "
                          "this computer's GPU is none of them.")
                     : tr("This computer offers the guest no DRM native context: the "
                          "virglrenderer QEMU uses has none for its GPU. Vitrine's QEMU, which "
                          "the Builds window builds, has it for AMD GPUs, and for Intel GPUs on "
                          "the i915 or Xe driver.");
    }
    default:
        return {};
    }
}

QString shutdownText(const QString &name, GuestShutdown::Way way)
{
    switch (way) {
    case GuestShutdown::Way::None:
        break;
    case GuestShutdown::Way::ToolsAgent:
        return tr("Asked %1 to shut down through its guest tools").arg(name);
    case GuestShutdown::Way::GuestAgent:
        return tr("Asked %1 to shut down through its guest agent").arg(name);
    case GuestShutdown::Way::PowerButton:
        return tr("The guest may ask what to do: answer it on its screen. Shut Down pressed the "
                  "power button of %1.")
            .arg(name);
    }
    return {};
}

int shutdownTimeout(GuestShutdown::Way way)
{
    /* the question may wait on the guest's screen a while */
    return way == GuestShutdown::Way::PowerButton ? 120000 : 10000;
}

}
