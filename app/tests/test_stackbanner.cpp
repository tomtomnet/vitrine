// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTest>

#include "activation.h"
#include "core/paths.h"
#include "core/stackbuilder.h"
#include "ui/qemudocs.h"
#include "ui/stackbanner.h"

/*
 * The banner of Vitrine's QEMU follows the preferences: hidden while they
 * name a QEMU of their own, shown again when they go back to the default,
 * even when both are the same binary
 */
class TestStackBanner : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        /* settings and data of their own: no stack built there */
        QStandardPaths::setTestModeEnabled(true);
        /* the QEMU found by default, a stand-in that tells nothing */
        QFile qemu(m_tmp.filePath(Paths::qemuSystemName()));
        QVERIFY(qemu.open(QIODevice::WriteOnly) && qemu.write("#!/bin/sh\nexit 1\n") > 0);
        qemu.close();
        QVERIFY(qemu.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        qputenv("PATH", m_tmp.path().toUtf8());
        Paths::setQemuBinary({});
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
    }

    void followsThePreferences()
    {
        const QString found = Paths::defaultQemuBinary();
        QCOMPARE(found, m_tmp.filePath(Paths::qemuSystemName()));
        if (StackBuilder::state() != StackBuilder::State::NotBuilt) {
            QSKIP("needs host/ without a build of it");
        }
        StackBanner banner;
        QVERIFY(!banner.isHidden());

        /* "use another QEMU": the one found by default, chosen by path */
        Paths::setQemuBinary(found);
        QemuDocs::reloadPreferred();
        QVERIFY(banner.isHidden());
        /* and back */
        Paths::setQemuBinary({});
        QemuDocs::reloadPreferred();
        QVERIFY(!banner.isHidden());
    }

    /*
     * Vitrine's QEMU built by hand while vitrine runs (host/build.sh): the
     * banner goes when vitrine comes back in front, and comes back for a
     * build of other inputs.  The build's result as build.sh leaves it: its
     * manifest in a folder of the stack, `current` pointing to it.
     */
    void builtByHand()
    {
        if (StackBuilder::state() != StackBuilder::State::NotBuilt) {
            QSKIP("needs host/ without a build of it");
        }
        StackBanner banner;
        QVERIFY(!banner.isHidden());
        QVERIFY(text(banner).contains("not built yet"));

        const QString stamp = StackBuilder::inputStamp(StackBuilder::hostDir());
        const QString stack = Paths::stackDir();
        QVERIFY(!stamp.isEmpty());
        QVERIFY(writeBuild(stack + '/' + stamp.left(16), stamp));
        QVERIFY(QFile::link(stamp.left(16), stack + "/current"));
        const auto clean = qScopeGuard([&stack]() { QDir(stack).removeRecursively(); });
        QCOMPARE(StackBuilder::state(), StackBuilder::State::UpToDate);
        /* nothing told it yet */
        QVERIFY(!banner.isHidden());
        comeBack();
        QVERIFY(banner.isHidden());

        /* built again from other inputs, as an older vitrine's would be */
        QVERIFY(writeBuild(stack + '/' + stamp.left(16), QString(64, '0')));
        QVERIFY(banner.isHidden());
        comeBack();
        QVERIFY(!banner.isHidden());
        QVERIFY2(text(banner).contains("out of date"), qPrintable(text(banner)));
    }

private:
    /* What it says, its labels' texts */
    static QString text(const StackBanner &banner)
    {
        QStringList texts;
        for (const QLabel *label : banner.findChildren<QLabel *>()) {
            if (!label->isHidden() && !label->text().isEmpty()) {
                texts << label->text();
            }
        }
        return texts.join(' ');
    }

    /* A build's manifest, of @stamp's inputs, in @prefix */
    static bool writeBuild(const QString &prefix, const QString &stamp)
    {
        QFile manifest(prefix + "/share/vitrine/stack.conf");
        return QDir().mkpath(prefix + "/share/vitrine") && manifest.open(QIODevice::WriteOnly) &&
               manifest.write("# What host/build.sh built here\nSTAMP=" + stamp.toLatin1() +
                              "\nBUILT=2026-10-15T04:00:00Z\n") > 0;
    }
};

QTEST_MAIN(TestStackBanner)
#include "test_stackbanner.moc"
