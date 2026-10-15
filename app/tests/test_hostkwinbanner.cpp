// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include "activation.h"
#include "core/hostkwin.h"
#include "hostkwinstandins.h"
#include "ui/hostkwinbanner.h"

using namespace HostKwin;

/*
 * The recipe's stand-in (host/kwin/build.sh --print-recipe): each reading
 * of the system says itself in calls, and waits while hold is there (10 s
 * at most), for a reading that takes its time
 */
static const char kRecipe[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
echo "$*" >> "$here/calls"
for _ in $(seq 100); do [ -e "$here/hold" ] || break; sleep 0.1; done
printf 'fedora=44\ncopr=copr:copr.fedorainfracloud.org:group_kdesig:kde-beta\nset=hl39 hl39\nkey=k\n'
)sh";

/*
 * The banner of Vitrine's KWin left behind by Plasma or Qt, when Plasma is
 * updated by hand while vitrine runs (dnf, in a terminal): it shows when
 * vitrine comes back in front, from a reading in the background, one at a
 * time.  The system is the stand-ins' (rpm and dnf on a state folder).
 */
class TestHostKwinBanner : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_state;
    QString m_recipe;

    static bool write(const QString &path, const QByteArray &text)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
    }

    /* @name at @evr installed, as the stand-ins hold it */
    bool install(const QString &name, const QString &evr, const QString &digest,
                 const QString &repo = "fedora")
    {
        return write(m_state + "/installed/" + name,
                     QString("NAME=%1\nVERSION=%2\nRELEASE=%3\nARCH=x86_64\nDIGEST=%4\n"
                             "SOURCERPM=%5-%2-%3.src.rpm\nEPOCHNUM=0\nREPO=%6\n"
                             "INSTALLTIME=1760000000\nVENDOR=(none)\n")
                         .arg(name, evr.section('-', 0, 0), evr.section('-', 1), digest,
                              name.startsWith("kwin") ? "kwin" : name, repo)
                         .toUtf8());
    }

    /* The readings of the system started so far */
    int readings() const
    {
        QFile calls(m_recipe + "/calls");
        return calls.open(QIODevice::ReadOnly) ? int(calls.readAll().count('\n')) : 0;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        QStandardPaths::setTestModeEnabled(true);
        const QString bin = m_tmp.filePath("bin");
        m_state = m_tmp.filePath("state");
        m_recipe = m_tmp.filePath("kwin");
        QVERIFY(write(bin + "/rpm", kFakeRpm));
        QVERIFY(write(bin + "/dnf", kFakeDnf));
        for (const char *name : {"rpm", "dnf"}) {
            QFile::setPermissions(bin + '/' + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        QVERIFY(write(m_recipe + "/build.sh", kRecipe));
        qputenv("PATH", bin.toUtf8() + ":/usr/bin:/bin");
        qputenv("STATE", m_state.toUtf8());

        /* Vitrine's KWin for Plasma 6.7.91 and Qt 6.11.2, installed: its record */
        const QString out = m_tmp.filePath("out");
        Record record;
        record.evr = "6.7.91-1.hl39.fc44";
        record.series = "hl39";
        record.when = QDateTime::currentDateTimeUtc();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            record.digests.insert(name, QString("vitrine-%1").arg(name));
            QVERIFY(install(name, record.evr, record.digests.value(name), "@commandline"));
        }
        record.buildroot.insert("qt6-qtbase", "6.11.2");
        QVERIFY(QDir().mkpath(out));
        QVERIFY(record.write(Record::path(out)));
        QVERIFY(install("plasma-workspace", "6.7.91-1.fc44", "pw"));
        QVERIFY(install("qt6-qtbase", "6.11.2-3.fc44", "qt"));
        QVERIFY(write(m_tmp.filePath("os-release"), "ID=fedora\nVERSION_ID=44\n"));

        HostKwinMonitor *monitor = HostKwinMonitor::instance();
        monitor->setSourceDir(m_recipe);
        monitor->setOutDir(out);
        monitor->setOsRelease(m_tmp.filePath("os-release"));
        monitor->setProc(m_tmp.filePath("noproc"));
    }

    /*
     * Plasma updated from a terminal while vitrine runs: the banner comes
     * when vitrine is back in front, and goes the same way.  Without its
     * hook it stayed as it was until the next start.
     */
    void plasmaUpdatedByHand()
    {
        HostKwinMonitor *monitor = HostKwinMonitor::instance();
        QSignalSpy changed(monitor, &HostKwinMonitor::changed);
        /* in a window never shown: no activation of its own */
        QWidget window;
        auto *banner = new HostKwinBanner(&window);
        /* the system read at its start */
        QVERIFY(changed.wait(10000));
        QCOMPARE(monitor->status().owner, Status::Owner::Vitrine);
        QVERIFY(banner->isHidden());

        /* dnf upgrade: Plasma 6.8.0.  Nothing told it yet */
        QVERIFY(install("plasma-workspace", "6.8.0-1.fc44", "pw2"));
        QTest::qWait(200);
        QVERIFY(banner->isHidden());
        comeBack();
        /* read in the background: the banner changes once the reading is back */
        QVERIFY(banner->isHidden());
        QVERIFY(changed.wait(10000));
        QVERIFY(!banner->isHidden());
        QVERIFY2(banner->text().contains("this computer's Plasma 6.8.0"), qPrintable(banner->text()));

        /* and back to 6.7.91: gone the next time */
        QVERIFY(install("plasma-workspace", "6.7.91-1.fc44", "pw"));
        comeBack();
        QVERIFY(changed.wait(10000));
        QVERIFY(banner->isHidden());
    }

    /*
     * Back in front again while a reading is under way: no other one,
     * neither then nor after it, and the GUI thread never waits for it
     */
    void oneReadingAtATime()
    {
        HostKwinMonitor *monitor = HostKwinMonitor::instance();
        QSignalSpy changed(monitor, &HostKwinMonitor::changed);
        QWidget window;
        new HostKwinBanner(&window);
        if (!monitor->isRead()) {
            QVERIFY(changed.wait(10000));
        }
        changed.clear();
        const int before = readings();

        QVERIFY(write(m_recipe + "/hold", ""));
        QElapsedTimer timer;
        timer.start();
        comeBack();
        QTRY_COMPARE(readings(), before + 1);
        comeBack();
        comeBack();
        QVERIFY2(timer.elapsed() < 3000, "a reading held the GUI thread");
        QVERIFY(changed.isEmpty());
        QVERIFY(QFile::remove(m_recipe + "/hold"));
        QVERIFY(changed.wait(10000));
        /* a reading queued meanwhile would have started by now */
        QTest::qWait(1000);
        QCOMPARE(readings(), before + 1);
        QCOMPARE(changed.size(), 1);
    }
};

QTEST_MAIN(TestHostKwinBanner)
#include "test_hostkwinbanner.moc"
