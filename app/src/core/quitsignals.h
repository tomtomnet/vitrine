// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>

class QSocketNotifier;

/*
 * SIGTERM (systemd ending the user's session, kill), SIGINT (Ctrl+C in
 * vitrine's terminal) and SIGHUP (that terminal closed) as requests to
 * quit, which the app answers in its GUI thread: unhandled, they end it as
 * a crash does.  The handler does only what is safe in a handler: it
 * writes the signal's number to a socket pair, which a QSocketNotifier
 * reads in the GUI thread, saying received().
 *
 * Each signal gets its default action back as it arrives (SA_RESETHAND),
 * so that the same signal again ends the app at once, whatever the first
 * one began.  Per signal: systemd sends SIGHUP right after SIGTERM to the
 * processes of a login session (SendSIGHUP=yes), one request in two
 * signals.  A signal ignored when the app started (SIGHUP under nohup)
 * stays ignored.  One at a time; when it goes, the signals get back what
 * they had before it.
 */
class QuitSignals : public QObject
{
    Q_OBJECT

public:
    explicit QuitSignals(QObject *parent = nullptr);
    ~QuitSignals() override;

    /* SIGTERM, SIGINT and SIGHUP */
    static QList<int> handled();
    /* Its handler is in place for @signal: not after @signal came once, nor
       for a signal ignored before */
    bool handles(int signal) const;

signals:
    /* @signal arrived: SIGTERM, SIGINT or SIGHUP; each one that arrives */
    void received(int signal);

private:
    void read();

    QSocketNotifier *m_notifier = nullptr;
};
