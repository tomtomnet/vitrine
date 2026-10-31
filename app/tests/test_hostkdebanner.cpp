// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include "activation.h"
#include "core/hostkde.h"
#include "hostkdestandins.h"
#include "ui/banner.h"
#include "ui/hostkdebanner.h"

using namespace HostKde;

/*
 * The recipe's stand-in (host/kde/build.sh --print-recipe FEDORA
 * PACKAGE=VERSION...): each reading of the system says itself in calls,
 * and waits while hold is there (10 s at most), for a reading that takes
 * its time.  Sets for KWin 6.7.91 and Kirigami 6.31.0 only.
 */
static const char kRecipe[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
echo "$*" >> "$here/calls"
for _ in $(seq 100); do [ -e "$here/hold" ] || break; sleep 0.1; done
printf 'fedora=44\ncopr=copr:copr.fedorainfracloud.org:group_kdesig:kde-beta\n'
shift 2
for a; do
	case $a in
	kwin=6.7.91) echo "set.kwin=hl39 hl39" ;;
	kf6-kirigami=6.31.0) echo "set.kf6-kirigami=f1 f1" ;;
	*) echo "set.${a%%=*}=" ;;
	esac
	echo "key.${a%%=*}=k"
done
)sh";

/* Two of host/kde/components.conf's packages */
static const char kComponents[] = R"(
[host-kwin]
title=KWin
kind=system
package=kwin
follows=plasma-workspace 3
against=qt6-qtbase qt6-qtdeclarative kdecoration libplasma
patches=research/patches/kwin

[host-kirigami]
title=Kirigami
kind=system
package=kf6-kirigami
follows=kf6-kcoreaddons 2
against=qt6-qtbase qt6-qtdeclarative
patches=research/patches/kirigami
)";

/*
 * The banner of Vitrine's host packages left behind by the system, when it
 * is updated by hand while vitrine runs (dnf, in a terminal): it shows when
 * vitrine comes back in front, from a reading in the background, one at a
 * time.  The system is the stand-ins' (rpm and dnf on a state folder).
 */
