// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

#include "core/hostdevices.h"

class Vm;
class VmStore;

/*
 * What both apps offer of this computer's USB devices for a running VM
 * (the Qt app's USB Devices menu, the GTK app's dialog): each device
 * plugged in, whether the VM has it, whether it may have it and what to
 * say of it; Keep for the Next Starts, which writes the choice to the
 * VM's arguments as its USB Devices settings do; and the words of giving
 * a device and taking it back.  Qt Core only.
 */
class UsbChoices
{
    /* the Qt app's UsbMenu's words, in its translation context */
    Q_DECLARE_TR_FUNCTIONS(UsbMenu)

public:
    using Ids = std::pair<quint16, quint16>;

    struct Entry {
        enum Kind { Note, Device, Keep, Settings };
        Kind kind = Note;
        /* the device's name, the note's text, the command's */
        QString text;
        /* beside a device's name: its IDs, "in beta", "attaching…", "not connected" */
        QString right;
        /* what to know of it, a line each */
        QStringList tips;
        bool checked = false;
        bool enabled = false;
        /* in the host's devices given, -1 for none */
        int device = -1;
        bool operator==(const Entry &other) const = default;
    };

    /* Where devices can go and come back: QMP answers (running or paused) */
    static bool runs(const Vm *vm);
    /* vendor:product, as lsusb has them */
    static QString idText(quint16 vendor, quint16 product);
    /* Its manufacturer and product, from sysfs or usb.ids, else its IDs */
    static QString nameOf(const UsbDevice &dev);
    /* @vm's own name for @dev in @pending: "VM/bus/device" */
    static QString key(const Vm *vm, const UsbDevice &dev);
    /* The VM of @store other than @vm that has @dev, if any */
    static Vm *holder(VmStore *store, const UsbDevice &dev, const Vm *vm);
    /*
     * The device IDs @vm would get at its next starts if kept now: those of
     * its arguments, but for the devices plugged in (@hosts), which go by
     * whether the VM has them; @changes if not those it gets now
     */
    static QList<Ids> keptIds(Vm *vm, const QList<UsbDevice> &hosts, bool *changes);
    /*
     * What to show for @vm: notes (not running, reading, why it cannot,
     * none plugged in), the devices of @hosts, Keep and USB Settings;
     * @pending: what an entry says while its device is given or taken
     * back, by key()
     */
    static QList<Entry> entries(VmStore *store, Vm *vm, const QList<UsbDevice> &hosts,
                                const QHash<QString, QString> &pending);
    /*
     * The devices of @hosts @vm has now written to its arguments, for its
     * next starts (and a USB controller, if it had none): @message says
     * what changed, empty when nothing did; false with @error if the
     * arguments could not be saved (notKept() says so)
     */
    static bool keep(Vm *vm, const QList<UsbDevice> &hosts, QString *message, QString *error);

    /* What the entries say meanwhile */
    static QString attaching();
    static QString takingBack();
    /* A keyboard or a mouse, a wireless adapter: the question before @vm gets @dev */
    static QString confirmTitle(const UsbDevice &dev, const Vm *vm);
    static QString confirmText(const UsbDevice &dev, const Vm *vm);
    /* How it went */
    static QString attached(const UsbDevice &dev, const Vm *vm);
    static QString back(const UsbDevice &dev);
    static QString notGiven(const UsbDevice &dev, const Vm *vm);
    static QString noAccess(const UsbDevice &dev, const QString &why);
    static QString stillIn(const UsbDevice &dev, const Vm *vm);
    static QString notKept(const Vm *vm);
    /* What USB Devices does, for its menu or its button */
    static QString tip();
};
