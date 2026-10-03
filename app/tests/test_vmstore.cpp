// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "core/paths.h"
#include "core/vmstore.h"

static QString read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

static void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
    }
}

class TestVmStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void create()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.path());
        QSignalSpy added(&store, &VmStore::added);

        QVERIFY(store.vms().isEmpty());
        Vm *vm = store.create("My VM");
        QVERIFY(vm);
        QCOMPARE(vm->id(), "My-VM");
        QCOMPARE(vm->name(), "My VM");
        QCOMPARE(vm->dir(), tmp.filePath("My-VM"));
        QCOMPARE(read(vm->argsPath()), "-name My VM\n");
        QCOMPARE(added.size(), 1);

        QCOMPARE(store.create("My VM")->id(), "My-VM-2");
        QCOMPARE(store.create("../escape")->id(), "escape");
        QCOMPARE(store.create("..")->id(), "vm");
        QCOMPARE(store.create("Été, 2")->id(), "Été-2");
        QCOMPARE(store.find("My-VM"), vm);
        QCOMPARE(store.vms().size(), 5);
    }

    void sortedByName()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.path());

        store.create("b");
        store.create("A");
        store.create("c");
        QStringList names;
        for (Vm *vm : store.vms()) {
            names << vm->name();
        }
        QCOMPARE(names, QStringList({"A", "b", "c"}));
    }

    void save()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.path());
        Vm *vm = store.create("x");
        QSignalSpy changed(vm, &Vm::changed);
        ArgsFile args = ArgsFile::parse("# mine\n-name y\n-m 2G\n");
        QString error;

        QVERIFY2(vm->save(args, &error), qPrintable(error));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(read(vm->argsPath()), "# mine\n-name y\n-m 2G\n");
        QCOMPARE(vm->name(), "y");
        QCOMPARE(vm->id(), "x");
        /* our own save is no outside edit */
        QTest::qWait(200);
        QCOMPARE(changed.size(), 1);
    }

    void editedOutside()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.path());
        Vm *vm = store.create("x");
        QSignalSpy changed(vm, &Vm::changed);

        write(vm->argsPath(), "-name edited\n");
        QVERIFY(changed.wait());
        QCOMPARE(vm->name(), "edited");

        /* editors replace the file: the watch must survive */
        write(vm->dir() + "/vm.args.new", "-name replaced\n");
        QVERIFY(QFile::remove(vm->argsPath()));
        QVERIFY(QFile::rename(vm->dir() + "/vm.args.new", vm->argsPath()));
        QTRY_COMPARE(vm->name(), "replaced");
        write(vm->argsPath(), "-name again\n");
        QTRY_COMPARE(vm->name(), "again");
    }

    void foldersAddedAndRemovedOutside()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.path());
        QSignalSpy added(&store, &VmStore::added);
        QSignalSpy removed(&store, &VmStore::removed);

        /* not a VM without vm.args */
        QDir().mkpath(tmp.filePath("junk"));
        write(tmp.filePath("copied/vm.args"), "-name Copied\n");
        QTRY_COMPARE(added.size(), 1);
        QCOMPARE(store.vms().size(), 1);
        QCOMPARE(store.vms()[0]->name(), "Copied");

        QVERIFY(QDir(tmp.filePath("copied")).removeRecursively());
        QTRY_COMPARE(removed.size(), 1);
        QCOMPARE(removed[0][0].toString(), "copied");
        QVERIFY(store.vms().isEmpty());
    }

    void existingFolders()
    {
        QTemporaryDir tmp;
        write(tmp.filePath("one/vm.args"), "-name One\n");
        write(tmp.filePath("two/vm.args"), "-m 1G\n");

        VmStore store(tmp.path());
        QCOMPARE(store.vms().size(), 2);
        QCOMPARE(store.find("one")->name(), "One");
        QCOMPARE(store.find("two")->name(), "two");
        QCOMPARE(store.find("two")->args().lines.size(), 1);
    }

    void diskImage()
    {
        QString qemu = qEnvironmentVariable("QGM_TEST_QEMU");
        QTemporaryDir tmp;
        QString error;

        if (qemu.isEmpty()) {
            qemu = QStandardPaths::findExecutable("qemu-system-x86_64");
        }
        if (qemu.isEmpty()) {
            QSKIP("no QEMU binary, set QGM_TEST_QEMU");
        }
        Paths::setQemuBinary(qemu);
        if (QFileInfo(QFileInfo(qemu).dir(), "qemu-img").isExecutable()) {
            QCOMPARE(Paths::qemuImg(), QFileInfo(qemu).dir().filePath("qemu-img"));
        }
        if (Paths::qemuImg().isEmpty()) {
            QSKIP("no qemu-img");
        }
        QVERIFY2(createDiskImage(tmp.filePath("disk.qcow2"), 64LL << 20, &error),
                 qPrintable(error));
        QVERIFY(QFileInfo::exists(tmp.filePath("disk.qcow2")));
        QVERIFY(!createDiskImage(tmp.filePath("missing/disk.qcow2"), 1 << 20, &error));
        QVERIFY(!error.isEmpty());
        Paths::setQemuBinary({});
    }
};

QTEST_GUILESS_MAIN(TestVmStore)
#include "test_vmstore.moc"
