#include "qmp.h"

#include <QJsonDocument>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

Qmp::~Qmp()
{
    if (m_fd >= 0) {
        close(m_fd);
    }
}

bool Qmp::sendAll(const QByteArray &data, int sendFd, QString *err)
{
    size_t off = 0;
    while (off < size_t(data.size())) {
        iovec iov{const_cast<char *>(data.constData()) + off, size_t(data.size()) - off};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int))];
        if (sendFd >= 0 && off == 0) {
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            cmsghdr *c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &sendFd, sizeof(int));
        }
        ssize_t n = sendmsg(m_fd, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *err = QStringLiteral("QMP send: %1").arg(strerror(errno));
            return false;
        }
        off += size_t(n);
    }
    return true;
}

bool Qmp::readMessage(QJsonObject *msg, int timeoutMs, QString *err)
{
    for (;;) {
        int nl = m_buf.indexOf('\n');
        if (nl >= 0) {
            QByteArray line = m_buf.left(nl);
            m_buf.remove(0, nl + 1);
            QJsonParseError pe;
            QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
            if (!doc.isObject()) {
                continue;
            }
            *msg = doc.object();
            return true;
        }
        pollfd p{m_fd, POLLIN, 0};
        int r = poll(&p, 1, timeoutMs);
        if (r == 0) {
            *err = QStringLiteral("QMP: timeout");
            return false;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            *err = QStringLiteral("QMP poll: %1").arg(strerror(errno));
            return false;
        }
        char tmp[4096];
        ssize_t n = read(m_fd, tmp, sizeof(tmp));
        if (n <= 0) {
            *err = QStringLiteral("QMP: connection closed");
            return false;
        }
        m_buf.append(tmp, n);
    }
}

bool Qmp::handshake(QString *err, int timeoutMs)
{
    QJsonObject greeting;
    if (!readMessage(&greeting, timeoutMs, err)) {
        return false;
    }
    if (!greeting.contains(QLatin1String("QMP"))) {
        *err = QStringLiteral("QMP: unexpected greeting");
        return false;
    }
    QJsonObject ret;
    return execute(QStringLiteral("qmp_capabilities"), {}, &ret, err);
}

bool Qmp::execute(const QString &command, const QJsonObject &args, QJsonObject *ret,
                  QString *err, int sendFd, int timeoutMs)
{
    QJsonObject cmd{{QStringLiteral("execute"), command}};
    if (!args.isEmpty()) {
        cmd.insert(QStringLiteral("arguments"), args);
    }
    QByteArray data = QJsonDocument(cmd).toJson(QJsonDocument::Compact) + '\n';
    if (!sendAll(data, sendFd, err)) {
        return false;
    }
    for (;;) {
        QJsonObject msg;
        if (!readMessage(&msg, timeoutMs, err)) {
            return false;
        }
        if (msg.contains(QLatin1String("event"))) {
            continue; // events are not interesting for the PoC
        }
        if (msg.contains(QLatin1String("error"))) {
            *err = QStringLiteral("QMP %1: %2").arg(command,
                msg.value(QLatin1String("error")).toObject()
                    .value(QLatin1String("desc")).toString());
            return false;
        }
        if (ret) {
            *ret = msg;
        }
        return true;
    }
}
