// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbchoices.h"

#include <QFile>
#include <QSet>

#include <algorithm>

#include <unistd.h>

#include "core/hostsettings.h"
#include "core/pcipassthrough.h"
#include "core/usbguard.h"
#include "core/usbhotplug.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

/* The first part of a reason, up to its first ": " or ". ": the rest for its tooltip */
static QString firstPart(const QString &text)
{
    qsizetype end = text.size();
    for (const char *mark : {": ", ". "}) {
        const qsizetype at = text.indexOf(mark);
        if (at > 0 && at < end) {
            end = at;
        }
    }
    return text.left(end);
}

bool UsbChoices::runs(const Vm *vm)
{
    const VmRunner::State state = vm->runner()->state();
    return state == VmRunner::State::Running || state == VmRunner::State::Paused;
}

QString UsbChoices::idText(quint16 vendor, quint16 product)
{
    return QString("%1:%2").arg(vendor, 4, 16, QChar('0')).arg(product, 4, 16, QChar('0'));
}

QString UsbChoices::nameOf(const UsbDevice &dev)
{
    const QString name = QString("%1 %2").arg(dev.manufacturer, dev.product).simplified();
    return name.isEmpty() ? idText(dev.vendorId, dev.productId) : name;
}

QString UsbChoices::key(const Vm *vm, const UsbDevice &dev)
{
    return QString("%1/%2/%3").arg(vm->id()).arg(dev.bus).arg(dev.device);
}

Vm *UsbChoices::holder(VmStore *store, const UsbDevice &dev, const Vm *vm)
{
    /* what it has, or before it is read (starting) what its arguments pass through:
       as the attach and the start check it */
    return UsbGuard::attachConflict(store, vm, dev);
}

/*
 * Those of the arguments, but for the devices plugged in now, which go by
 * whether the VM has them: one the VM has is added, one it does not have
 * removed; those plugged out stay as they are
 */
QList<UsbChoices::Ids> UsbChoices::keptIds(Vm *vm, const QList<UsbDevice> &hosts, bool *changes)
{
    const UsbHotplug *hotplug = UsbHotplug::of(vm->runner());
    QList<Ids> saved, want;
    QSet<Ids> present, had;

    for (const VmConfig::UsbId &id : VmConfig::usbPassthrough(vm->args())) {
        saved << Ids(id.vendor, id.product);
    }
    for (const UsbDevice &d : hosts) {
        const Ids ids(d.vendorId, d.productId);
        present.insert(ids);
        if (hotplug->has(d)) {
            had.insert(ids);
        }
    }
    for (const Ids &ids : std::as_const(saved)) {
        if ((!present.contains(ids) || had.contains(ids)) && !want.contains(ids)) {
            want << ids;
        }
    }
    for (const UsbDevice &d : hosts) {
        const Ids ids(d.vendorId, d.productId);
        if (had.contains(ids) && !want.contains(ids)) {
            want << ids;
        }
    }
    *changes = QSet<Ids>(want.cbegin(), want.cend()) != QSet<Ids>(saved.cbegin(), saved.cend());
    return want;
}

