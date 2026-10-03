// SPDX-License-Identifier: GPL-2.0-or-later
#include "qmpclient.h"

#include <QHash>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>

#include <utility>

struct QmpClient::Private
{
    struct Command {
        qint64 id;
        QString name;
        QJsonObject arguments;
        Callback callback;
    };

    QmpClient *q;
    QLocalSocket *socket = nullptr;
    QByteArray buffer;
    bool connected = false;
    bool ready = false;
    qint64 nextId = 1;
    qint64 capabilitiesId = -1;
    QList<Command> queued;              // until ready
    QHash<qint64, Callback> sent;       // until their reply

    void send(qint64 id, const QString &command, const QJsonObject &arguments);
    void read();
    void handle(const QJsonObject &message);
    void closed();
    void drop();
    void failAll(const QString &error);
};

void QmpClient::Private::send(qint64 id, const QString &command,
                              const QJsonObject &arguments)
{
    QJsonObject message{{"execute", command}, {"id", id}};

    if (!arguments.isEmpty()) {
        message["arguments"] = arguments;
    }
    socket->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void QmpClient::Private::read()
{
    const QPointer<QmpClient> self(q);
    qsizetype newline;

    buffer += socket->readAll();
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        const QJsonDocument doc = QJsonDocument::fromJson(line);

        buffer.remove(0, newline + 1);
        if (!doc.isObject()) {
            continue;
        }
        handle(doc.object());
        /* a callback may have closed or deleted us */
        if (!self || !socket) {
            return;
        }
    }
}

void QmpClient::Private::handle(const QJsonObject &message)
{
    if (message.contains("QMP")) {
        capabilitiesId = nextId++;
        send(capabilitiesId, "qmp_capabilities", {});
        return;
    }
    if (message.contains("event")) {
        emit q->qmpEvent(message["event"].toString(), message["data"].toObject());
        return;
    }
    if (!message.contains("return") && !message.contains("error")) {
        return;
    }

    const qint64 id = message["id"].toInteger(-1);
    QString error;

    if (message.contains("error")) {
        const QJsonObject e = message["error"].toObject();
        error = e["desc"].toString(e["class"].toString("error"));
    }
    if (!ready && id == capabilitiesId) {
        if (!error.isEmpty()) {
            drop();
            emit q->connectionFailed(error);
            return;
        }
        ready = true;
        for (const Command &c : std::exchange(queued, {})) {
            if (c.callback) {
                sent.insert(c.id, c.callback);
            }
            send(c.id, c.name, c.arguments);
        }
        emit q->ready();
        return;
    }

    const Callback callback = sent.take(id);
    if (callback) {
        callback(message["return"], error);
    }
}

void QmpClient::Private::closed()
{
    const bool wasReady = ready;

    drop();
    if (wasReady) {
        const QPointer<QmpClient> self(q);
        failAll(QmpClient::tr("The connection to QEMU closed"));
        if (self) {
            emit q->disconnected();
        }
    } else {
        emit q->connectionFailed(QmpClient::tr("QEMU closed the connection"));
    }
}

/* Closes the socket quietly */
void QmpClient::Private::drop()
{
    if (socket) {
        QObject::disconnect(socket, nullptr, q, nullptr);
        socket->abort();
        socket->deleteLater();
        socket = nullptr;
    }
    buffer.clear();
    connected = false;
    ready = false;
    capabilitiesId = -1;
    /* the replies to what was sent are lost with the connection */
}

void QmpClient::Private::failAll(const QString &error)
{
    const QPointer<QmpClient> self(q);
    QList<Callback> callbacks = sent.values();

    for (const Command &c : std::as_const(queued)) {
        if (c.callback) {
            callbacks << c.callback;
        }
    }
    sent.clear();
    queued.clear();
    for (const Callback &callback : std::as_const(callbacks)) {
        callback(QJsonValue(), error);
        if (!self) {
            return;
        }
    }
}

QmpClient::QmpClient(QObject *parent) : QObject(parent), d(new Private)
{
    d->q = this;
}

QmpClient::~QmpClient()
{
    /* no callbacks: their owners are likely going away too */
    if (d->socket) {
        QObject::disconnect(d->socket, nullptr, this, nullptr);
        d->socket->abort();
        d->socket->setParent(nullptr);
        d->socket->deleteLater();
    }
    delete d;
}

void QmpClient::connectToSocket(const QString &path)
{
    const QPointer<QmpClient> self(this);

    if (d->connected) {
        d->drop();
        d->failAll(tr("Reconnecting to QEMU"));
        if (!self) {
            return;
        }
    } else {
        d->drop();
    }

    QLocalSocket *socket = new QLocalSocket(this);
    d->socket = socket;
    connect(socket, &QLocalSocket::connected, this, [this]() {
        d->connected = true;
    });
    connect(socket, &QLocalSocket::readyRead, this, [this]() {
        d->read();
    });
    connect(socket, &QLocalSocket::disconnected, this, [this]() {
        d->closed();
    });
    connect(socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        /* once connected, disconnected() follows */
        if (!d->connected && d->socket) {
            const QString error = d->socket->errorString();
            d->drop();
            emit connectionFailed(error);
        }
    });
    socket->connectToServer(path);
}

void QmpClient::disconnectFromSocket()
{
    d->drop();
    d->failAll(tr("Disconnected from QEMU"));
}

bool QmpClient::isReady() const
{
    return d->ready;
}

void QmpClient::execute(const QString &command, const QJsonObject &arguments,
                        Callback callback)
{
    const qint64 id = d->nextId++;

    if (!d->ready) {
        d->queued.append({id, command, arguments, callback});
        return;
    }
    if (callback) {
        d->sent.insert(id, callback);
    }
    d->send(id, command, arguments);
}
