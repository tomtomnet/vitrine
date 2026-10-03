// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>

#include <functional>

/*
 * A QMP connection over a unix socket: reads the greeting, negotiates the
 * capabilities, matches the replies to the commands by id, and forwards
 * the events.
 */
class QmpClient : public QObject
{
    Q_OBJECT

public:
    /* @error is empty on success, else the QMP error description */
    using Callback = std::function<void(const QJsonValue &result, const QString &error)>;

    explicit QmpClient(QObject *parent = nullptr);
    ~QmpClient() override;

    void connectToSocket(const QString &path);
    /* Fails the pending commands, without disconnected() */
    void disconnectFromSocket();
    /* Connected, capabilities negotiated */
    bool isReady() const;
    /* Queued until ready; the callback gets an error if the connection drops */
    void execute(const QString &command, const QJsonObject &arguments = {},
                 Callback callback = {});

signals:
    void ready();
    /* Could not connect, or the connection closed before ready(): the
       queued commands wait for the next connection */
    void connectionFailed(const QString &error);
    /* The connection closed after ready(): the pending commands failed */
    void disconnected();
    void qmpEvent(const QString &name, const QJsonObject &data);

private:
    struct Private;
    Private *d;
};
