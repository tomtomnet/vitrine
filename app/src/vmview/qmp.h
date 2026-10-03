// Minimal synchronous QMP client over an already connected socket, with
// SCM_RIGHTS fd passing (needed for getfd + add_client of the D-Bus display).
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class Qmp
{
public:
    explicit Qmp(int fd) : m_fd(fd) {}
    ~Qmp();

    bool handshake(QString *err, int timeoutMs = 10000);
    // Runs one command; passes sendFd along with it when >= 0.
    bool execute(const QString &command, const QJsonObject &args, QJsonObject *ret,
                 QString *err, int sendFd = -1, int timeoutMs = 10000);
    int fd() const { return m_fd; }

private:
    bool readMessage(QJsonObject *msg, int timeoutMs, QString *err);
    bool sendAll(const QByteArray &data, int sendFd, QString *err);

    int m_fd;
    QByteArray m_buf;
};