class TestHostKdeBanner : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_state;
    QString m_recipe;
    QString m_data;

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
        const QString source = name.startsWith("kwin") ? QStringLiteral("kwin") : name;
        return write(m_state + "/installed/" + name,
                     QString("NAME=%1\nVERSION=%2\nRELEASE=%3\nARCH=x86_64\nDIGEST=%4\n"
                             "SOURCERPM=%5-%2-%3.src.rpm\nEPOCHNUM=0\nREPO=%6\n"
                             "INSTALLTIME=1760000000\nVENDOR=(none)\n")
                         .arg(name, evr.section('-', 0, 0), evr.section('-', 1), digest, source, repo)
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
        m_recipe = m_tmp.filePath("kde");
        m_data = m_tmp.filePath("data");
        QVERIFY(write(bin + "/rpm", kFakeRpm));
        QVERIFY(write(bin + "/dnf", kFakeDnf));
        for (const char *name : {"rpm", "dnf"}) {
            QFile::setPermissions(bin + '/' + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        QVERIFY(write(m_recipe + "/build.sh", kRecipe));
        QVERIFY(write(m_recipe + "/components.conf", kComponents));
        qputenv("PATH", bin.toUtf8() + ":/usr/bin:/bin");
        qputenv("STATE", m_state.toUtf8());

        /* Vitrine's KWin for Plasma 6.7.91 and Qt 6.11.2, installed: its record */
        Record record;
        record.evr = "6.7.91-1.hl39.fc44";
        record.series = "hl39";
        record.tag = "hl39";
        record.when = QDateTime::currentDateTimeUtc();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            record.digests.insert(name, QString("vitrine-%1").arg(name));
            QVERIFY(install(name, record.evr, record.digests.value(name), "@commandline"));
        }
        record.buildroot.insert("qt6-qtbase", "6.11.2");
        QVERIFY(record.write(Record::path(m_data + "/host-kwin")));
        QVERIFY(install("plasma-workspace", "6.7.91-1.fc44", "pw"));
        QVERIFY(install("qt6-qtbase", "6.11.2-3.fc44", "qt"));
        QVERIFY(install("kf6-kirigami", "6.31.0-1.fc44", "kirigami"));
        QVERIFY(install("kf6-kcoreaddons", "6.31.0-1.fc44", "kca"));
        QVERIFY(write(m_tmp.filePath("os-release"), "ID=fedora\nVERSION_ID=44\n"));

        HostKdeMonitor *monitor = HostKdeMonitor::instance();
        monitor->setSourceDir(m_recipe);
        monitor->setDataDir(m_data);
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
        HostKdeMonitor *monitor = HostKdeMonitor::instance();
        QSignalSpy changed(monitor, &HostKdeMonitor::changed);
        /* in a window never shown: no activation of its own */
        QWidget window;
        auto *banner = new HostKdeBanner(&window);
        /* the system read at its start */
        QVERIFY(changed.wait(10000));
        QCOMPARE(monitor->status("host-kwin").owner, Status::Owner::Vitrine);
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
        QVERIFY(banner->text().contains("before you log out"));
        QCOMPARE(banner->outdated(), QStringList{"host-kwin"});
        /* no set for KWin 6.8.0 in this recipe: nothing to rebuild, only Revert… */
        QVERIFY(banner->rebuildable().isEmpty());
        QVERIFY(banner->findChild<Banner *>()->button()->isHidden());
        QSignalSpy reverted(banner, &HostKdeBanner::revertRequested);
        for (QPushButton *button : banner->findChildren<QPushButton *>()) {
            if (button->text() == "Revert…") {
                button->click();
            }
        }
        QCOMPARE(reverted.size(), 1);
        QCOMPARE(reverted.last().value(0).toStringList(), QStringList{"host-kwin"});

        /* and back to 6.7.91: gone the next time */
        QVERIFY(install("plasma-workspace", "6.7.91-1.fc44", "pw"));
        comeBack();
        QVERIFY(changed.wait(10000));
        QVERIFY(banner->isHidden());
    }

    /* The KDE Frameworks past Vitrine's Kirigami: said, Rebuild… once the recipe has a set */
    void frameworksMovedOn()
    {
        HostKdeMonitor *monitor = HostKdeMonitor::instance();
        QSignalSpy changed(monitor, &HostKdeMonitor::changed);
        QWidget window;
        auto *banner = new HostKdeBanner(&window);
        Record record;
        record.evr = "6.31.0-1.f1.fc44";
        record.tag = "f1";
        record.when = QDateTime::currentDateTimeUtc();
        record.digests.insert("kf6-kirigami", "vitrine-f1");
        record.buildroot.insert("qt6-qtbase", "6.11.2");
        QVERIFY(record.write(Record::path(m_data + "/host-kirigami")));
        QVERIFY(install("kf6-kirigami", "6.31.0-1.f1.fc44", "vitrine-f1", "@commandline"));
        monitor->refresh();
        QVERIFY(changed.wait(10000));
        QCOMPARE(monitor->status("host-kirigami").owner, Status::Owner::Vitrine);
        QVERIFY(banner->isHidden());

        QVERIFY(install("kf6-kcoreaddons", "6.32.0-1.fc44", "kca2"));
        comeBack();
        QVERIFY(changed.wait(10000));
        QVERIFY(!banner->isHidden());
        QVERIFY2(banner->text().contains("Vitrine's Kirigami no longer matches this computer's KDE "
                                         "Frameworks 6.32.0"),
                 qPrintable(banner->text()));
        QVERIFY(!banner->text().contains("log out"));
        QCOMPARE(banner->outdated(), QStringList{"host-kirigami"});
        QVERIFY(banner->rebuildable().isEmpty());
        QVERIFY(banner->text().contains("no set for the new version yet"));

        /* and Qt with KWin: both said, and Revert… for both */
        QVERIFY(install("qt6-qtbase", "6.11.3-1.fc44", "qt2"));
        comeBack();
        QVERIFY(changed.wait(10000));
        QVERIFY(banner->text().contains("Vitrine's KWin no longer matches this computer's Qt 6.11.3"));
        QVERIFY(banner->text().contains("KDE Frameworks 6.32.0 and Qt 6.11.3"));
        QCOMPARE(banner->outdated(), QStringList({"host-kwin", "host-kirigami"}));

        /* back as they were */
        QVERIFY(install("qt6-qtbase", "6.11.2-3.fc44", "qt"));
        QVERIFY(install("kf6-kcoreaddons", "6.31.0-1.fc44", "kca"));
        QVERIFY(install("kf6-kirigami", "6.31.0-1.fc44", "kirigami"));
        QFile::remove(Record::path(m_data + "/host-kirigami"));
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
        HostKdeMonitor *monitor = HostKdeMonitor::instance();
        QSignalSpy changed(monitor, &HostKdeMonitor::changed);
        QWidget window;
        new HostKdeBanner(&window);
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

QTEST_MAIN(TestHostKdeBanner)
#include "test_hostkdebanner.moc"
