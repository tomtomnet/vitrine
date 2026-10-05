// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "core/paths.h"
#include "core/vmrunner.h"
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

    /* The card update's Don't Ask Again goes with the prompt; the other settings stay */
    void oldCardUpdateSettings()
    {
        QTemporaryDir tmp;
        {
            QSettings s(Paths::settingsPath(), QSettings::IniFormat);
            s.setValue("cardupdate/declined/Fedora", true);
            s.setValue("cardupdate/declined/Other", true);
            s.setValue("qemu/binary", "/usr/bin/qemu-system-x86_64");
        }
        VmStore store(tmp.path());
        QSettings s(Paths::settingsPath(), QSettings::IniFormat);
        QVERIFY(!s.childGroups().contains("cardupdate"));
        QCOMPARE(s.value("qemu/binary").toString(), "/usr/bin/qemu-system-x86_64");
        s.remove("qemu");
    }

    /* Another folder: its VMs as found there, the old ones left where they are */
    void setDirListsTheOtherFolder()
    {
        QTemporaryDir a, b;
        write(a.filePath("one/vm.args"), "-name One\n");
        write(a.filePath("both/vm.args"), "-name Both in A\n");
        write(b.filePath("two/vm.args"), "-name Two\n");
        write(b.filePath("both/vm.args"), "-name Both in B\n");
        QDir().mkpath(b.filePath("junk"));

        VmStore store(a.path());
        QSignalSpy added(&store, &VmStore::added);
        QSignalSpy removed(&store, &VmStore::removed);
        QSignalSpy changed(&store, &VmStore::dirChanged);
        QCOMPARE(store.vms().size(), 2);

        store.setDir(b.path());
        QCOMPARE(store.dir(), QDir(b.path()).absolutePath());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(removed.size(), 2);
        QCOMPARE(added.size(), 2);
        QCOMPARE(store.vms().size(), 2);
        QVERIFY(!store.find("one"));
        QCOMPARE(store.find("two")->name(), "Two");
        /* the same name in both folders: the new folder's */
        QCOMPARE(store.find("both")->name(), "Both in B");
        QCOMPARE(store.find("both")->dir(), b.filePath("both"));
        /* nothing moved or deleted */
        QVERIFY(QFileInfo::exists(a.filePath("one/vm.args")));
        QVERIFY(QFileInfo::exists(a.filePath("both/vm.args")));
        /* new VMs go to the new folder, which is watched */
        QCOMPARE(store.create("Three")->dir(), b.filePath("Three"));
        write(b.filePath("four/vm.args"), "-name Four\n");
        QTRY_VERIFY(store.find("four"));
        /* the old folder is not watched any more */
        write(a.filePath("five/vm.args"), "-name Five\n");
        QTest::qWait(200);
        QVERIFY(!store.find("five"));

        /* and back: as they were */
        store.setDir(a.path());
        QCOMPARE(store.vms().size(), 3);
        QCOMPARE(store.find("both")->name(), "Both in A");
        QVERIFY(store.find("five"));
        QVERIFY(!store.find("two"));
    }

    /*
     * A VM of the old folder still running stays listed, its name taken,
     * until it stops; then it leaves the list
     */
    void setDirKeepsRunningVms()
    {
        const QString env = qEnvironmentVariable("VITRINE_TEST_QEMU");
        const QString qemu = env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64")
                                           : env;
        if (!QFileInfo(qemu).isExecutable()) {
            QSKIP("no QEMU, set VITRINE_TEST_QEMU");
        }
        QTemporaryDir a, b;
        const QString id = QString("vitrine-store-%1").arg(QCoreApplication::applicationPid());
        write(a.filePath(id + "/vm.args"), "-machine q35\n-m 128\n-nodefaults\n-display none\n");
        write(b.filePath("other/vm.args"), "-name Other\n");
        Paths::setQemuBinary(qemu);
        VmStore store(a.path());
        Vm *vm = store.find(id);
        QVERIFY(vm);
        vm->runner()->start(vm->args());
        QTRY_COMPARE_WITH_TIMEOUT(vm->runner()->state(), VmRunner::State::Running, 20000);

        QSignalSpy removed(&store, &VmStore::removed);
        store.setDir(b.path());
        QCOMPARE(store.find(id), vm);
        QVERIFY(store.find("other"));
        QVERIFY(removed.isEmpty());
        /* its name stays taken meanwhile */
        QCOMPARE(store.create(id)->id(), id + "-2");

        vm->runner()->forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(removed.size(), 1, 20000);
        QCOMPARE(removed[0][0].toString(), id);
        QVERIFY(!store.find(id));
        QVERIFY(QFileInfo::exists(a.filePath(id + "/vm.args")));
        Paths::setQemuBinary({});
    }

    /* The folder of the VMs: chosen, else the default */
    void vmsDirSetting()
    {
        QTemporaryDir tmp;
        QCOMPARE(Paths::vmsDir(), Paths::defaultVmsDir());
        Paths::setVmsDir(tmp.path());
        QCOMPARE(Paths::vmsDir(), QDir(tmp.path()).absolutePath());
        /* the default chosen is no choice: it follows the data folder */
        Paths::setVmsDir(Paths::defaultVmsDir());
        QCOMPARE(Paths::vmsDir(), Paths::defaultVmsDir());
        Paths::setVmsDir(tmp.path());
        Paths::setVmsDir({});
        QCOMPARE(Paths::vmsDir(), Paths::defaultVmsDir());
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
        QString qemu = qEnvironmentVariable("VITRINE_TEST_QEMU");
        QTemporaryDir tmp;
        QString error;

        if (qemu.isEmpty()) {
            qemu = QStandardPaths::findExecutable("qemu-system-x86_64");
        }
        if (qemu.isEmpty()) {
            QSKIP("no QEMU binary, set VITRINE_TEST_QEMU");
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
