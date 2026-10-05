// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmenu.h"

#include <QAction>
#include <QFile>
#include <QMessageBox>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <unistd.h>

#include "core/usbhotplug.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/icons.h"
#include "ui/usbaccess.h"
#include "ui/widgets.h"

static int s_refreshMs = 1000;

using UsbIds = std::pair<quint16, quint16>;

/* Where devices can go and come back: QMP answers */
static bool runs(const Vm *vm)
{
    const VmRunner::State state = vm->runner()->state();
    return state == VmRunner::State::Running || state == VmRunner::State::Paused;
}

/* vendor:product, as lsusb has them */
static QString idText(quint16 vendor, quint16 product)
{
    return QString("%1:%2").arg(vendor, 4, 16, QChar('0')).arg(product, 4, 16, QChar('0'));
}

/* Its manufacturer and product, from sysfs or usb.ids, else its IDs */
static QString nameOf(const UsbDevice &dev)
{
    const QString name = QString("%1 %2").arg(dev.manufacturer, dev.product).simplified();
    return name.isEmpty() ? idText(dev.vendorId, dev.productId) : name;
}

/* As a menu shows it: an & is no mnemonic, a tab no column */
static QString menuText(QString text)
{
    return text.replace('&', "&&").replace('\t', ' ');
}

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

void UsbMenu::setRefreshInterval(int ms)
{
    s_refreshMs = ms;
}

UsbMenu::UsbMenu(VmStore *store, QWidget *parent)
    : QMenu(tr("&USB Devices"), parent), m_store(store), m_refresh(new QTimer(this))
{
    setObjectName("usbDevices");
    setIcon(Icons::themed({"drive-removable-media-usb", "media-removable"},
                          QStyle::SP_DriveFDIcon));
    setToolTipsVisible(true);
    menuAction()->setToolTip(tr("Give this computer's USB devices to the VM while it runs, "
                                "or take them back"));
    menuAction()->setEnabled(false);

    /* devices plugged in or out while it shows */
    connect(m_refresh, &QTimer::timeout, this, [this]() {
        readHosts();
        rebuild();
    });
    connect(this, &QMenu::aboutToShow, this, &UsbMenu::showing);
    connect(this, &QMenu::aboutToHide, m_refresh, &QTimer::stop);

    /* what each VM has, known as it runs: another VM may have the device asked for */
    auto follow = [this](Vm *vm) {
        connect(UsbHotplug::of(vm->runner()), &UsbHotplug::changed, this, [this]() {
            if (isVisible()) {
                rebuild();
            }
        });
        connect(vm->runner(), &VmRunner::stateChanged, this, [this, vm]() {
            if (vm == m_vm) {
                updateEnabled();
            }
            if (isVisible()) {
                rebuild();
            }
        });
    };
    for (Vm *vm : store->vms()) {
        follow(vm);
    }
    connect(store, &VmStore::added, this, follow);
}

void UsbMenu::setVm(Vm *vm)
{
    m_vm = vm;
    updateEnabled();
}

Vm *UsbMenu::vm() const
{
    return m_vm;
}

void UsbMenu::updateEnabled()
{
    menuAction()->setEnabled(m_vm && runs(m_vm));
}

QToolButton *UsbMenu::addTo(QToolBar *toolbar)
{
    toolbar->addAction(menuAction());
    auto *button = qobject_cast<QToolButton *>(toolbar->widgetForAction(menuAction()));
    if (button) {
        /* a click opens the menu: the action itself does nothing */
        button->setPopupMode(QToolButton::InstantPopup);
        button->setObjectName("usbDevicesButton");
    }
    return button;
}

void UsbMenu::setSaveGuard(const std::function<bool(Vm *)> &guard)
{
    m_saveGuard = guard;
}

void UsbMenu::showing()
{
    readHosts();
    /* what the VMs have now: QEMU's own monitor or menu may have changed it */
    for (Vm *vm : m_store->vms()) {
        if (runs(vm)) {
            UsbHotplug::of(vm->runner())->refresh();
        }
    }
    m_shown.clear();
    rebuild();
    m_refresh->start(s_refreshMs);
}

void UsbMenu::readHosts()
{
    m_hosts = UsbHotplug::hostDevices();
}

QString UsbMenu::key(const Vm *vm, const UsbDevice &dev) const
{
    return QString("%1/%2/%3").arg(vm->id()).arg(dev.bus).arg(dev.device);
}

Vm *UsbMenu::holder(const UsbDevice &dev, const Vm *vm) const
{
    for (Vm *other : m_store->vms()) {
        if (other == vm || !other->runner()->isActive()) {
            continue;
        }
        const UsbHotplug *hotplug = UsbHotplug::of(other->runner());
        if (hotplug->isKnown()) {
            if (hotplug->has(dev)) {
                return other;
            }
            continue;
        }
        /* not read yet (starting): what its arguments pass through */
        for (const VmConfig::UsbId &id : VmConfig::usbPassthrough(other->runner()->runArgs())) {
            if (id.vendor == dev.vendorId && id.product == dev.productId) {
                return other;
            }
        }
    }
    return nullptr;
}

