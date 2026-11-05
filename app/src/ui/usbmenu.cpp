// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmenu.h"

#include <QAction>
#include <QFile>
#include <QMessageBox>
#include <QTimer>

#include <unistd.h>

#include "core/usbhotplug.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/icons.h"
#include "ui/usbaccess.h"
#include "ui/usbchoices.h"
#include "ui/widgets.h"

static int s_refreshMs = 1000;

/* As a menu shows it: an & is no mnemonic, a tab no column */
static QString menuText(QString text)
{
    return text.replace('&', "&&").replace('\t', ' ');
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
    menuAction()->setToolTip(UsbChoices::tip());
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
    menuAction()->setEnabled(m_vm && UsbChoices::runs(m_vm));
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
        if (UsbChoices::runs(vm)) {
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

QList<UsbMenu::Entry> UsbMenu::entries() const
{
    QList<Entry> list;

    for (const UsbChoices::Entry &c : UsbChoices::entries(m_store, m_vm, m_hosts, m_pending)) {
        Entry e;
        e.toolTip = c.tips.join('\n');
        e.enabled = c.enabled;
        switch (c.kind) {
        case UsbChoices::Entry::Note:
            e.text = menuText(c.text);
            break;
        case UsbChoices::Entry::Device:
            e.text = menuText(c.text) + (c.right.isEmpty() ? QString() : '\t' + menuText(c.right));
            e.checkable = true;
            e.checked = c.checked;
            e.device = c.device;
            break;
        case UsbChoices::Entry::Keep:
            list << Entry();
            e.text = c.text;
            e.device = Keep;
            break;
        case UsbChoices::Entry::Settings:
            if (list.isEmpty() || !list.last().text.isEmpty()) {
                list << Entry();
            }
            e.text = c.text;
            e.device = Settings;
            break;
        }
        list << e;
    }
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
    const QPointer<Vm> guard(vm);

    if (!UsbChoices::runs(vm)) {
        return;
    }
    /* the host's keyboard, mouse or Bluetooth: not without asking */
    if (dev.isInput() || dev.isWireless()) {
        if (!Widgets::confirm(dialogParent(), QMessageBox::Warning,
                              UsbChoices::confirmTitle(dev, vm), UsbChoices::confirmText(dev, vm),
                              tr("&Attach"), icon()) ||
            !guard || !UsbChoices::runs(guard)) {
            return;
        }
    }

    const QString k = UsbChoices::key(vm, dev);
    const QPointer<UsbMenu> self(this);
    m_pending.insert(k, UsbChoices::attaching());
    UsbAccess::grant({dev}, this,
                     [self, guard, dev, k](const QStringList &out, const QString &why) {
        if (!self) {
            return;
        }
        if (!guard || !UsbChoices::runs(guard)) {
            self->m_pending.remove(k);
            return;
        }
        if (!out.isEmpty()) {
            self->m_pending.remove(k);
            self->warn(UsbChoices::notGiven(dev, guard), UsbChoices::noAccess(dev, why));
            return;
        }
        UsbHotplug::of(guard->runner())->attach(dev, [self, guard, k, dev](const QString &error) {
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
                emit self->message(UsbChoices::attached(dev, guard));
            } else {
                self->warn(UsbChoices::notGiven(dev, guard), error);
            }
        });
    }, UsbHotplug::devRoot());
}

void UsbMenu::detach(Vm *vm, const UsbDevice &dev)
{
    const QString k = UsbChoices::key(vm, dev);
    const QPointer<Vm> guard(vm);
    const QPointer<UsbMenu> self(this);

    m_pending.insert(k, UsbChoices::takingBack());
    UsbHotplug::of(vm->runner())->detach(dev, [self, guard, k, dev](const QString &error) {
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
            emit self->message(UsbChoices::back(dev));
        } else {
            self->warn(UsbChoices::stillIn(dev, guard), error);
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
    QString message, error;
    if (!UsbChoices::keep(vm, m_hosts, &message, &error)) {
        warn(UsbChoices::notKept(vm), error);
        return;
    }
    if (!message.isEmpty()) {
        emit this->message(message);
    }
}
