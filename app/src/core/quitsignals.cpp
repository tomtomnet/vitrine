// SPDX-License-Identifier: GPL-2.0-or-later
#include "quitsignals.h"

#include <QSocketNotifier>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr std::array kSignals = {SIGTERM, SIGINT, SIGHUP};

/*
 * The handler writes to [1], the notifier reads [0].  Made once and kept
 * for the life of the process: a handler that runs on another thread as
 * the QuitSignals goes never writes to a descriptor closed, or reused.
 */
int s_fds[2] = {-1, -1};
/* What each signal had before, and whether the handler is the one now */
struct sigaction s_previous[kSignals.size()];
bool s_installed[kSignals.size()] = {};
const QuitSignals *s_instance = nullptr;

void handler(int signal)
{
    const int saved = errno;
    const char number = char(signal);

    (void)!::write(s_fds[1], &number, 1);
    errno = saved;
}

bool ignored(const struct sigaction &action)
{
    return !(action.sa_flags & SA_SIGINFO) && action.sa_handler == SIG_IGN;
}

}

QuitSignals::QuitSignals(QObject *parent) : QObject(parent)
{
    if (s_instance) {
        qWarning("vitrine: SIGTERM, SIGINT and SIGHUP are handled already");
        return;
    }
    if (s_fds[0] < 0 &&
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, s_fds) != 0) {
        qWarning("vitrine: SIGTERM, SIGINT and SIGHUP end vitrine at once: no socket pair (%s)",
                 strerror(errno));
        s_fds[0] = s_fds[1] = -1;
        return;
    }
    s_instance = this;
    /* what an earlier one left unread is not for this one */
    char drop[16];
    while (::read(s_fds[0], drop, sizeof drop) > 0) {
    }
    m_notifier = new QSocketNotifier(s_fds[0], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &QuitSignals::read);

    struct sigaction action = {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    /* the same signal again: its default action, at once */
    action.sa_flags = SA_RESTART | SA_RESETHAND;
    for (size_t i = 0; i < kSignals.size(); i++) {
        s_installed[i] = ::sigaction(kSignals[i], nullptr, &s_previous[i]) == 0 &&
                         !ignored(s_previous[i]) &&
                         ::sigaction(kSignals[i], &action, nullptr) == 0;
    }
}

QuitSignals::~QuitSignals()
{
    if (s_instance != this) {
        return;
    }
    for (size_t i = 0; i < kSignals.size(); i++) {
        if (std::exchange(s_installed[i], false)) {
            ::sigaction(kSignals[i], &s_previous[i], nullptr);
        }
    }
    s_instance = nullptr;
}

QList<int> QuitSignals::handled()
{
    return QList<int>(kSignals.begin(), kSignals.end());
}

bool QuitSignals::handles(int signal) const
{
    struct sigaction now = {};

    return s_instance == this && ::sigaction(signal, nullptr, &now) == 0 &&
           !(now.sa_flags & SA_SIGINFO) && now.sa_handler == handler;
}

void QuitSignals::read()
{
    char number;

    while (::read(s_fds[0], &number, 1) == 1) {
        emit received(number);
    }
}
