// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QRadioButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>

#include "core/guestos.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmstore.h"
#include "ui/newvmdialog.h"
#include "ui/oschooser.h"
#include "ui/qemudocs.h"

/* The 3D card of vitrine's QEMU, all of it */
static const char kCard[] = "-device virtio-vga-gl,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n";

/*
 * A new Linux VM is made for vitrine's QEMU, built or not, unless the
 * preferences choose another QEMU, which it is adapted to.  Before
 * vitrine's QEMU was built, the dialog adapted the VM to the system's
 * QEMU found in the meantime: no native context, for good.
 */
/* An ISO 9660 image of a label, as small as it goes: its volume descriptors */
static bool writeIso(const QString &path, const QString &volume, const QString &publisher = {})
{
    QByteArray pvd(2048, '\0');
    QByteArray end(2048, '\0');
    QFile f(path);

    pvd[0] = 1;
    pvd.replace(1, 5, "CD001");
    pvd[6] = 1;
    pvd.replace(8, 32, QByteArray(32, ' '));
    pvd.replace(40, 32, volume.toLatin1().leftJustified(32, ' '));
    pvd[80] = 20;
    pvd[128] = 0;
    pvd[129] = 8;
    pvd.replace(318, 128, publisher.toLatin1().leftJustified(128, ' '));
    end[0] = char(0xff);
    end.replace(1, 5, "CD001");
    return f.open(QIODevice::WriteOnly) && f.write(QByteArray(0x8000, '\0')) == 0x8000 &&
           f.write(pvd) == 2048 && f.write(end) == 2048;
}

class TestNewVmDialog : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    int m_count = 0;

    static bool script(const QString &path, const QByteArray &body)
    {
        QFile f(path);

        if (!QDir().mkpath(QFileInfo(path).path()) || !f.open(QIODevice::WriteOnly) ||
            f.write("#!/bin/sh\n" + body + "\n") < 0) {
            return false;
        }
        f.close();
        return f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }

    /* The system's QEMU: QEMU 10.2, without native context */
    QString systemQemu() const { return m_tmp.filePath("bin/" + Paths::qemuSystemName()); }

    /* The arguments of a new VM of "Linux with KDE Plasma" on a disk image */
    QString create()
    {
        VmStore store(m_tmp.filePath("vms"));
        NewVmDialog dialog(&store);
        auto *name = dialog.findChild<QLineEdit *>("name");
        auto *existing = dialog.findChild<QRadioButton *>("existingDisk");
        auto *disk = dialog.findChild<QLineEdit *>("diskPath");

        if (!name || !existing || !disk) {
            return {};
        }
        name->setText(QString("Fedora %1").arg(++m_count));
        existing->setChecked(true);
        disk->setText(m_tmp.filePath("disk.img"));
        dialog.accept();
        return dialog.vm() ? dialog.vm()->args().toText() : QString();
    }

