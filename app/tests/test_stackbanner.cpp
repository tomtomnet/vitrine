// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

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
};

QTEST_MAIN(TestStackBanner)
#include "test_stackbanner.moc"
