// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QMenu>
#include <QPointer>

#include <functional>

#include "core/hostdevices.h"

class QTimer;
class QToolBar;
class QToolButton;
class Vm;
class VmStore;

/*
 * USB Devices: this computer's USB devices for the selected VM while it
 * runs, whatever shows its screen (vitrine's window, QEMU's own, none).
 * A device is checked when the VM has it: a click gives it to the VM -
 * once its node may be opened, and the user agrees for a keyboard, a
 * mouse or a Bluetooth adapter - or takes it back.  A device another VM
 * has is disabled, saying which, as are all when the VM cannot have any
 * (no USB controller).  Devices unplugged meanwhile leave the menu.  Keep
 * for the Next Starts writes the choice to the VM's arguments, as its USB
 * Devices settings do.  One menu for the Machine menu, a toolbar button
 * (addTo()) and the list's context menu.
 */
class UsbMenu : public QMenu
{
    Q_OBJECT

public:
    explicit UsbMenu(VmStore *store, QWidget *parent = nullptr);

    /* The VM it is for, the selected one: enabled while it runs or is paused */
    void setVm(Vm *vm);
    Vm *vm() const;
    /* A button on @toolbar that opens this menu */
    QToolButton *addTo(QToolBar *toolbar);
    /*
     * Asked before the arguments of @vm are written, false to give up: the
     * settings shown may have changes to apply first
     */
    void setSaveGuard(const std::function<bool(Vm *vm)> &guard);
    /* How often the host's devices are read while the menu shows (ms), for tests */
    static void setRefreshInterval(int ms);

signals:
    /* USB Settings… */
    void settingsRequested(Vm *vm);
    /* What was done, for the status bar */
    void message(const QString &text);

private:
    /* An entry as shown, to rebuild the menu only when it changes */
    struct Entry
    {
        QString text;
        QString toolTip;
        bool checkable = false;
        bool checked = false;
        bool enabled = false;
        int device = -1;            // in m_hosts
        bool operator==(const Entry &other) const = default;
    };
    enum Command { Keep = -2, Settings = -3 };

    void showing();
    void readHosts();
    QList<Entry> entries() const;
    void rebuild();
    void updateEnabled();
    void attach(Vm *vm, const UsbDevice &dev);
    void detach(Vm *vm, const UsbDevice &dev);
    void keep(Vm *vm);
    /* The VM other than @vm that has @dev, if any */
    Vm *holder(const UsbDevice &dev, const Vm *vm) const;
    /* The device IDs @vm would get at its next starts if kept now; @changes if not those it gets */
    QList<std::pair<quint16, quint16>> keptIds(Vm *vm, bool *changes) const;
    QString key(const Vm *vm, const UsbDevice &dev) const;
    void warn(const QString &text, const QString &why);
    QWidget *dialogParent() const;

    VmStore *m_store;
    QPointer<Vm> m_vm;
    QList<UsbDevice> m_hosts;
    QList<Entry> m_shown;
    QTimer *m_refresh;
    /* attaching or taking back, by key(): what the entry says meanwhile */
    QHash<QString, QString> m_pending;
    std::function<bool(Vm *)> m_saveGuard;
};
