// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

#include "core/hostdevices.h"

class QmpClient;
class QTimer;
class VmRunner;

/*
 * Host USB devices given to a running VM and taken back, whatever shows
 * its screen, over its QMP monitor: the runner's client, as the socket
 * takes one.  device_add usb-host by the host bus and address, with an id
 * of vitrine's; device_del; and the usb-host devices the VM has, read
 * from QOM: those of its arguments too, which pass devices through by
 * vendor and product ID.  One per runner: of().
 *
 * QEMU opens a usb-host device given by bus and address as it adds it,
 * and keeps it once the host device is unplugged, waiting for a device at
 * that address: whatever device the kernel numbers so later would go to
 * the guest, a keyboard as well as another stick.  So vitrine takes its
 * own back as soon as their device leaves the host, which it checks every
 * few seconds while the VM has some.
 */
class UsbHotplug : public QObject
{
    Q_OBJECT

public:
    /* A usb-host device of the VM, and which host devices it takes */
    struct Device
    {
        QString path;               // in QOM: /machine/peripheral-anon/device[2]
        QString id;                 // empty for one without
        /* QEMU's filter, 0 or empty for any */
        int bus = 0;                // hostbus
        int address = 0;            // hostaddr
        /* vendorid, productid; for vitrine's own, those of the device it
           was added for, from its id */
        quint16 vendorId = 0;
        quint16 productId = 0;
        QString port;               // hostport: the port path, 2.3 for 1-2.3
        QString node;               // hostdevice: /dev/bus/usb/BBB/DDD
        bool attached = false;      // QEMU holds a host device for it

        /* Added by vitrine: its id is idFor() a host device */
        bool isOwn() const;
        /* QEMU gives it @host, or would, as it may open it */
        bool matches(const UsbDevice &host) const;
        bool operator==(const Device &other) const = default;
    };
    /* @error is empty on success, else why not, plainly */
    using Done = std::function<void(const QString &error)>;

    /* The one of @runner, made the first time */
    static UsbHotplug *of(VmRunner *runner);

    /* The VM's, as last read: none until read in this run */
    QList<Device> devices() const;
    /* Read since the VM started, or was found running */
    bool isKnown() const;
    /* Why host devices cannot go to the VM, else empty */
    QString unavailable() const;
    /* Those of devices() that take @host */
    QList<Device> devicesFor(const UsbDevice &host) const;
    bool has(const UsbDevice &host) const;

    /* Reads the devices again; changed() if they are not as they were */
    void refresh();
    /* @done once the VM has @host, or with why not */
    void attach(const UsbDevice &host, const Done &done);
    /* @done once the VM no longer has @host, or with why not */
    void detach(const UsbDevice &host, const Done &done);

    /* What vitrine names the usb-host device of @host:
       vitrine-usb-BUS-ADDRESS-VENDOR-PRODUCT */
    static QString idFor(const UsbDevice &host);
    /* QEMU's error about a usb-host device, as plain as can be */
    static QString explain(const QString &error);

    /* The host's devices that can go to a VM: the USB devices plugged in
       but the hubs, from the sysfs of setRoots() */
    static QList<UsbDevice> hostDevices();
    /* The node of @host, under the /dev of setRoots() */
    static QString nodeOf(const UsbDevice &host);
    static QString devRoot();
    /* "/sys" and "/dev" elsewhere: a fake host, for tests */
    static void setRoots(const QString &sysfs, const QString &dev);
    /* How often the host's devices are checked for vitrine's own (ms), for tests */
    static void setWatchInterval(int ms);

signals:
    /* devices(), isKnown() or unavailable() changed */
    void changed();

private:
    enum class Answer { Unknown, Yes, No };
    struct Read;

    explicit UsbHotplug(VmRunner *runner);
    /* The monitor, while the VM runs or is paused and QMP is ready */
    QmpClient *qmp() const;
    void stateChanged();
    void reset();
    void finishRead(const Read &read);
    void updateWatch();
    /* vitrine's own whose device left the host: taken back */
    void checkOwn();
    void note(const QString &text) const;

    VmRunner *m_runner;
    QmpClient *m_events = nullptr;  // connected to its events
    QList<Device> m_devices;
    bool m_known = false;
    bool m_reading = false;
    bool m_again = false;           // refresh() asked while reading
    quint64 m_generation = 0;       // a new run, replies of the last one ignored
    Answer m_controller = Answer::Unknown;  // a USB bus
    Answer m_usbHost = Answer::Unknown;     // QEMU has usb-host (libusb)
    QTimer *m_watch;
    QSet<QString> m_taking;         // paths of own devices being taken back
};
