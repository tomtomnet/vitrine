// SPDX-License-Identifier: GPL-2.0-or-later
#include "helpersession.h"

#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* The words the helper's lines start with ("error:" the helper's own, as
   "error: cannot use /run/vitrine-helper"): any other line is pkexec's */
static const QStringList kProtocol{"ready",  "ok",       "skip", "error", "error:",
                                   "exited", "restored", "left", "bye"};

HelperSession::HelperSession(QObject *parent) : QObject(parent) {}

HelperSession::~HelperSession()
{
    /* the helper sees its input end and goes on with what it watches */
    closeAll();
    if (m_pid > 0) {
        waitpid(m_pid, nullptr, WNOHANG);
    }
}

bool HelperSession::start(const QStringList &command, QString *error)
{
    std::vector<QByteArray> args;
    std::vector<char *> argv;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    sigset_t none, all;
    int sv[2], err;
    pid_t pid;

    if (m_pid > 0 || m_fd >= 0 || command.isEmpty()) {
        *error = tr("vitrine-helper cannot be started twice");
        return false;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        *error = QString::fromLocal8Bit(strerror(errno));
        return false;
    }
    for (const QString &arg : command) {
        args.push_back(arg.toLocal8Bit());
    }
    for (QByteArray &arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDERR_FILENO);
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK |
                                        POSIX_SPAWN_SETSIGDEF);
    err = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(sv[1]);
    if (err) {
        ::close(sv[0]);
        *error = tr("cannot run %1: %2").arg(command.first(), QString::fromLocal8Bit(strerror(err)));
        return false;
    }
    m_pid = pid;
    m_fd = sv[0];
    fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    m_readable = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_readable, &QSocketNotifier::activated, this, &HelperSession::readable);
    m_writable = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    m_writable->setEnabled(false);
    connect(m_writable, &QSocketNotifier::activated, this, &HelperSession::flush);
    m_pidfd = int(syscall(SYS_pidfd_open, pid, 0));
    if (m_pidfd >= 0) {
        m_exited = new QSocketNotifier(m_pidfd, QSocketNotifier::Read, this);
        connect(m_exited, &QSocketNotifier::activated, this, &HelperSession::reap);
    } else {
        /* kernels before 5.3 */
        m_exitPoll = new QTimer(this);
        connect(m_exitPoll, &QTimer::timeout, this, &HelperSession::reap);
        m_exitPoll->start(500);
    }
    return true;
}

void HelperSession::send(const QByteArray &line)
{
    if (m_closeInput) {
        return;
    }
    m_out += line + '\n';
    flush();
}

void HelperSession::closeInput()
{
    m_closeInput = true;
    flush();
}

void HelperSession::cancel()
{
    m_out.clear();
    closeInput();
}

void HelperSession::readable()
{
    char buf[4096];
    qsizetype nl;

    if (m_fd < 0) {
        return;
    }
    for (;;) {
        const ssize_t n = ::read(m_fd, buf, sizeof(buf));
        if (n > 0) {
            m_in.append(buf, n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            if (n == 0 && m_readable) {
                /* the end of its output: its exit follows */
                m_readable->setEnabled(false);
            }
            break;
        }
    }
    while ((nl = m_in.indexOf('\n')) >= 0) {
        const QString text = QString::fromUtf8(m_in.left(nl)).trimmed();
        const QString word = text.section(' ', 0, 0);

        m_in.remove(0, nl + 1);
        if (!kProtocol.contains(word)) {
            /* pkexec's, before the helper runs: why it did not */
            if (!text.isEmpty()) {
                m_foreign << text;
            }
        } else if (word == "ready" && !m_ready) {
            m_ready = true;
            flush();
        }
        emit line(text);
    }
}

void HelperSession::flush()
{
    if (m_fd < 0) {
        return;
    }
    /* the requests wait for the helper itself, not pkexec */
    while (m_ready && !m_out.isEmpty()) {
        const ssize_t n = ::send(m_fd, m_out.constData(), size_t(m_out.size()),
                                 MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            m_out.remove(0, n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            /* full: the rest when it can take it; gone: its exit follows */
            if (n < 0 && errno == EAGAIN && m_writable) {
                m_writable->setEnabled(true);
                return;
            }
            m_out.clear();
            break;
        }
    }
    if (m_writable) {
        m_writable->setEnabled(false);
    }
    if (m_closeInput && m_out.isEmpty() && !m_inputClosed) {
        m_inputClosed = true;
        ::shutdown(m_fd, SHUT_WR);
    }
}

void HelperSession::reap()
{
    int status = 0;
    pid_t r;

    if (m_pid <= 0 || (r = waitpid(m_pid, &status, WNOHANG)) == 0) {
        return;
    }
    /* what it said last */
    readable();
    m_exitCode = r < 0                ? -1
                 : WIFEXITED(status)  ? WEXITSTATUS(status)
                 : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                      : -1;
    m_pid = 0;
    closeAll();
    emit finished();
}

void HelperSession::closeAll()
{
    for (QSocketNotifier *notifier : {m_readable, m_writable, m_exited}) {
        if (notifier) {
            notifier->setEnabled(false);
            notifier->deleteLater();
        }
    }
    if (m_exitPoll) {
        m_exitPoll->stop();
        m_exitPoll->deleteLater();
    }
    m_readable = m_writable = m_exited = nullptr;
    m_exitPoll = nullptr;
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_pidfd >= 0) {
        ::close(m_pidfd);
        m_pidfd = -1;
    }
    m_ready = false;
}