/*
 * Those of the arguments, but for the devices plugged in now, which go by
 * whether the VM has them: one the VM has is added, one it does not have
 * removed; those plugged out stay as they are
 */
QList<UsbIds> UsbMenu::keptIds(Vm *vm, bool *changes) const
{
    const UsbHotplug *hotplug = UsbHotplug::of(vm->runner());
    QList<UsbIds> saved, want;
    QSet<UsbIds> present, had;

    for (const VmConfig::UsbId &id : VmConfig::usbPassthrough(vm->args())) {
        saved << UsbIds(id.vendor, id.product);
    }
    for (const UsbDevice &d : m_hosts) {
        const UsbIds ids(d.vendorId, d.productId);
        present.insert(ids);
        if (hotplug->has(d)) {
            had.insert(ids);
        }
    }
    for (const UsbIds &ids : std::as_const(saved)) {
        if ((!present.contains(ids) || had.contains(ids)) && !want.contains(ids)) {
            want << ids;
        }
    }
    for (const UsbDevice &d : m_hosts) {
        const UsbIds ids(d.vendorId, d.productId);
        if (had.contains(ids) && !want.contains(ids)) {
            want << ids;
        }
    }
    *changes = QSet<UsbIds>(want.cbegin(), want.cend()) != QSet<UsbIds>(saved.cbegin(), saved.cend());
    return want;
}

