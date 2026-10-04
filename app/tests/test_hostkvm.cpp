// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFileInfo>
#include <QTest>

#include "core/hostkvm.h"

class TestHostKvm : public QObject
{
    Q_OBJECT

private slots:
    /* This host's answer: a value where /dev/kvm opens (Linux 6.16 and
       later on AMD: yes) */
    void thisHost()
    {
        if (!QFileInfo("/dev/kvm").isReadable()) {
            QSKIP("no /dev/kvm");
        }
        const std::optional<bool> can = HostKvm::canHonorGuestPat();
        QVERIFY(can.has_value());
        qInfo("this host's KVM can honor the guest's PAT: %s", *can ? "yes" : "no");
    }

    void withHostPat_data()
    {
        QTest::addColumn<QString>("args");
        QTest::addColumn<int>("canHonor");   // 1 yes, 0 no, -1 unknown
        QTest::addColumn<QString>("expected");
        QTest::addColumn<bool>("noted");

        const QString on = "-machine q35\n-accel kvm,honor-guest-pat=on,kernel-irqchip=split\n";
        const QString asAuto = "-machine q35\n-accel kvm,honor-guest-pat=auto,kernel-irqchip=split\n";
        /* a kernel before 6.16: QEMU would refuse on */
        QTest::newRow("on, cannot") << on << 0 << asAuto << true;
        QTest::newRow("on, can") << on << 1 << on << false;
        /* /dev/kvm did not tell: QEMU decides, as written */
        QTest::newRow("on, unknown") << on << -1 << on << false;
        for (const char *value : {"auto", "off"}) {
            const QString args = QString("-accel kvm,honor-guest-pat=%1\n").arg(value);
            QTest::newRow(value) << args << 0 << args << false;
        }
        QTest::newRow("absent") << "-accel kvm\n" << 0 << "-accel kvm\n" << false;
        QTest::newRow("tcg") << "-accel tcg,honor-guest-pat=on\n" << 0
                             << "-accel tcg,honor-guest-pat=on\n" << false;
    }
    void withHostPat()
    {
        QFETCH(QString, args);
        QFETCH(int, canHonor);
        QFETCH(QString, expected);
        QFETCH(bool, noted);
        QString note;

        bool asked = false;
        const ArgsFile out = HostKvm::withHostPat(
            ArgsFile::parse(args),
            [&]() {
                asked = true;
                return canHonor < 0 ? std::nullopt : std::optional<bool>(canHonor == 1);
            },
            &note);
        QCOMPARE(out.toText(), expected);
        /* /dev/kvm asked only for on with KVM */
        QCOMPARE(asked, args.contains("kvm,honor-guest-pat=on"));
        QCOMPARE(!note.isEmpty(), noted);
        if (noted) {
            QVERIFY(note.startsWith("honor-guest-pat=on started as auto: "));
        }
    }
};

QTEST_GUILESS_MAIN(TestHostKvm)
#include "test_hostkvm.moc"
