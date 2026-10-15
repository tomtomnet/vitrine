// SPDX-License-Identifier: GPL-2.0-or-later
#include <QSignalSpy>
#include <QTest>

#include <csignal>

#include "core/quitsignals.h"

namespace {

/* What @signal does now: its handler, or SIG_DFL, SIG_IGN */
void *actionOf(int signal)
{
    struct sigaction now = {};

    if (::sigaction(signal, nullptr, &now) != 0) {
        return nullptr;
    }
    return (now.sa_flags & SA_SIGINFO) ? reinterpret_cast<void *>(now.sa_sigaction)
                                       : reinterpret_cast<void *>(now.sa_handler);
}

void *defaultAction()
{
    return reinterpret_cast<void *>(SIG_DFL);
}

}

/*
 * SIGTERM, SIGINT and SIGHUP reach the event loop as received(), from a
 * handler that only writes to a socket; the signal that came gets its
 * default action back, so that the same one again ends the process at
 * once; a signal ignored beforehand (nohup) stays ignored; what the
 * signals did before comes back when it goes.
 */
class TestQuitSignals : public QObject
{
    Q_OBJECT

private slots:
    void received_data()
    {
        QTest::addColumn<int>("signal");
        QTest::newRow("SIGTERM") << SIGTERM;
        QTest::newRow("SIGINT") << SIGINT;
        QTest::newRow("SIGHUP") << SIGHUP;
    }

    void received()
    {
        QFETCH(int, signal);
        void *const before = actionOf(signal);
        {
            QuitSignals quit;
            QSignalSpy spy(&quit, &QuitSignals::received);

            QCOMPARE(QuitSignals::handled(), QList<int>({SIGTERM, SIGINT, SIGHUP}));
            QVERIFY(quit.handles(signal));
            QCOMPARE(::raise(signal), 0);
            /* not from the handler: from the event loop */
            QVERIFY(spy.isEmpty());
            QTRY_COMPARE(spy.size(), 1);
            QCOMPARE(spy[0][0].toInt(), signal);
            /* the same signal again would end the process at once */
            QCOMPARE(actionOf(signal), defaultAction());
            QVERIFY(!quit.handles(signal));
            /* the others still come here */
            for (int other : QuitSignals::handled()) {
                QVERIFY(other == signal || quit.handles(other));
            }
        }
        QCOMPARE(actionOf(signal), before);
    }

    /* SIGTERM and SIGHUP at once, as systemd ends the processes of a login
       session: both come, one after the other */
    void termThenHup()
    {
        QuitSignals quit;
        QSignalSpy spy(&quit, &QuitSignals::received);

        QCOMPARE(::raise(SIGTERM), 0);
        QCOMPARE(::raise(SIGHUP), 0);
        QTRY_COMPARE(spy.size(), 2);
        QCOMPARE(spy[0][0].toInt(), SIGTERM);
        QCOMPARE(spy[1][0].toInt(), SIGHUP);
    }

    /* Ignored before it, as nohup leaves SIGHUP: ignored still */
    void ignoredStaysIgnored()
    {
        struct sigaction ignore = {};
        struct sigaction before = {};

        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        QCOMPARE(::sigaction(SIGHUP, &ignore, &before), 0);
        {
            QuitSignals quit;
            QSignalSpy spy(&quit, &QuitSignals::received);

            QVERIFY(!quit.handles(SIGHUP));
            QVERIFY(quit.handles(SIGTERM));
            QCOMPARE(::raise(SIGHUP), 0);
            QTest::qWait(100);
            QVERIFY(spy.isEmpty());
        }
        QCOMPARE(actionOf(SIGHUP), reinterpret_cast<void *>(SIG_IGN));
        QCOMPARE(::sigaction(SIGHUP, &before, nullptr), 0);
    }

    /* One at a time: a second one handles nothing, and leaves the first's alone */
    void oneAtATime()
    {
        QuitSignals first;
        {
            QTest::ignoreMessage(QtWarningMsg,
                                 "vitrine: SIGTERM, SIGINT and SIGHUP are handled already");
            QuitSignals second;
            QVERIFY(!second.handles(SIGTERM));
        }
        QVERIFY(first.handles(SIGTERM));
        QSignalSpy spy(&first, &QuitSignals::received);
        QCOMPARE(::raise(SIGTERM), 0);
        QTRY_COMPARE(spy.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestQuitSignals)
#include "test_quitsignals.moc"
