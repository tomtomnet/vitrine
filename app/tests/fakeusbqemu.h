// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSet>
#include <QStringList>

#include <utility>

/*
 * QEMU's end of QMP for USB, as QEMU 11.1 answers: its usb-host devices
 * in /machine/peripheral (with an id) and /machine/peripheral-anon;
 * device_add usb-host by bus and address opening the host device at once,
 * failing for one not plugged in or busy; device_del taking a USB device
 * out of QOM at once, DEVICE_DELETED following its reply here (QEMU sends
 * it as it frees the device, through RCU: before or after the reply).
 */
class FakeUsbQemu : public QObject
{
public:
    struct Device
    {
        QString path;
        QString type = "usb-host";
        int bus = 0, address = 0;
        int vendor = 0, product = 0;
        QString port, node;
        bool attached = false;
    };

    explicit FakeUsbQemu(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            m_peers << peer;
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() { read(peer); });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }

    bool controller = true;
    bool libusb = true;
    /* host devices QEMU finds, "bus/address", and those another program holds */
    QSet<QString> plugged;
    QSet<QString> busy;
    QList<Device> devices;
    QList<QJsonObject> commands;

    QStringList executed() const
    {
        QStringList names;
        for (const QJsonObject &c : commands) {
            names << c["execute"].toString();
        }
        return names;
    }
    QList<QJsonObject> executed(const QString &name) const
    {
        QList<QJsonObject> list;
        for (const QJsonObject &c : commands) {
            if (c["execute"] == name) {
                list << c;
            }
        }
        return list;
    }
    void event(const QString &name, const QJsonObject &data)
    {
        for (QLocalSocket *peer : std::as_const(m_peers)) {
            if (peer->state() == QLocalSocket::ConnectedState) {
                peer->write(QJsonDocument(QJsonObject{{"event", name}, {"data", data}})
                                .toJson(QJsonDocument::Compact) + '\n');
            }
        }
    }
    /* As QEMU does after device_del, or a guest's unplug: DEVICE_DELETED
       now, or with @later after the reply being written */
    void remove(const QString &path, QList<QJsonObject> *later = nullptr)
    {
        for (qsizetype i = 0; i < devices.size(); i++) {
            if (devices[i].path == path) {
                devices.removeAt(i);
                QJsonObject data{{"path", path}};
                if (path.startsWith("/machine/peripheral/")) {
                    data["device"] = path.section('/', 3);
                }
                if (later) {
                    *later << data;
                } else {
                    event("DEVICE_DELETED", data);
                }
                return;
            }
        }
    }

private:
    static QJsonObject error(const QString &cls, const QString &desc)
    {
        return QJsonObject{{"class", cls}, {"desc", desc}};
    }

    QJsonValue answer(const QJsonObject &command, QJsonObject *failure)
    {
        const QString name = command["execute"].toString();
        const QJsonObject args = command["arguments"].toObject();

        if (name == "query-status") {
            return QJsonObject{{"running", true}, {"status", "running"}};
        }
        if (name == "x-query-usb") {
            if (!controller) {
                *failure = error("GenericError", "USB support not enabled");
                return {};
            }
            return QJsonObject{{"human-readable-text", ""}};
        }
        if (name == "device-list-properties") {
            if (!libusb && args["typename"] == "usb-host") {
                *failure = error("DeviceNotFound", "Device 'usb-host' not found");
                return {};
            }
            return QJsonArray();
        }
        if (name == "qom-list") {
            const QString path = args["path"].toString();
            QJsonArray list{QJsonObject{{"name", "type"}, {"type", "string"}}};
            /* the controller, which is no usb-host */
            if (path == "/machine/peripheral-anon") {
                list << QJsonObject{{"name", "device[0]"}, {"type", "child<qemu-xhci>"}};
            }
            for (const Device &d : std::as_const(devices)) {
                if (d.path.section('/', 0, 2) == path) {
                    list << QJsonObject{{"name", d.path.section('/', 3)},
                                        {"type", "child<" + d.type + ">"}};
                }
            }
            return list;
        }
        if (name == "qom-get") {
            const QString path = args["path"].toString();
            const QString property = args["property"].toString();
            for (const Device &d : std::as_const(devices)) {
                if (d.path != path) {
                    continue;
                }
                const QHash<QString, QJsonValue> values{
                    {"hostbus", d.bus},       {"hostaddr", d.address}, {"vendorid", d.vendor},
                    {"productid", d.product}, {"hostport", d.port},    {"hostdevice", d.node},
                    {"attached", d.attached}};
                if (values.contains(property)) {
                    return values[property];
                }
                *failure = error("GenericError",
                                 QString("Property '%1.%2' not found").arg(d.type, property));
                return {};
            }
            *failure = error("DeviceNotFound", QString("Device '%1' not found").arg(path));
            return {};
        }
        if (name == "device_add") {
            const QString driver = args["driver"].toString();
            const QString id = args["id"].toString();
            const QString at = QString("%1/%2").arg(args["hostbus"].toInt())
                                   .arg(args["hostaddr"].toInt());
            if (driver == "usb-host" && !libusb) {
                *failure = error("GenericError",
                                 "'usb-host' is not a valid device model name");
                return {};
            }
            if (!controller) {
                *failure = error("GenericError",
                                 QString("No 'usb-bus' bus found for device '%1'").arg(driver));
                return {};
            }
            for (const Device &d : std::as_const(devices)) {
                if (d.path == "/machine/peripheral/" + id) {
                    *failure = error("GenericError",
                                     QString("Duplicate device ID '%1' for device").arg(id));
                    return {};
                }
            }
            if (!plugged.contains(at)) {
                *failure = error("GenericError", QString("failed to find host usb device %1")
                                                     .arg(QString(at).replace('/', ':')));
                return {};
            }
            if (busy.contains(at)) {
                *failure = error("GenericError", QString("failed to open host usb device %1")
                                                     .arg(QString(at).replace('/', ':')));
                return {};
            }
            Device d;
            d.path = "/machine/peripheral/" + id;
            d.bus = args["hostbus"].toInt();
            d.address = args["hostaddr"].toInt();
            d.attached = true;
            devices << d;
            return QJsonObject();
        }
        if (name == "device_del") {
            const QString id = args["id"].toString();
            const QString path = id.startsWith('/') ? id : "/machine/peripheral/" + id;
            for (const Device &d : std::as_const(devices)) {
                if (d.path == path) {
                    remove(path, &m_deleted);
                    return QJsonObject();
                }
            }
            *failure = error("DeviceNotFound", QString("Device '%1' not found").arg(id));
            return {};
        }
        return QJsonObject();
    }

    void read(QLocalSocket *peer)
    {
        QByteArray &buffer = m_buffers[peer];
        buffer += peer->readAll();
        qsizetype nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            const QJsonObject command = QJsonDocument::fromJson(buffer.left(nl)).object();
            buffer.remove(0, nl + 1);
            if (command["execute"] != "qmp_capabilities") {
                commands << command;
            }
            QJsonObject failure;
            const QJsonValue result = answer(command, &failure);
            QJsonObject reply{{"id", command["id"]}};
            if (failure.isEmpty()) {
                reply["return"] = result.isUndefined() ? QJsonValue(QJsonObject()) : result;
            } else {
                reply["error"] = failure;
            }
            peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
            for (const QJsonObject &data : std::exchange(m_deleted, {})) {
                event("DEVICE_DELETED", data);
            }
        }
    }

    QLocalServer m_server;
    QList<QJsonObject> m_deleted;
    QList<QLocalSocket *> m_peers;
    QHash<QLocalSocket *, QByteArray> m_buffers;
};
