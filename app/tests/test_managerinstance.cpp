// SPDX-License-Identifier: GPL-2.0-or-later
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/managerinstance.h"

/*
 * One manager at a time, the Qt app or the GTK app: the first takes the
 * lock and listens, a later one finds the lock taken and asks the first to
 * show its window, with its activation token; a lock left by a manager
 * that died is taken over.
 */
class TestManagerInstance : public QObject
{
    Q_OBJECT

private slots:
    void secondAsksTheFirst()
    {
        QTemporaryDir dir;
        ManagerInstance first(dir.path());
        ManagerInstance second(dir.path());
        QSignalSpy asked(&first, &ManagerInstance::activationRequested);
        QString error;

        QCOMPARE(first.socketPath(), dir.path() + "/manager.sock");
        QCOMPARE(first.lockPath(), dir.path() + "/manager.lock");
        QVERIFY(first.lock());
        /* held: and again by its holder */
        QVERIFY(first.lock());
        QVERIFY(!second.lock());
        QVERIFY2(first.listen(&error), qPrintable(error));

        QVERIFY(second.activateRunning("token-1"));
        QTRY_COMPARE(asked.size(), 1);
        QCOMPARE(asked.at(0).at(0).toByteArray(), QByteArray("token-1"));
        /* none to pass on: the line says activate alone */
        QVERIFY(second.activateRunning({}));
        QTRY_COMPARE(asked.size(), 2);
        QCOMPARE(asked.at(1).at(0).toByteArray(), QByteArray());
    }

    void lockOfADeadManager()
    {
        QTemporaryDir dir;
        QFile stale(dir.path() + "/manager.lock");

        /* as QLockFile writes it: a process that is no more */
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("2147483646\nvitrine\n" + QSysInfo::machineHostName().toUtf8() + "\n");
        stale.close();

        ManagerInstance instance(dir.path());
        QVERIFY(instance.lock());
    }

    void nobodyAnswers()
    {
        QTemporaryDir dir;
        ManagerInstance instance(dir.path());
        QElapsedTimer timer;

        timer.start();
        QVERIFY(!instance.activateRunning("token"));
        /* it waited for a manager still starting */
        QVERIFY(timer.elapsed() >= 2500);
    }
};

QTEST_GUILESS_MAIN(TestManagerInstance)
#include "test_managerinstance.moc"