QList<UsbChoices::Entry> UsbChoices::entries(VmStore *store, Vm *vm, const QList<UsbDevice> &hosts,
                                             const QHash<QString, QString> &pending)
{
    QList<Entry> list;
    const auto note = [&list](const QString &text, const QString &toolTip = {}) {
        Entry e;
        e.text = text;
        if (!toolTip.isEmpty()) {
            e.tips << toolTip;
        }
        list << e;
    };

    if (!vm || !runs(vm)) {
        note(tr("The VM is not running"));
    } else if (const UsbHotplug *hotplug = UsbHotplug::of(vm->runner()); !hotplug->isKnown()) {
        note(tr("Reading the devices of the VM…"));
    } else {
        const QString why = hotplug->unavailable();
        if (!why.isEmpty()) {
            note(firstPart(why), why);
        }
        if (hosts.isEmpty()) {
            note(tr("No USB devices"));
        }
        for (qsizetype i = 0; i < hosts.size(); i++) {
            const UsbDevice &dev = hosts[i];
            const QList<UsbHotplug::Device> own = hotplug->devicesFor(dev);
            const QString waiting = pending.value(key(vm, dev));
            QString otherWhy;
            const Vm *other =
                own.isEmpty() ? UsbGuard::attachConflict(store, vm, dev, &otherWhy, &hosts) : nullptr;
            const QString node = UsbHotplug::nodeOf(dev);
            QString right = idText(dev.vendorId, dev.productId);
            QStringList tip{tr("Bus %1, address %2, port %3").arg(dev.bus).arg(dev.device)
                                .arg(dev.port)};
            Entry e;

            e.kind = Entry::Device;
            e.device = int(i);
            e.checked = !own.isEmpty();
            e.enabled = waiting.isEmpty() && (e.checked || (why.isEmpty() && !other));
            if (!waiting.isEmpty()) {
                right = waiting;
            } else if (other) {
                right = tr("in %1").arg(other->name());
                tip << otherWhy;
            } else if (e.checked && std::none_of(own.cbegin(), own.cend(),
                                                 [](const auto &d) { return d.attached; })) {
                right = tr("not connected");
                tip << tr("The VM has it, but QEMU could not open it, for want of access "
                          "maybe: uncheck it, then check it again.");
            } else if (!e.checked &&
                       ::access(QFile::encodeName(node).constData(), R_OK | W_OK) != 0) {
                tip << tr("This user may not open %1 yet: you are asked for the access "
                          "first.").arg(node);
            }
            if (!e.checked && dev.isInput()) {
                tip << tr("A keyboard, a mouse or another input device: this computer "
                          "cannot use it while the VM has it.");
            } else if (!e.checked && dev.isWireless()) {
                tip << tr("A wireless adapter, such as Bluetooth: this computer loses what "
                          "connects through it while the VM has it.");
            }
            e.text = nameOf(dev);
            /* the IDs once, for a device that has no name */
            e.right = right == e.text ? QString() : right;
            e.tips = tip;
            list << e;
        }

        bool changes = false;
        keptIds(vm, hosts, &changes);
        Entry keep;
        keep.kind = Entry::Keep;
        keep.text = tr("&Keep for This VM");
        keep.enabled = changes;
        keep.tips << (changes ? tr("%1 gets the devices checked here at each start and whenever "
                                   "they reconnect while it runs, on any port, as its USB Devices "
                                   "settings say; the desktop leaves them alone meanwhile")
                                    .arg(vm->name())
                              : tr("%1 gets these devices at each start and reconnect already")
                                    .arg(vm->name()));
        list << keep;
    }

    Entry settings;
    settings.kind = Entry::Settings;
    settings.text = tr("USB &Settings…");
    settings.enabled = vm != nullptr;
    settings.tips << tr("The devices the VM gets as it starts, and its USB controller");
    list << settings;
    return list;
}

bool UsbChoices::keep(Vm *vm, const QList<UsbDevice> &hosts, QString *message, QString *error)
{
    bool changes = false;
    const QList<Ids> want = keptIds(vm, hosts, &changes);

    message->clear();
    if (!changes) {
        return true;
    }
    ArgsFile args = vm->args();
    QList<Ids> saved;
    for (const VmConfig::UsbId &id : VmConfig::usbPassthrough(args)) {
        saved << Ids(id.vendor, id.product);
    }
    QList<VmConfig::UsbId> ids;
    for (const Ids &id : want) {
        ids << VmConfig::UsbId{id.first, id.second};
    }
    VmConfig::setUsbPassthrough(args, ids);
    /* as the settings would ask: the devices need one at the next start */
    if (!ids.isEmpty() && !VmConfig::hasUsbController(args)) {
        args.add("device", "qemu-xhci");
    }
    if (!vm->save(args, error)) {
        return false;
    }
    *message = ids.isEmpty() ? tr("%1 starts without USB devices from now on").arg(vm->name())
                             : tr("%1 gets these USB devices at every start and reconnect").arg(vm->name());
    /* running: those kept now by their IDs at once, so that QEMU takes them back after a
       reconnect, and the desktop kept away from them while it runs */
    if (runs(vm)) {
        UsbHotplug *hotplug = UsbHotplug::of(vm->runner());
        QList<Ids> added;
        for (const Ids &id : want) {
            if (!saved.contains(id)) {
                added << id;
            }
        }
        for (const UsbDevice &d : hosts) {
            if (added.contains(Ids(d.vendorId, d.productId)) && hotplug->has(d)) {
                hotplug->keep(d, [](const QString &) {});
            }
        }
        if (!added.isEmpty() && PciPassthrough::instance()) {
            PciPassthrough::instance()->keepUsb(vm->id(), vm->name(), added, vm->runner()->pid());
        }
    }
    return true;
}

