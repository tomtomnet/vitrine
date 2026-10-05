// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbhotplug.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

#include "core/qmpclient.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"

static QString s_sysfs = "/sys";
static QString s_dev = "/dev";
/* A device unplugged is taken back within this */
static int s_watchMs = 2000;

/* QEMU's containers of the devices of -device and device_add, with an id or without */
static const char kNamed[] = "/machine/peripheral";
static const char kAnonymous[] = "/machine/peripheral-anon";
static const char kOwnPrefix[] = "vitrine-usb-";

/* vitrine-usb-BUS-ADDRESS-VENDOR-PRODUCT */
static const QRegularExpression &ownId()
{
    static const QRegularExpression re("^vitrine-usb-(\\d+)-(\\d+)-([0-9a-f]{4})-([0-9a-f]{4})$");
    return re;
}

bool UsbHotplug::Device::isOwn() const
{
    return ownId().match(id).hasMatch();
}

/*
 * As QEMU's autoscan matches a host device, but for hostdevice, which is
 * that node alone, and an empty filter, which takes whatever device QEMU
 * may open first: no device in particular
 */
bool UsbHotplug::Device::matches(const UsbDevice &host) const
{
    if (!node.isEmpty()) {
        return node == host.devNode();
    }
    if (!bus && !address && !vendorId && !productId && port.isEmpty()) {
        return false;
    }
    /* QEMU's port path has no bus: 2.3 for sysfs's 1-2.3 */
    return (!bus || bus == host.bus) && (!address || address == host.device) &&
           (!vendorId || vendorId == host.vendorId) &&
           (!productId || productId == host.productId) &&
           (port.isEmpty() || port == host.port.section('-', 1));
}

/* What a refresh() gathers, the replies coming in any order */
struct UsbHotplug::Read
{
    quint64 generation = 0;
    int pending = 0;
    Answer controller = Answer::Unknown;
    Answer usbHost = Answer::Unknown;
    QList<Device> devices;
};

UsbHotplug *UsbHotplug::of(VmRunner *runner)
{
    if (auto *found = runner->findChild<UsbHotplug *>(QString(), Qt::FindDirectChildrenOnly)) {
        return found;
    }
    return new UsbHotplug(runner);
}

UsbHotplug::UsbHotplug(VmRunner *runner)
    : QObject(runner), m_runner(runner), m_watch(new QTimer(this))
{
    m_watch->setInterval(s_watchMs);
    connect(m_watch, &QTimer::timeout, this, &UsbHotplug::checkOwn);
    connect(runner, &VmRunner::stateChanged, this, &UsbHotplug::stateChanged);
    stateChanged();
}

QmpClient *UsbHotplug::qmp() const
{
    const VmRunner::State state = m_runner->state();
    QmpClient *client = m_runner->qmp();

    if (state != VmRunner::State::Running && state != VmRunner::State::Paused) {
        return nullptr;
    }
    return client && client->isReady() ? client : nullptr;
}

/*
 * A new run is read once it runs: its devices may be vitrine's own, of a
 * vitrine that ran before, whose devices may have left the host meanwhile
 */
void UsbHotplug::stateChanged()
{
    QmpClient *client = m_runner->qmp();

    /* the runner keeps its client: connected once */
    if (client && client != m_events) {
        m_events = client;
        connect(client, &QmpClient::qmpEvent, this,
                [this](const QString &name, const QJsonObject &) {
            /* by anyone: QEMU's own monitor, the SDL menu, here */
            if (name == "DEVICE_DELETED" && m_known) {
                refresh();
            }
        });
    }
    if (!m_runner->isActive()) {
        reset();
    } else if (!m_known && !m_reading && qmp()) {
        refresh();
    }
}

