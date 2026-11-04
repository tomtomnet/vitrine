// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/vmsummary.h"

/*
 * What the GTK app says of a VM (VmSummary), in the Qt app's words: its
 * state, its console's first page, the sections and rows of its details,
 * a shutdown's notices.  No GTK: the words alone.
 */
class TestVmSummary : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    /* A VM of @args in a folder of its own */
    std::unique_ptr<Vm> vmOf(const QString &id, const QByteArray &args)
    {
        const QString dir = m_dir.filePath(id);
        QFile file(dir + "/vm.args");

        if (!QDir().mkpath(dir) || !file.open(QIODevice::WriteOnly)) {
            return nullptr;
        }
        file.write(args);
        file.close();
        return std::make_unique<Vm>(dir);
    }

    static const VmSummary::Row *rowOf(const QList<VmSummary::Section> &sections,
                                       const QString &section, const QString &key)
    {
        for (const VmSummary::Section &s : sections) {
            for (const VmSummary::Row &row : s.rows) {
                if (s.title == section && row.key == key) {
                    return &row;
                }
            }
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        /* settings of their own, and no QEMU to read the documentation of */
        QStandardPaths::setTestModeEnabled(true);
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
    }

    void home_data()
    {
        QTest::addColumn<QByteArray>("args");
        QTest::addColumn<QString>("text");
        QTest::newRow("desktop") << QByteArray("-m 4G\n-smp 4\n-display dbus,p2p=yes,gl=on\n"
                                               "-drive file=disks/disk.qcow2,if=virtio\n"
                                               "-cdrom install.iso\n")
                                 << "4 GiB of memory, 4 processors, screen in this window.\n"
                                    "Disks: disk.qcow2";
        QTest::newRow("own window") << QByteArray("-m 1536M\n-display sdl\n")
                                    << "1536 MiB of memory, 1 processor, screen in its own window.";
        QTest::newRow("headless") << QByteArray("-smp 2\n-display none\n")
                                  << "2 processors, no screen.";
    }

    void home()
    {
        QFETCH(QByteArray, args);
        QFETCH(QString, text);
        const std::unique_ptr<Vm> vm = vmOf(QTest::currentDataTag(), "-name Home\n" + args);

        QVERIFY(vm);
        QCOMPARE(VmSummary::homeText(vm->args()), text);
        QCOMPARE(VmSummary::stateText(vm.get()), "Powered off");
        QVERIFY(!VmSummary::keptOpen(vm.get()));
    }

    void details()
    {
        const std::unique_ptr<Vm> vm =
            vmOf("details", "-name Details\n-m 8G\n-accel kvm\n-smp 4,sockets=1,cores=2,threads=2\n"
                            "-device virtio-vga-gl\n-display dbus,p2p=yes,gl=on\n"
                            "-drive file=disk.qcow2,if=virtio,format=qcow2\n"
                            "-netdev user,id=net0\n-device virtio-net-pci,netdev=net0\n"
                            "#share tag=data,path=/srv/data,readonly=on\n");
        QVERIFY(vm);
        QFile log(vm->runner()->logPath());
        QVERIFY(log.open(QIODevice::WriteOnly));
        log.close();

        const QList<VmSummary::Section> sections = VmSummary::details(vm.get(), nullptr);
        QStringList titles;
        for (const VmSummary::Section &section : sections) {
            titles << section.title;
        }
        /* the empty ones left out: no USB nor PCI devices */
        QCOMPARE(titles, QStringList({"System", "Display", "Storage", "Network and Sound",
                                      "Shared Folders", "Files"}));

        const VmSummary::Row *memory = rowOf(sections, "System", "Memory");
        QVERIFY(memory);
        QCOMPARE(memory->value, "8 GiB");
        const VmSummary::Row *processors = rowOf(sections, "System", "Processors");
        QVERIFY(processors);
        QCOMPARE(processors->value, "4 (1 sockets, 2 cores, 2 threads)");
        const VmSummary::Row *accel = rowOf(sections, "System", "Acceleration");
        QVERIFY(accel);
        QCOMPARE(accel->value, "KVM");
        const VmSummary::Row *graphics = rowOf(sections, "Display", "Graphics");
        QVERIFY(graphics);
        QVERIFY2(graphics->value.contains("virtio-vga-gl"), qPrintable(graphics->value));
        const VmSummary::Row *disk = rowOf(sections, "Storage", "Disk (virtio)");
        QVERIFY(disk);
        QCOMPARE(disk->value, "disk.qcow2");
        /* the disk is not there: no usage to tell */
        QCOMPARE(disk->detail, QString());
        const VmSummary::Row *share = rowOf(sections, "Shared Folders", "data");
        QVERIFY(share);
        QCOMPARE(share->value, "/srv/data · read only");
        QCOMPARE(share->path, "/srv/data");
        const VmSummary::Row *folder = rowOf(sections, "Files", "Folder");
        QVERIFY(folder);
        QCOMPARE(folder->path, vm->dir());
        const VmSummary::Row *logRow = rowOf(sections, "Files", "Log");
        QVERIFY(logRow);
        QCOMPARE(logRow->path, vm->runner()->logPath());
        /* stopped: nothing known of what the guest does with its card */
        QCOMPARE(VmSummary::nativeContextNote(vm.get(), GpuContexts::Status::Virgl), QString());
    }

    void noDisk()
    {
        const std::unique_ptr<Vm> vm = vmOf("nodisk", "-name No Disk\n");
        QVERIFY(vm);
        const QList<VmSummary::Section> sections = VmSummary::details(vm.get(), nullptr);
        const VmSummary::Row *disks = rowOf(sections, "Storage", "Disks");

        QVERIFY(disks);
        QCOMPARE(disks->value, "none");
        const VmSummary::Row *memory = rowOf(sections, "System", "Memory");
        QVERIFY(memory);
        QCOMPARE(memory->value, "QEMU default");
    }

    void shutdown()
    {
        QCOMPARE(VmSummary::shutdownText("Fedora", GuestShutdown::Way::ToolsAgent),
                 "Asked Fedora to shut down through its guest tools");
        QCOMPARE(VmSummary::shutdownText("Fedora", GuestShutdown::Way::GuestAgent),
                 "Asked Fedora to shut down through its guest agent");
        QVERIFY(VmSummary::shutdownText("Fedora", GuestShutdown::Way::PowerButton)
                    .startsWith("The guest may ask what to do: answer it on its screen."));
        QCOMPARE(VmSummary::shutdownText("Fedora", GuestShutdown::Way::None), QString());
        /* the guest's question may wait a while */
        QVERIFY(VmSummary::shutdownTimeout(GuestShutdown::Way::PowerButton) >
                VmSummary::shutdownTimeout(GuestShutdown::Way::ToolsAgent));
    }
};

QTEST_GUILESS_MAIN(TestVmSummary)
#include "test_vmsummary.moc"
