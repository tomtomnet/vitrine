// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/vmcloner.h"
#include "core/vmstore.h"

class TestVmCloner : public QObject
{
    Q_OBJECT

    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(data);
        }
    }

    static QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

private slots:
    void clone()
    {
        QTemporaryDir tmp;
        const QString ext = tmp.filePath("elsewhere");
        VmStore store(tmp.filePath("vms"));
        QString error;
        Vm *source = store.create("Fedora", &error);

        write(source->dir() + "/OVMF_CODE.fd", "code");
        write(source->dir() + "/OVMF_VARS.fd", "vars");
        write(source->dir() + "/disk.qcow2", "disk");
        write(source->dir() + "/install.iso", "iso");
        write(ext + "/data.img", "data");
        write(ext + "/other/disk.qcow2", "other disk");
        write(ext + "/os.iso", "os");
        QVERIFY(source->save(ArgsFile::parse(
            "-name Fedora\n"
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=OVMF_CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=OVMF_VARS.fd\n"
            "-drive file=disk.qcow2,if=virtio,format=qcow2\n"
            "-hdb " + ext + "/data.img\n"
            "-drive file=" + ext + "/other/disk.qcow2,if=virtio\n"
            "-drive file=" + ext + "/os.iso,media=cdrom\n"
            "-cdrom install.iso\n"
            "-kernel " + ext + "/vmlinuz\n"
            "-device virtio-net-pci,netdev=n,mac=52:54:00:11:22:33\n"
            "-uuid 12345678-1234-1234-1234-123456789abc\n")));

        QCOMPARE(VmCloner::filesToCopy(source).size(), 5);

        VmCloner cloner(&store);
        QSignalSpy finished(&cloner, &VmCloner::finished);
        cloner.start(source, "Fedora copy");
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][1].toString(), "");
        Vm *clone = finished[0][0].value<Vm *>();
        QVERIFY(clone);
        QCOMPARE(clone->name(), "Fedora copy");

        /* what it writes, copied into its folder */
        QCOMPARE(read(clone->dir() + "/OVMF_CODE.fd"), "code");
        QCOMPARE(read(clone->dir() + "/OVMF_VARS.fd"), "vars");
        QCOMPARE(read(clone->dir() + "/disk.qcow2"), "disk");
        QCOMPARE(read(clone->dir() + "/data.img"), "data");
        QCOMPARE(read(clone->dir() + "/disk-2.qcow2"), "other disk");
        const QString text = clone->args().toText();
        QVERIFY2(text.contains("unit=1,file=OVMF_VARS.fd\n"), qPrintable(text));
        QVERIFY(text.contains("-drive file=disk.qcow2,if=virtio,format=qcow2\n"));
        QVERIFY(text.contains("-hdb data.img\n"));
        QVERIFY(text.contains("-drive file=disk-2.qcow2,if=virtio\n"));
        /* what it reads, shared */
        QVERIFY(text.contains("-drive file=" + ext + "/os.iso,media=cdrom\n"));
        QVERIFY(text.contains("-cdrom " + source->dir() + "/install.iso\n"));
        QVERIFY(text.contains("-kernel " + ext + "/vmlinuz\n"));
        QVERIFY(!QFileInfo::exists(clone->dir() + "/install.iso"));
        /* a machine of its own */
        QVERIFY(!text.contains("11:22:33"));
        QVERIFY(QRegularExpression("mac=52:54:00:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}\n")
                    .match(text).hasMatch());
        QVERIFY(!text.contains("12345678-1234"));
        QVERIFY(text.contains("-uuid "));
        /* the source is left as it was */
        QVERIFY(source->args().toText().contains("mac=52:54:00:11:22:33"));
    }

    void cancel()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.filePath("vms"));
        QString error;
        Vm *source = store.create("Big", &error);

        write(source->dir() + "/disk.img", QByteArray(1 << 20, 'x'));
        QVERIFY(source->save(ArgsFile::parse("-hda disk.img\n")));
        VmCloner cloner(&store);
        QSignalSpy finished(&cloner, &VmCloner::finished);
        cloner.start(source, "Big copy");
        cloner.cancel();
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].value<Vm *>());
        QCOMPARE(finished[0][1].toString(), "Cancelled");
        QTRY_COMPARE(store.vms().size(), 1);
        QVERIFY(!QFileInfo::exists(tmp.filePath("vms/Big-copy")));
    }
};

QTEST_GUILESS_MAIN(TestVmCloner)
#include "test_vmcloner.moc"