void UsbHotplug::reset()
{
    const bool was = m_known || !m_devices.isEmpty();

    m_generation++;
    m_devices.clear();
    m_known = false;
    m_reading = false;
    m_again = false;
    m_controller = Answer::Unknown;
    m_usbHost = Answer::Unknown;
    m_taking.clear();
    m_watch->stop();
    if (was) {
        emit changed();
    }
}

QList<UsbHotplug::Device> UsbHotplug::devices() const
{
    return m_devices;
}

bool UsbHotplug::isKnown() const
{
    return m_known;
}

static QString noController()
{
    return UsbHotplug::tr("This VM has no USB controller. Add one in its USB Devices settings: "
                          "it has it from its next start.");
}

static QString noLibusb()
{
    return UsbHotplug::tr("The QEMU of this VM cannot pass USB devices through: it is built "
                          "without libusb.");
}

QString UsbHotplug::unavailable() const
{
    if (!m_known) {
        return {};
    }
    if (m_usbHost == Answer::No) {
        return noLibusb();
    }
    if (m_controller == Answer::No) {
        return noController();
    }
    return {};
}

QList<UsbHotplug::Device> UsbHotplug::devicesFor(const UsbDevice &host) const
{
    QList<Device> list;

    for (const Device &d : m_devices) {
        if (d.matches(host)) {
            list << d;
        }
    }
    return list;
}

bool UsbHotplug::has(const UsbDevice &host) const
{
    return std::any_of(m_devices.cbegin(), m_devices.cend(),
                       [&host](const Device &d) { return d.matches(host); });
}

/*
 * x-query-usb fails without a USB bus; device-list-properties without
 * usb-host, once a run; then the usb-host devices in QEMU's containers,
 * and their filters, all sent at once
 */
void UsbHotplug::refresh()
{
    QmpClient *client = qmp();

    if (!client) {
        return;
    }
    if (m_reading) {
        m_again = true;
        return;
    }
    m_reading = true;
    m_again = false;

    const QPointer<UsbHotplug> self(this);
    auto read = std::make_shared<Read>();
    read->generation = m_generation;
    read->usbHost = m_usbHost;
    /* the last reply finishes */
    auto finished = [self, read]() {
        if (--read->pending == 0 && self) {
            self->finishRead(*read);
        }
    };

    read->pending++;
    client->execute("x-query-usb", {}, [read, finished](const QJsonValue &, const QString &error) {
        /* "USB support not enabled"; older QEMUs lack the command */
        read->controller = error.isEmpty()                ? Answer::Yes
                           : error.contains("not enabled") ? Answer::No
                                                           : Answer::Unknown;
        finished();
    });
    if (read->usbHost == Answer::Unknown) {
        read->pending++;
        client->execute("device-list-properties", {{"typename", "usb-host"}},
                        [read, finished](const QJsonValue &, const QString &error) {
            /* "Device 'usb-host' not found", but for errors of the connection */
            read->usbHost = error.isEmpty()             ? Answer::Yes
                            : error.contains("not found") ? Answer::No
                                                          : Answer::Unknown;
            finished();
        });
    }
    for (const char *container : {kNamed, kAnonymous}) {
        const QString parent = container;
        read->pending++;
        client->execute("qom-list", {{"path", parent}},
                        [self, read, finished, parent, client](const QJsonValue &result,
                                                               const QString &) {
            /* none: a container QEMU has not made yet, or the connection went */
            if (!self) {
                return;
            }
            for (const QJsonValue &entry : result.toArray()) {
                if (entry["type"].toString() != "child<usb-host>") {
                    continue;
                }
                Device d;
                const QString name = entry["name"].toString();
                d.path = parent + '/' + name;
                d.id = parent == kNamed ? name : QString();
                read->devices << d;
                const qsizetype index = read->devices.size() - 1;

                for (const char *property : {"hostbus", "hostaddr", "vendorid", "productid",
                                             "hostport", "hostdevice", "attached"}) {
                    const QString key = property;
                    read->pending++;
                    client->execute("qom-get", {{"path", d.path}, {"property", key}},
                                    [read, finished, index, key](const QJsonValue &value,
                                                                 const QString &) {
                        /* hostdevice is missing with an old libusb: empty */
                        Device &dev = read->devices[index];
                        if (key == "hostbus") {
                            dev.bus = value.toInt();
                        } else if (key == "hostaddr") {
                            dev.address = value.toInt();
                        } else if (key == "vendorid") {
                            dev.vendorId = quint16(value.toInt());
                        } else if (key == "productid") {
                            dev.productId = quint16(value.toInt());
                        } else if (key == "hostport") {
                            dev.port = value.toString();
                        } else if (key == "hostdevice") {
                            dev.node = value.toString();
                        } else {
                            dev.attached = value.toBool();
                        }
                        finished();
                    });
                }
            }
            finished();
        });
    }
}

