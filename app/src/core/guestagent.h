// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QStringList>

#include <functional>

class QLocalSocket;
class QTimer;

/*
 * A connection to the QEMU guest agent, qemu-ga in the guest, through the
 * socket of its virtio-serial port: newline-delimited JSON as with QMP, but
 * no greeting.  A 0xFF byte and guest-sync-delimited first get past
 * whatever an earlier client left half-said, either way.
 */
class GuestAgent : public QObject
{
    Q_OBJECT

public:
    /* @error is empty on success */
    using Callback = std::function<void(const QJsonValue &result, const QString &error)>;

    struct ExecResult {
        int exitCode = -1;
        QString out;
        QString err;
        QString error;          // why the program could not run, or did not end
    };

    explicit GuestAgent(QObject *parent = nullptr);
    ~GuestAgent() override;

    /* ready() once in sync with the agent, else failed() */
    void connectToSocket(const QString &path, int timeoutMs = 10000);
    void disconnectFromSocket();
    void execute(const QString &command, const QJsonObject &arguments, const Callback &callback);
    /* Runs @path with @args in the guest, as root, and waits for it to end */
    void exec(const QString &path, const QStringList &args,
              const std::function<void(const ExecResult &)> &done, int timeoutMs = 30000);

signals:
    void ready();
    void failed(const QString &error);

private:
    void read();
    void send(qint64 id, const QString &command, const QJsonObject &arguments);
    void pollExec(qint64 pid, const QString &path,
                  const std::function<void(const ExecResult &)> &done,
                  const QElapsedTimer &clock, int timeoutMs);
    void fail(const QString &error);

    QLocalSocket *m_socket = nullptr;
    QTimer *m_timeout;
    QByteArray m_buffer;
    qint64 m_nextId = 1;
    qint64 m_syncId = -1;
    QHash<qint64, Callback> m_pending;
};
