// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QRadioButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include "core/paths.h"
#include "core/vmstore.h"
#include "ui/newvmdialog.h"
#include "ui/qemudocs.h"

/* The 3D card of vitrine's QEMU, all of it */
static const char kCard[] = "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n";

/*
 * A new Linux VM is made for vitrine's QEMU, built or not, unless the
 * preferences choose another QEMU, which it is adapted to.  Before
 * vitrine's QEMU was built, the dialog adapted the VM to the system's
 * QEMU found in the meantime: no native context, for good.
 */
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
                                     "'-device virtio-gpu-gl-pci,help')\n"
                                     "  echo 'virtio-gpu-gl-pci options:'\n"
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
    }

    /* Not built: the system's QEMU is found meanwhile, but not targeted */
    void vitrinesQemuNotBuilt()
    {
        QVERIFY(Paths::stackQemu().isEmpty());
        QCOMPARE(Paths::qemuBinary(), systemQemu());
        const QString args = create();

        QVERIFY2(args.contains(kCard), qPrintable(args));
        QVERIFY(args.contains("-accel kvm,honor-guest-pat=on\n"));
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

        QVERIFY2(args.contains("-device virtio-gpu-gl-pci,hostmem=4G,blob=on\n"),
                 qPrintable(args));
        QVERIFY(args.contains("-accel kvm\n"));
        QVERIFY(!args.contains("drm_native_context"));
        QVERIFY(!args.contains("venus"));
    }
};

QTEST_MAIN(TestNewVmDialog)
#include "test_newvmdialog.moc"