UsbChoices::SettingsStatus UsbChoices::settingsStatus(const UsbDevice &dev, bool access,
                                                     const QList<Ids> &aside)
{
    const QString ids = idText(dev.vendorId, dev.productId);
    SettingsStatus s;

    if (aside.contains(Ids(dev.vendorId, dev.productId))) {
        s.text = tr("Ready");
        s.details = tr("Set aside for VMs: a VM that keeps it gets it at each start and whenever it "
                       "reconnects, on any port, and the desktop leaves it alone while that VM "
                       "runs. Bus %1, port %2.").arg(dev.bus).arg(dev.port);
    } else if (HostSettings::helperInstalled()) {
        s.text = tr("Set aside at start");
        s.details = tr("When a VM that keeps it starts, vitrine-helper sets it aside, with an "
                       "administrator's password once: your user opens it at every connection, "
                       "and the VM gets it back whenever it reconnects. Bus %1, port %2.")
                        .arg(dev.bus).arg(dev.port);
    } else if (access) {
        s.text = tr("Ready");
        s.details = tr("Bus %1, port %2").arg(dev.bus).arg(dev.port);
    } else {
        s.text = tr("No access");
        s.details = tr("QEMU cannot open %1. Give your user access with a udev rule, e.g. in "
                       "/etc/udev/rules.d/70-qemu-usb.rules:\n"
                       "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"%2\", ATTR{idProduct}==\"%3\", "
                       "TAG+=\"uaccess\"")
                        .arg(dev.devNode(), ids.section(':', 0, 0), ids.section(':', 1));
    }
    return s;
}

QString UsbChoices::notConnected()
{
    return tr("Attached whenever it connects while the VM runs");
}

QString UsbChoices::attaching()
{
    return tr("attaching…");
}

QString UsbChoices::takingBack()
{
    return tr("taking back…");
}

QString UsbChoices::confirmTitle(const UsbDevice &dev, const Vm *vm)
{
    return tr("Give %1 to %2?").arg(nameOf(dev), vm->name());
}

QString UsbChoices::confirmText(const UsbDevice &dev, const Vm *vm)
{
    const QString text =
        dev.isInput()
            ? tr("%1 is a keyboard, a mouse or another input device: while %2 has it, "
                 "this computer cannot use it. Uncheck it in the USB Devices menu to "
                 "give it back.")
            : tr("%1 is a wireless adapter, such as Bluetooth: while %2 has it, this "
                 "computer loses what connects through it, keyboards and mice "
                 "included. Uncheck it in the USB Devices menu to give it back.");
    return text.arg(nameOf(dev), vm->name());
}

QString UsbChoices::attached(const UsbDevice &dev, const Vm *vm)
{
    return tr("%1 attached to %2").arg(nameOf(dev), vm->name());
}

QString UsbChoices::back(const UsbDevice &dev)
{
    return tr("%1 is back on this computer").arg(nameOf(dev));
}

QString UsbChoices::notGiven(const UsbDevice &dev, const Vm *vm)
{
    return tr("%1 was not given to %2.").arg(nameOf(dev), vm->name());
}

QString UsbChoices::noAccess(const UsbDevice &dev, const QString &why)
{
    return tr("QEMU must open %1, which this user may not: %2.").arg(UsbHotplug::nodeOf(dev), why);
}

QString UsbChoices::stillIn(const UsbDevice &dev, const Vm *vm)
{
    return tr("%1 is still in %2.").arg(nameOf(dev), vm->name());
}

QString UsbChoices::notKept(const Vm *vm)
{
    return tr("The USB devices of %1 were not kept.").arg(vm->name());
}

QString UsbChoices::tip()
{
    return tr("Give this computer's USB devices to the VM while it runs, or take them back");
}