QList<UsbMenu::Entry> UsbMenu::entries() const
{
    QList<Entry> list;
    Vm *vm = m_vm;
    const auto note = [&list](const QString &text, const QString &toolTip = {}) {
        Entry e;
        e.text = menuText(text);
        e.toolTip = toolTip;
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
        if (m_hosts.isEmpty()) {
            note(tr("No USB devices"));
        }
        for (qsizetype i = 0; i < m_hosts.size(); i++) {
            const UsbDevice &dev = m_hosts[i];
            const QList<UsbHotplug::Device> own = hotplug->devicesFor(dev);
            const QString pending = m_pending.value(key(vm, dev));
            const Vm *other = own.isEmpty() ? holder(dev, vm) : nullptr;
            const QString node = UsbHotplug::nodeOf(dev);
            QString right = idText(dev.vendorId, dev.productId);
            QStringList tip{tr("Bus %1, address %2, port %3").arg(dev.bus).arg(dev.device)
                                .arg(dev.port)};
            Entry e;

            e.device = int(i);
            e.checkable = true;
            e.checked = !own.isEmpty();
            e.enabled = pending.isEmpty() && (e.checked || (why.isEmpty() && !other));
            if (!pending.isEmpty()) {
                right = pending;
            } else if (other) {
                right = tr("in %1").arg(other->name());
                tip << tr("%1 has it: take it back there first.").arg(other->name());
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
            /* the IDs once, for a device that has no name */
            const QString name = nameOf(dev);
            e.text = menuText(name) + (right == name ? QString() : '\t' + menuText(right));
            e.toolTip = tip.join('\n');
            list << e;
        }

        bool changes = false;
        keptIds(vm, &changes);
        Entry keep;
        keep.text = tr("&Keep for the Next Starts");
        keep.device = Keep;
        keep.enabled = changes;
        keep.toolTip = changes ? tr("%1 gets the devices checked here at each start, as its "
                                    "USB Devices settings say").arg(vm->name())
                               : tr("%1 gets these devices at its next starts already")
                                     .arg(vm->name());
        list << Entry() << keep;
    }

    Entry settings;
    settings.text = tr("USB &Settings…");
    settings.device = Settings;
    settings.enabled = vm != nullptr;
    settings.toolTip = tr("The devices the VM gets as it starts, and its USB controller");
    if (list.isEmpty() || !list.last().text.isEmpty()) {
        list << Entry();
    }
    list << settings;
    return list;
}

void UsbMenu::rebuild()
{
    const QList<Entry> list = entries();

    /* not while the mouse is on it, unless it changes */
    if (list == m_shown && !actions().isEmpty()) {
        return;
    }
    m_shown = list;
    clear();
    for (const Entry &e : list) {
        if (e.text.isEmpty()) {
            addSeparator();
            continue;
        }
        QAction *action = addAction(e.text);
        action->setToolTip(e.toolTip);
        action->setCheckable(e.checkable);
        action->setChecked(e.checked);
        action->setEnabled(e.enabled);
        if (e.device == Settings) {
            action->setIcon(Icons::themed({"configure", "preferences-system"},
                                          QStyle::SP_FileDialogDetailedView));
        }
        if (!e.enabled) {
            continue;
        }
        const QPointer<Vm> vm(m_vm);
        const UsbDevice dev = e.device >= 0 ? m_hosts[e.device] : UsbDevice();
        const int command = e.device;
        const bool checked = e.checked;
        connect(action, &QAction::triggered, this, [this, vm, dev, command, checked]() {
            /* once the menu closed: dialogs may follow */
            QTimer::singleShot(0, this, [this, vm, dev, command, checked]() {
                if (!vm) {
                    return;
                }
                if (command == Settings) {
                    emit settingsRequested(vm);
                } else if (command == Keep) {
                    keep(vm);
                } else if (checked) {
                    detach(vm, dev);
                } else {
                    attach(vm, dev);
                }
            });
        });
    }
}

QWidget *UsbMenu::dialogParent() const
{
    return parentWidget() ? parentWidget()->window() : nullptr;
}

void UsbMenu::warn(const QString &text, const QString &why)
{
    auto *box = Widgets::messageBox(QMessageBox::Warning, tr("USB Devices"), text,
                                    QMessageBox::Close, dialogParent());
    box->setInformativeText(why);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

void UsbMenu::attach(Vm *vm, const UsbDevice &dev)
{
    const QString name = nameOf(dev);
    const QPointer<Vm> guard(vm);

    if (!runs(vm)) {
        return;
    }
    /* the host's keyboard, mouse or Bluetooth: not without asking */
    if (dev.isInput() || dev.isWireless()) {
        const QString text =
            dev.isInput()
                ? tr("%1 is a keyboard, a mouse or another input device: while %2 has it, "
                     "this computer cannot use it. Uncheck it in the USB Devices menu to "
                     "give it back.")
                : tr("%1 is a wireless adapter, such as Bluetooth: while %2 has it, this "
                     "computer loses what connects through it, keyboards and mice "
                     "included. Uncheck it in the USB Devices menu to give it back.");
        if (!Widgets::confirm(dialogParent(), QMessageBox::Warning,
                              tr("Give %1 to %2?").arg(name, vm->name()),
                              text.arg(name, vm->name()), tr("&Attach"), icon()) ||
            !guard || !runs(guard)) {
            return;
        }
    }

    const QString k = key(vm, dev);
    const QPointer<UsbMenu> self(this);
    m_pending.insert(k, tr("attaching…"));
    UsbAccess::grant({dev}, this,
                     [self, guard, dev, k, name](const QStringList &out, const QString &why) {
        if (!self) {
            return;
        }
        if (!guard || !runs(guard)) {
            self->m_pending.remove(k);
            return;
        }
        if (!out.isEmpty()) {
            self->m_pending.remove(k);
            self->warn(tr("%1 was not given to %2.").arg(name, guard->name()),
                       tr("QEMU must open %1, which this user may not: %2.")
                           .arg(UsbHotplug::nodeOf(dev), why));
            return;
        }
        UsbHotplug::of(guard->runner())->attach(dev, [self, guard, k, name](const QString &error) {
            if (!self) {
                return;
            }
            self->m_pending.remove(k);
            if (self->isVisible()) {
                self->rebuild();
            }
            if (!guard) {
                return;
            }
            if (error.isEmpty()) {
                emit self->message(tr("%1 attached to %2").arg(name, guard->name()));
            } else {
                self->warn(tr("%1 was not given to %2.").arg(name, guard->name()), error);
            }
        });
    }, UsbHotplug::devRoot());
}

void UsbMenu::detach(Vm *vm, const UsbDevice &dev)
{
    const QString name = nameOf(dev);
    const QString k = key(vm, dev);
    const QPointer<Vm> guard(vm);
    const QPointer<UsbMenu> self(this);

    m_pending.insert(k, tr("taking back…"));
    UsbHotplug::of(vm->runner())->detach(dev, [self, guard, k, name](const QString &error) {
        if (!self) {
            return;
        }
        self->m_pending.remove(k);
        if (self->isVisible()) {
            self->rebuild();
        }
        if (!guard) {
            return;
        }
        if (error.isEmpty()) {
            emit self->message(tr("%1 is back on this computer").arg(name));
        } else {
            self->warn(tr("%1 is still in %2.").arg(name, guard->name()), error);
        }
    });
}

void UsbMenu::keep(Vm *vm)
{
    /* the settings' own changes first, which may touch the same lines */
    if (m_saveGuard && !m_saveGuard(vm)) {
        return;
    }
    readHosts();
    bool changes = false;
    const QList<UsbIds> want = keptIds(vm, &changes);
    if (!changes) {
        return;
    }

    ArgsFile args = vm->args();
    QList<VmConfig::UsbId> ids;
    for (const UsbIds &id : want) {
        ids << VmConfig::UsbId{id.first, id.second};
    }
    VmConfig::setUsbPassthrough(args, ids);
    /* as the settings would ask: the devices need one at the next start */
    if (!ids.isEmpty() && !VmConfig::hasUsbController(args)) {
        args.add("device", "qemu-xhci");
    }
    QString error;
    if (!vm->save(args, &error)) {
        warn(tr("The USB devices of %1 were not kept.").arg(vm->name()), error);
        return;
    }
    emit message(ids.isEmpty() ? tr("%1 starts without USB devices from now on").arg(vm->name())
                               : tr("%1 gets these USB devices at its next starts too")
                                     .arg(vm->name()));
}
