// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <sys/types.h>

class QSocketNotifier;
class QTimer;

/*
 * One run of vitrine-helper (pkexec vitrine-helper VERB, or a command
 * standing in for it in tests): its stdin, stdout and stderr one end of a
 * socket pair - a write to a helper that went away gives EPIPE, never a
 * SIGPIPE that would end vitrine - in a session of its own (Ctrl+C in
 * vitrine's terminal is not for it).  Requests wait for its "ready" line;
 * each line it writes comes as line(); finished() once it has exited,
 * reaped.  Not waited for when this goes: the helper is root's, and goes on
 * alone with what it watches.
 */
class HelperSession : public QObject
{
    Q_OBJECT

public:
    explicit HelperSession(QObject *parent = nullptr);
    ~HelperSession() override;

    /* Runs @command: false with why in @error */
    bool start(const QStringList &command, QString *error);
    /* @line sent (a newline after it) once the helper is ready */
    void send(const QByteArray &line);
    /* Its input ends, once what was sent has gone: it goes on with what it
       watches, and ends after it */
    void closeInput();
    /* Its input ends now, what was not sent yet dropped */
    void cancel();
    bool running() const { return m_pid > 0; }
    bool ready() const { return m_ready; }
    /* The lines that are no reply of the helper's: pkexec's, before it ran */
    QStringList foreign() const { return m_foreign; }
    /* Its exit status once finished(): 128 + the signal for a signal */
    int exitCode() const { return m_exitCode; }

signals:
    void line(const QString &line);
    void finished();

private:
    void readable();
    void flush();
    void reap();
    void closeAll();

    pid_t m_pid = 0;
    int m_fd = -1;
    int m_pidfd = -1;
    int m_exitCode = -1;
    QSocketNotifier *m_readable = nullptr;
    QSocketNotifier *m_writable = nullptr;
    QSocketNotifier *m_exited = nullptr;
    QTimer *m_exitPoll = nullptr;
    QByteArray m_in;
    QByteArray m_out;
    bool m_ready = false;
    bool m_closeInput = false;
    bool m_inputClosed = false;
    QStringList m_foreign;
};