private slots:
    void initTestCase()
    {
        QFile disk(m_tmp.filePath("disk.img"));

        QVERIFY(m_tmp.isValid());
        if (Paths::hostArch() != "x86_64") {
            QSKIP("the 3D card of Linux on a PC");
        }
        /* settings, VMs and stack in here: vitrine's QEMU not built */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        QVERIFY(script(systemQemu(), "case \"$1 $2\" in\n"
                                     "'-device virtio-vga-gl,help')\n"
                                     "  echo 'virtio-vga-gl options:'\n"
                                     "  echo '  blob=<bool>'\n"
                                     "  echo '  hostmem=<size>'\n"
                                     "  echo '  venus=<bool>' ;;\n"
                                     "esac"));
        qputenv("PATH", QFileInfo(systemQemu()).path().toUtf8());
        QVERIFY(disk.open(QIODevice::WriteOnly) && disk.write(QByteArray(512, '\0')) > 0);
        disk.close();
        Paths::setQemuBinary({});
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
        GuestOs::Catalogue::reload();
    }

    /* The system: Fedora at first, then as the disc tells, or as chosen */
    void system()
    {
        VmStore store(m_tmp.filePath("vms"));
        NewVmDialog dialog(&store);
        auto *name = dialog.findChild<QLineEdit *>("name");
        auto *iso = dialog.findChild<QLineEdit *>("iso");
        auto *os = dialog.findChild<OsChooser *>("os");
        auto *detect = dialog.findChild<QCheckBox *>("detect");
        auto *detected = dialog.findChild<QLabel *>("detected");
        auto *desktop = dialog.findChild<QComboBox *>("desktop");
        auto *memory = dialog.findChild<QSpinBox *>("memory");
        auto *existing = dialog.findChild<QRadioButton *>("existingDisk");
        auto *disk = dialog.findChild<QLineEdit *>("diskPath");
        const QString fedora = m_tmp.filePath("Fedora-Workstation-Live-44-1.1.x86_64.iso");
        const QString windows = m_tmp.filePath("windows.iso");
        const QString unknown = m_tmp.filePath("unknown.iso");

        /* vitrine's list of systems, the same on every computer */
        GuestOs::Catalogue::reload({});
        QVERIFY(name && iso && os && detect && detected && desktop && memory);
        QVERIFY(writeIso(fedora, "Fedora-WS-Live-44-1-1"));
        QVERIFY(writeIso(windows, "CCCOMA_X64FRE_EN-US_DV9", "MICROSOFT CORPORATION"));
        QVERIFY(writeIso(unknown, "ISOIMAGE"));

        /* what the template and the guest tools are made for */
        QCOMPARE(os->id(), "fedora44");
        QCOMPARE(desktop->currentData().toString(), "kde");
        QVERIFY(desktop->isEnabled());
        QVERIFY(detect->isChecked());
        QCOMPARE(name->text(), "");

        /* Fedora Workstation: GNOME */
        iso->setText(fedora);
        QCOMPARE(os->id(), "fedora44");
        QCOMPARE(desktop->currentData().toString(), "gnome");
        QCOMPARE(name->text(), "Fedora Linux 44");
        QVERIFY(!detected->isHidden());

        /* Windows 11: its template's defaults, its name */
        iso->setText(windows);
        QCOMPARE(os->id(), "win11");
        QCOMPARE(os->currentText(), "Microsoft Windows 11");
        QCOMPARE(name->text(), "Microsoft Windows 11");
        QVERIFY(!desktop->isEnabled());
        QCOMPARE(memory->value(), qMin(8192, qMax(memory->maximum() / 2, 1024)));

        /* a disc not known: the system stays, the user is told */
        iso->setText(unknown);
        QCOMPARE(os->id(), "win11");
        QVERIFY(detected->text().contains("Not recognized"));

        /* chosen by hand: the disc no longer decides */
        os->showPopup();
        QTest::keyClicks(os->searchField(), "arch linux");
        QTest::keyClick(os->searchField(), Qt::Key_Return);
        QCOMPARE(os->id(), "archlinux");
        QVERIFY(!detect->isChecked());
        QCOMPARE(name->text(), "Arch Linux");
        QVERIFY(desktop->isEnabled());
        iso->setText(fedora);
        QCOMPARE(os->id(), "archlinux");

        /* a name of the user's stays */
        name->clear();
        QTest::keyClicks(name, "Mine");
        detect->setChecked(true);
        QCOMPARE(os->id(), "fedora44");
        QCOMPARE(name->text(), "Mine");

        /* the VM's #guest */
        existing->setChecked(true);
        disk->setText(m_tmp.filePath("disk.img"));
        dialog.accept();
        QVERIFY(dialog.vm());
        const VmConfig::Guest guest = VmConfig::guest(dialog.vm()->args());
        QCOMPARE(guest.os, "linux");
        QCOMPARE(guest.id, "fedora44");
        QCOMPARE(guest.desktop, "gnome");
        QVERIFY(dialog.vm()->args().toText().contains(fedora));
    }

    /* Not built: the system's QEMU is found meanwhile, but not targeted */
    void vitrinesQemuNotBuilt()
    {
        QVERIFY(Paths::stackQemu().isEmpty());
        QCOMPARE(Paths::qemuBinary(), systemQemu());
        const QString args = create();

        QVERIFY2(args.contains(kCard), qPrintable(args));
        QVERIFY(args.contains("-accel kvm,honor-guest-pat=auto\n"));
        QVERIFY(!args.contains("venus"));
        QVERIFY(!args.contains("#qemu"));
    }

    void vitrinesQemuBuilt()
    {
        const QString stack = Paths::stackDir();
        const auto undo = qScopeGuard([stack]() { QFile::remove(stack + "/current"); });

        QVERIFY(stack.startsWith(m_tmp.path()));
        QVERIFY(script(stack + "/0123456789abcdef/bin/" + Paths::qemuSystemName(), "exit 1"));
        QVERIFY(QFile::link("0123456789abcdef", stack + "/current"));
        QVERIFY(!Paths::stackQemu().isEmpty());
        QemuDocs::reloadPreferred();
        const QString args = create();

        QVERIFY2(args.contains(kCard), qPrintable(args));
        QVERIFY(!args.contains("venus"));
    }

    /* Chosen in the preferences: adapted to, here the same binary as found */
    void otherQemuChosen()
    {
        const auto restore = qScopeGuard([]() {
            Paths::setQemuBinary({});
            QemuDocs::reloadPreferred();
        });

        Paths::setQemuBinary(systemQemu());
        QemuDocs::reloadPreferred();
        const QString args = create();

        QVERIFY2(args.contains("-device virtio-vga-gl,hostmem=4G,blob=on\n"),
                 qPrintable(args));
        QVERIFY(args.contains("-accel kvm\n"));
        QVERIFY(!args.contains("drm_native_context"));
        QVERIFY(!args.contains("venus"));
    }
};

QTEST_MAIN(TestNewVmDialog)
#include "test_newvmdialog.moc"
