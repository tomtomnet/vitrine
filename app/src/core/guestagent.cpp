// SPDX-License-Identifier: GPL-2.0-or-later
#include "guestagent.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>

GuestAgent::GuestAgent(QObject *parent) : QObject(parent), m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        fail(tr("The guest agent does not answer: is qemu-guest-agent running in the guest?"));
    });
}

GuestAgent::~GuestAgent()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

void GuestAgent::connectToSocket(const QString &path, int timeoutMs)
{
    disconnectFromSocket();
    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::readyRead, this, &GuestAgent::read);
    connect(m_socket, &QLocalSocket::connected, this, [this]() {
        /* 0xFF resets the parser of the agent, which answers with a 0xFF
           first, where what an earlier client left ends */
        m_socket->write("\xff");
        m_syncId = qint64(QDateTime::currentMSecsSinceEpoch() % 1000000000);
        send(-1, "guest-sync-delimited", {{"id", m_syncId}});
    });
    connect(m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        fail(tr("Cannot talk to the guest agent: %1").arg(m_socket->errorString()));
    });
    m_timeout->start(timeoutMs);
    m_socket->connectToServer(path);
}

void GuestAgent::disconnectFromSocket()
{
    m_timeout->stop();
    m_buffer.clear();
    m_syncId = -1;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    const auto pending = std::exchange(m_pending, {});
    for (const Callback &callback : pending) {
        callback({}, tr("Disconnected from the guest agent"));
    }
}

void GuestAgent::send(qint64 id, const QString &command, const QJsonObject &arguments)
{
    QJsonObject message{{"execute", command}};

    if (!arguments.isEmpty()) {
        message["arguments"] = arguments;
    }
    if (id >= 0) {
        message["id"] = id;
    }
    m_socket->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void GuestAgent::execute(const QString &command, const QJsonObject &arguments,
                         const Callback &callback)
{
    if (!m_socket || m_syncId >= 0) {
        callback({}, tr("Not connected to the guest agent"));
        return;
    }
    const qint64 id = m_nextId++;
    m_pending.insert(id, callback);
    send(id, command, arguments);
}

void GuestAgent::read()
{
    const QPointer<GuestAgent> self(this);
    qsizetype newline;

    m_buffer += m_socket->readAll();
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        /* what an earlier client left, up to a 0xFF, is dropped */
        QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        line = line.mid(line.lastIndexOf('\xff') + 1);

        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        if (reply.isEmpty()) {
            continue;
        }
        if (m_syncId >= 0) {
            if (reply["return"].toInteger(-1) == m_syncId) {
                m_syncId = -1;
                m_timeout->stop();
                emit ready();
                if (!self) {
                    return;
                }
            }
            continue;
        }
        const Callback callback = m_pending.take(reply["id"].toInteger(-1));
        if (callback) {
            const QJsonObject e = reply["error"].toObject();
            callback(reply["return"], e.isEmpty() ? QString() : e["desc"].toString("error"));
            if (!self || !m_socket) {
                return;
            }
        }
    }
}

void GuestAgent::exec(const QString &path, const QStringList &args,
                      const std::function<void(const ExecResult &)> &done, int timeoutMs)
{
    QElapsedTimer clock;

    clock.start();
    execute("guest-exec",
            {{"path", path}, {"arg", QJsonArray::fromStringList(args)}, {"capture-output", true}},
            [this, path, done, clock, timeoutMs](const QJsonValue &result, const QString &error) {
        if (!error.isEmpty()) {
            done(ExecResult{-1, {}, {}, error});
            return;
        }
        pollExec(result["pid"].toInteger(), path, done, clock, timeoutMs);
    });
}

/* Until it has exited */
void GuestAgent::pollExec(qint64 pid, const QString &path,
                          const std::function<void(const ExecResult &)> &done,
                          const QElapsedTimer &clock, int timeoutMs)
{
    execute("guest-exec-status", {{"pid", pid}},
            [=, this](const QJsonValue &status, const QString &error) {
        if (!error.isEmpty()) {
            done(ExecResult{-1, {}, {}, error});
        } else if (status["exited"].toBool()) {
            auto text = [&](const char *key) {
                return QString::fromUtf8(QByteArray::fromBase64(status[key].toString().toLatin1()));
            };
            done(ExecResult{status["exitcode"].toInt(-1), text("out-data"), text("err-data"), {}});
        } else if (clock.elapsed() > timeoutMs) {
            done(ExecResult{-1, {}, {}, tr("%1 did not end").arg(path)});
        } else {
            QTimer::singleShot(100, this, [=, this]() { pollExec(pid, path, done, clock, timeoutMs); });
        }
    });
}

void GuestAgent::fail(const QString &error)
{
    const QPointer<GuestAgent> self(this);

    disconnectFromSocket();
    if (self) {
        emit failed(error);
    }
}
