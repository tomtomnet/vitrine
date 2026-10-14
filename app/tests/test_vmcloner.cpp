// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/tasks.h"
#include "core/vmcloner.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

#include <sys/stat.h>

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
        QStringList texts;
        connect(TaskList::instance(), &TaskList::taskChanged, this,
                [&texts](Task *task) { texts << task->text(); });
        cloner.start(source, "Fedora copy");
        /* a task of the app's while it copies */
        QVERIFY(cloner.task());
        QCOMPARE(cloner.task()->kind(), Task::Kind::Clone);
        QCOMPARE(cloner.task()->title(), "Clone of Fedora");
        QVERIFY(cloner.task()->isRunning());
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][1].toString(), "");
        Vm *clone = finished[0][0].value<Vm *>();
        QVERIFY(clone);
        QCOMPARE(clone->name(), "Fedora copy");
        disconnect(TaskList::instance(), &TaskList::taskChanged, this, nullptr);
        QVERIFY2(texts.contains("Copying disk.qcow2"), qPrintable(texts.join('\n')));
        QCOMPARE(cloner.task()->state(), Task::State::Done);
        QCOMPARE(cloner.task()->text(), "New VM: Fedora copy");
        QCOMPARE(cloner.task()->fraction(), 1.0);
        cloner.task()->dismiss();

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
        QCOMPARE(cloner.task()->state(), Task::State::Stopped);
        cloner.task()->dismiss();
    }

    /*
     * While it copies, the app goes on: the clone is no VM of the list yet,
     * the source does not start, nor go, nor gets cloned again; the clone
     * is in the list once complete, the source free again
     */
    void background()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.filePath("vms"));
        QString error;
        Vm *source = store.create("Slow", &error);

        /* its disk a pipe: cp waits for what the test writes into it */
        QVERIFY(::mkfifo(QFile::encodeName(source->dir() + "/disk.img").constData(), 0600) == 0);
        QVERIFY(source->save(ArgsFile::parse("-name Slow\n-hda disk.img\n")));
        VmCloner cloner(&store);
        QSignalSpy finished(&cloner, &VmCloner::finished);
        cloner.start(source, "Slow copy");
        QVERIFY(cloner.isRunning());
        QVERIFY(finished.isEmpty());
        QCOMPARE(store.vms(), QList<Vm *>({source}));
        QVERIFY(QFileInfo(tmp.filePath("vms/Slow-copy")).isDir());
        QVERIFY(!QFileInfo::exists(tmp.filePath("vms/Slow-copy/vm.args")));
        /* the folder seen meanwhile: still no VM */
        store.reload();
        QCOMPARE(store.vms().size(), 1);

        const QString busy = source->runner()->busy();
        QVERIFY2(busy.contains("Slow is being cloned"), qPrintable(busy));
        QSignalSpy failed(source->runner(), &VmRunner::failed);
        source->runner()->start(source->args());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), busy);
        QCOMPARE(source->runner()->state(), VmRunner::State::Stopped);
        QVERIFY(!store.remove(source, &error));
        QCOMPARE(error, busy);
        VmCloner again(&store);
        QSignalSpy refused(&again, &VmCloner::finished);
        again.start(source, "Slow copy 2");
        QCOMPARE(refused.size(), 1);
        QCOMPARE(refused[0][1].toString(), busy);
        QVERIFY(!again.task());

        QFile disk(source->dir() + "/disk.img");
        QVERIFY(disk.open(QIODevice::WriteOnly));
        disk.write("disk");
        disk.close();
        QVERIFY(finished.wait(10000));
        Vm *clone = finished[0][0].value<Vm *>();
        QVERIFY(clone);
        QCOMPARE(store.vms().size(), 2);
        QVERIFY(store.vms().contains(clone));
        QCOMPARE(read(clone->dir() + "/disk.img"), "disk");
        QVERIFY(source->runner()->busy().isEmpty());
        cloner.task()->dismiss();
    }

    /* The app quits during a copy: the half-made clone goes, the source is free */
    void quitDuringCopy()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.filePath("vms"));
        QString error;
        Vm *source = store.create("Slow", &error);

        QVERIFY(::mkfifo(QFile::encodeName(source->dir() + "/disk.img").constData(), 0600) == 0);
        QVERIFY(source->save(ArgsFile::parse("-name Slow\n-hda disk.img\n")));
        {
            VmCloner cloner(&store);
            cloner.start(source, "Slow copy");
            QVERIFY(cloner.isRunning());
            QVERIFY(QFileInfo(tmp.filePath("vms/Slow-copy")).isDir());
        }
        QVERIFY(!QFileInfo::exists(tmp.filePath("vms/Slow-copy")));
        QVERIFY(source->runner()->busy().isEmpty());
        QCOMPARE(store.vms().size(), 1);
    }

    /* A copy that fails: the clone goes, its task says why */
    void failure()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.filePath("vms"));
        QString error;
        Vm *source = store.create("Locked", &error);

        write(source->dir() + "/disk.img", "disk");
        QVERIFY(QFile::setPermissions(source->dir() + "/disk.img", QFileDevice::WriteOwner));
        QVERIFY(source->save(ArgsFile::parse("-name Locked\n-hda disk.img\n")));
        VmCloner cloner(&store);
        QSignalSpy finished(&cloner, &VmCloner::finished);
        cloner.start(source, "Locked copy");
        QVERIFY(finished.wait(10000));
        QVERIFY(!finished[0][0].value<Vm *>());
        const QString why = finished[0][1].toString();
        QVERIFY2(why.contains("Permission denied"), qPrintable(why));
        QTRY_COMPARE(store.vms().size(), 1);
        QCOMPARE(cloner.task()->state(), Task::State::Failed);
        QCOMPARE(cloner.task()->text(), why);
        cloner.task()->dismiss();
    }
};

QTEST_GUILESS_MAIN(TestVmCloner)
#include "test_vmcloner.moc"