void UsbHotplug::finishRead(const Read &read)
{
    if (read.generation != m_generation) {
        /* of a run that ended: reset() went on without it */
        return;
    }
    m_reading = false;
    /* the replies were the connection's end, not QEMU's: the run is ending */
    if (!qmp()) {
        m_again = false;
        return;
    }

    QList<Device> devices = read.devices;
    for (Device &d : devices) {
        /* added by bus and address alone: the device it was added for */
        const QRegularExpressionMatch m = ownId().match(d.id);
        if (m.hasMatch()) {
            d.vendorId = quint16(m.captured(3).toUShort(nullptr, 16));
            d.productId = quint16(m.captured(4).toUShort(nullptr, 16));
        }
    }
    std::sort(devices.begin(), devices.end(),
              [](const Device &a, const Device &b) { return a.path < b.path; });

    /* QEMU said nothing about its USB bus: what the arguments say */
    Answer controller = read.controller;
    if (controller == Answer::Unknown) {
        controller = VmConfig::hasUsbController(m_runner->runArgs()) ? Answer::Yes : Answer::No;
    }
    const bool different = !m_known || devices != m_devices || controller != m_controller ||
                           read.usbHost != m_usbHost;
    m_known = true;
    m_devices = devices;
    m_controller = controller;
    m_usbHost = read.usbHost;
    updateWatch();
    if (different) {
        emit changed();
    }
    if (std::exchange(m_again, false)) {
        refresh();
    }
}

void UsbHotplug::updateWatch()
{
    const bool own = std::any_of(m_devices.cbegin(), m_devices.cend(),
                                 [](const Device &d) { return d.isOwn(); });

    if (!own) {
        m_watch->stop();
        return;
    }
    if (!m_watch->isActive()) {
        m_watch->start(s_watchMs);
    }
    /* now too: found running after its device left */
    checkOwn();
}

void UsbHotplug::checkOwn()
{
    QmpClient *client = qmp();

    if (!client) {
        return;
    }
    const QList<UsbDevice> hosts = hostDevices();
    for (const Device &d : std::as_const(m_devices)) {
        if (!d.isOwn() || m_taking.contains(d.path) ||
            std::any_of(hosts.cbegin(), hosts.cend(),
                        [&d](const UsbDevice &h) { return d.matches(h); })) {
            continue;
        }
        m_taking.insert(d.path);
        note(tr("USB: the device of %1 left this computer: took it back").arg(d.id));
        const QPointer<UsbHotplug> self(this);
        const quint64 generation = m_generation;
        client->execute("device_del", {{"id", d.id}},
                        [self, generation, path = d.path](const QJsonValue &, const QString &) {
            if (self && self->m_generation == generation) {
                self->m_taking.remove(path);
                self->refresh();
            }
        });
    }
}

void UsbHotplug::note(const QString &text) const
{
    m_runner->appendNote(text);
}

