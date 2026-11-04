// SPDX-License-Identifier: GPL-2.0-or-later
#include "managerinstance.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>

ManagerInstance::ManagerInstance(const QString &dir, QObject *parent)
    : QObject(parent), m_dir(dir)
{
}

ManagerInstance::~ManagerInstance() = default;

QString ManagerInstance::lockPath() const
{
    return m_dir + "/manager.lock";
}

QString ManagerInstance::socketPath() const
{
    return m_dir + "/manager.sock";
}

bool ManagerInstance::lock()
{
    if (m_lock && m_lock->isLocked()) {
        return true;
    }
    m_lock = std::make_unique<QLockFile>(lockPath());
    return m_lock->tryLock(100);
}

/* One try: connected, the line written */
static bool askToActivate(const QString &socketPath, const QByteArray &token)
{
    QLocalSocket socket;

    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(1000)) {
        return false;
    }
    socket.write("activate " + token + '\n');
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

bool ManagerInstance::activateRunning(const QByteArray &token) const
{
    /* the other one may be starting still */
    for (int i = 0; i < 30; i++) {
        if (askToActivate(socketPath(), token)) {
            return true;
        }
        QThread::msleep(100);
    }
    return false;
}

bool ManagerInstance::listen(QString *error)
{
    if (m_server) {
        return m_server->isListening();
    }
    m_server = new QLocalServer(this);
    QLocalServer::removeServer(socketPath());
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(socketPath())) {
        if (error) {
            *error = m_server->errorString();
        }
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *client = m_server->nextPendingConnection()) {
            connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            connect(client, &QLocalSocket::readyRead, this, [this, client]() {
                while (client->canReadLine()) {
                    const QByteArray line = client->readLine().trimmed();
                    if (line == "activate" || line.startsWith("activate ")) {
                        emit activationRequested(line.mid(9).trimmed());
                    }
                }
            });
        }
    });
    return true;
}
