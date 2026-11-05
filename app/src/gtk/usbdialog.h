// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>

#include <functional>

#include "core/hostdevices.h"
#include "ui/usbchoices.h"

class QTimer;
class Vm;
class VmStore;

/*
 * The USB devices of a running VM, as GNOME Boxes has them: this
 * computer's USB devices, a switch each (UsbChoices: the Qt app's USB
 * Devices menu, its entries and its words).  On, the VM has the device:
 * given once its node may be opened (UsbAccess asks for the access) and,
 * for a keyboard, a mouse or a Bluetooth adapter, once the user agrees;
 * off, it is back on this computer.  A device another VM has, and every
 * one while the VM cannot have any (no USB controller), are insensitive,
 * saying why.  Read again each second while it shows, the devices plugged
 * in or out meanwhile come and go.  Keep for the Next Starts writes the
 * choice to the VM's arguments, as its USB Devices settings do; USB
 * Settings… opens those.
 */
class UsbDialog : public Gtk4::Dialog
{
public:
    /* Before Keep writes the arguments of @vm: the settings shown may have changes to
       apply first; @go(true) to go on */
    using SaveGuard = std::function<void(Vm *vm, std::function<void(bool go)> go)>;

    UsbDialog(VmStore *store, Vm *vm, Presenter presenter, SaveGuard guard,
              std::function<void(Vm *vm)> openSettings);
    ~UsbDialog() override;

    Vm *vm() const;
    /* What it shows, in its order: the notes and devices, Keep, USB Settings… */
    QList<UsbChoices::Entry> shown() const { return m_shown; }
    /* The row of entry @index of shown(), for tests */
    GtkWidget *row(int index) const;
    /* What a device's switch does, as the user turns it (@device: in the host's devices) */
    void toggle(int device, bool on);
    void keep();
    /* The last thing it said in its toast, for tests */
    QString lastMessage() const { return m_lastMessage; }
    /* How often the host's devices are read while it shows (ms), for tests */
    static void setRefreshInterval(int ms);

private:
    void readHosts();
    void rebuild();
    void attach(const UsbDevice &dev);
    void attachNow(const UsbDevice &dev);
    void detach(const UsbDevice &dev);
    void keepNow();
    void warn(const QString &text, const QString &why);
    void say(const QString &text);
    void closed() override;

    VmStore *m_store;
    QPointer<Vm> m_vm;
    SaveGuard m_guard;
    std::function<void(Vm *)> m_openSettings;
    QList<UsbDevice> m_hosts;
    QList<UsbChoices::Entry> m_shown;
    /* attaching or taking back, by UsbChoices::key(): what the row says meanwhile */
    QHash<QString, QString> m_pending;
    QTimer *m_refresh;
    /* the switches set from the entries, not by the user */
    bool m_syncing = false;
    QString m_lastMessage;

    AdwToastOverlay *m_toasts;
    AdwWindowTitle *m_title;
    AdwPreferencesGroup *m_devices;
    /* the rows of the notes and devices, in the order of shown() */
    QList<GtkWidget *> m_rows;
    GtkWidget *m_keep;
    GtkWidget *m_settings;
};