void UsbHotplug::attach(const UsbDevice &host, const Done &done)
{
    QmpClient *client = qmp();

    if (!client) {
        done(tr("The VM is not running."));
        return;
    }
    if (const QString why = unavailable(); !why.isEmpty()) {
        done(why);
        return;
    }
    if (has(host)) {
        done({});
        return;
    }
    const QString id = idFor(host);
    const QJsonObject arguments{{"driver", "usb-host"}, {"id", id}, {"hostbus", host.bus},
                                {"hostaddr", host.device}};
    const QString what = tr("%1 at bus %2, address %3")
                             .arg(host.displayName()).arg(host.bus).arg(host.device);
    const QPointer<UsbHotplug> self(this);
    client->execute("device_add", arguments,
                    [self, what, id, done](const QJsonValue &, const QString &error) {
        if (!self) {
            return;
        }
        self->note(error.isEmpty() ? tr("USB: attached %1, as %2").arg(what, id)
                                   : tr("USB: could not attach %1: %2").arg(what, error));
        self->refresh();
        done(error.isEmpty() ? QString() : explain(error));
    });
}

void UsbHotplug::detach(const UsbDevice &host, const Done &done)
{
    QmpClient *client = qmp();

    if (!client) {
        done(tr("The VM is not running."));
        return;
    }
    const QList<Device> list = devicesFor(host);
    if (list.isEmpty()) {
        done({});
        return;
    }

    struct State {
        int pending = 0;
        QStringList which;
        QString error;
    };
    auto state = std::make_shared<State>();
    const QPointer<UsbHotplug> self(this);
    const QString name = host.displayName();
    for (const Device &d : list) {
        /* one of the arguments, without an id, by its path */
        const QString which = d.id.isEmpty() ? d.path : d.id;
        state->which << which;
        state->pending++;
        client->execute("device_del", {{"id", which}},
                        [self, state, done, name](const QJsonValue &, const QString &error) {
            /* gone already: as asked */
            if (!error.isEmpty() && !error.contains("not found") && state->error.isEmpty()) {
                state->error = error;
            }
            if (--state->pending > 0 || !self) {
                return;
            }
            const QString what = QString("%1 (%2)").arg(name, state->which.join(", "));
            self->note(state->error.isEmpty()
                           ? tr("USB: took back %1").arg(what)
                           : tr("USB: could not take back %1: %2").arg(what, state->error));
            self->refresh();
            done(state->error);
        });
    }
}

QString UsbHotplug::idFor(const UsbDevice &host)
{
    return QString("%1%2-%3-%4-%5")
        .arg(kOwnPrefix)
        .arg(host.bus)
        .arg(host.device)
        .arg(host.vendorId, 4, 16, QChar('0'))
        .arg(host.productId, 4, 16, QChar('0'));
}

QString UsbHotplug::explain(const QString &error)
{
    if (error.contains("failed to find host usb device")) {
        return tr("It is no longer plugged in.");
    }
    if (error.contains("failed to open host usb device")) {
        return tr("QEMU could not open it: another program, or another VM, may be using it.");
    }
    if (error.contains("No 'usb-bus' bus found")) {
        return noController();
    }
    if (error.contains("'usb-host' is not a valid device model name")) {
        return noLibusb();
    }
    return error;
}

QList<UsbDevice> UsbHotplug::hostDevices()
{
    QList<UsbDevice> list = HostDevices::usbDevices(s_sysfs);

    list.removeIf([](const UsbDevice &d) { return d.isHub || d.bus <= 0 || d.device <= 0; });
    return list;
}

QString UsbHotplug::nodeOf(const UsbDevice &host)
{
    /* /dev/bus/usb/001/004 */
    return s_dev + host.devNode().mid(4);
}

QString UsbHotplug::devRoot()
{
    return s_dev;
}

void UsbHotplug::setRoots(const QString &sysfs, const QString &dev)
{
    s_sysfs = sysfs;
    s_dev = dev;
}

void UsbHotplug::setWatchInterval(int ms)
{
    s_watchMs = ms;
}
