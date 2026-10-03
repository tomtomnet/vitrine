// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>

#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/vmrunner.h"

/* Runs $QGM_TEST_QEMU, else the qemu-system-x86_64 in PATH, headless */
static QString testQemu()
{
    const QString env = qEnvironmentVariable("QGM_TEST_QEMU");
    return env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64") : env;
}

static const char kHeadless[] = "-machine q35\n-m 128\n-nodefaults\n-display none\n";

static QString read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

class TestVmRunner : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    QString id;
    QString runDir;
    int count = 0;

    /* A stand-in for virtiofsd */
    QString script(const QString &name, const QByteArray &body)
    {
        const QString path = tmp.filePath(name);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("#!/bin/sh\n" + body + "\n");
            f.close();
        }
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                         QFileDevice::ExeOwner);
        return path;
    }

    qint64 qemuPid() const
    {
        return read(runDir + "/qemu.pid").trimmed().toLongLong();
    }

private slots:
    void initTestCase()
    {
        if (!QFileInfo(testQemu()).isExecutable()) {
            QSKIP("no QEMU build, set QGM_TEST_QEMU");
        }
        QStandardPaths::setTestModeEnabled(true);
        Paths::setQemuBinary(testQemu());
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
        Paths::setVirtiofsd({});
    }

    void init()
    {
        id = QString("qgm-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(++count);
        runDir = QFileInfo(VmRunner(id, tmp.path()).commandLine({}).last()).absolutePath();
        Paths::setVirtiofsd({});
    }

    void cleanup()
    {
        /* whatever a failed test left running */
        const qint64 pid = qemuPid();
        if (pid > 0) {
            ::kill(pid_t(pid), SIGKILL);
        }
        QDir().rmdir(runDir);
    }

    void commandLine()
    {
        const VmRunner runner(id, tmp.path());
        const QStringList command = runner.commandLine(
            ArgsFile::parse("-m 1G\n#share tag=pub,path=/home/x\n# note\n"));

        QCOMPARE(command.size(), 11);
        QCOMPARE(command[0], testQemu());
        QCOMPARE(command.mid(1, 2), QStringList({"-m", "1G"}));
        QCOMPARE(command[3], "-chardev");
        QCOMPARE(command[4], "socket,id=qgm-fs0,path=" + runDir + "/fs0.sock");
        QCOMPARE(command[5], "-device");
        QCOMPARE(command[6], "vhost-user-fs-pci,queue-size=1024,chardev=qgm-fs0,tag=pub");
        QCOMPARE(command[7], "-qmp");
        QCOMPARE(command[8], "unix:" + runDir + "/qmp.sock,server=on,wait=off");
        QCOMPARE(command[9], "-pidfile");
        QCOMPARE(command[10], runDir + "/qemu.pid");
        QVERIFY(runDir.toLocal8Bit().size() < 90);
    }

    /* a share to mount: the port of the guest agent, unless the VM has its own */
    void agentPort()
    {
        const VmRunner runner(id, tmp.path());
        const QStringList mount = runner.commandLine(
            ArgsFile::parse("-m 1G\n#share tag=pub,path=/home/x,mount=/mnt/pub\n"));
        const QStringList noMount = runner.commandLine(
            ArgsFile::parse("-m 1G\n#share tag=pub,path=/home/x\n"));
        const QStringList own = runner.commandLine(ArgsFile::parse(
            "-m 1G\n#share tag=pub,path=/home/x,mount=/mnt/pub\n"
            "-device virtserialport,chardev=ga,name=org.qemu.guest_agent.0\n"));

        QVERIFY(mount.contains("socket,id=qgm-ga,path=" + runDir + "/qga.sock,server=on,wait=off"));
        QVERIFY(mount.contains("virtserialport,bus=qgm-serial.0,chardev=qgm-ga,"
                               "name=org.qemu.guest_agent.0,id=qgm-ga-port"));
        QCOMPARE(mount.size(), noMount.size() + 6 + 2);     // and -smbios
        QVERIFY(!own.join(' ').contains("qgm-ga"));
    }

    /* systemd in the guest mounts what it reads from SMBIOS, as if in /etc/fstab */
    void fstabCredential()
    {
        const VmRunner runner(id, tmp.path());
        auto fstab = [&runner](const QString &args) {
            const QStringList command = runner.commandLine(ArgsFile::parse(args));
            const QString prefix = "type=11,value=io.systemd.credential.binary:fstab.extra=";
            QStringList found;

            for (qsizetype i = 1; i < command.size(); i++) {
                if (command[i - 1] == "-smbios" && command[i].startsWith(prefix)) {
                    found << QString::fromUtf8(
                        QByteArray::fromBase64(command[i].mid(prefix.size()).toLatin1()));
                }
            }
            return found.isEmpty() ? QString("none") : found.join("+");
        };

        QCOMPARE(fstab("#share tag=pub,path=/home/x,mount=/mnt/pub\n"
                       "#share tag=docs,path=/home/y,readonly=on,mount=/home/me/My docs\n"
                       "#share tag=other,path=/home/z\n"),
                 "pub /mnt/pub virtiofs nofail 0 0\n"
                 "docs /home/me/My\\040docs virtiofs ro,nofail 0 0\n");
        /* with an agent of its own too */
        QCOMPARE(fstab("#share tag=pub,path=/home/x,mount=/mnt/pub\n"
                       "-device virtserialport,chardev=ga,name=org.qemu.guest_agent.0\n"),
                 "pub /mnt/pub virtiofs nofail 0 0\n");
        QCOMPARE(fstab("#share tag=pub,path=/home/x\n"), "none");
        /* its own credential */
        QCOMPARE(fstab("#share tag=pub,path=/home/x,mount=/mnt/pub\n"
                       "-smbios type=11,value=io.systemd.credential:fstab.extra=a /b none bind\n"),
                 "none");
        /* targets without -smbios */
        QCOMPARE(fstab("#qemu /opt/qemu/bin/qemu-system-ppc64\n"
                       "#share tag=pub,path=/home/x,mount=/mnt/pub\n"), "none");
        QCOMPARE(fstab("#qemu /opt/qemu/bin/qemu-system-aarch64\n"
                       "#share tag=pub,path=/home/x,mount=/mnt/pub\n"),
                 "pub /mnt/pub virtiofs nofail 0 0\n");
        QCOMPARE(fstab("-machine isapc\n#share tag=pub,path=/home/x,mount=/mnt/pub\n"), "none");
    }

    void longIdsKeepShortSockets()
    {
        const VmRunner runner(QString(100, 'x'), tmp.path());
        const QString socket = runner.commandLine({}).at(2).mid(5).section(',', 0, 0);

        QVERIFY(socket.endsWith("/qmp.sock"));
        QVERIFY(socket.toLocal8Bit().size() < 100);
        QVERIFY(!socket.contains("xxxxxxxxxx"));
        QDir().rmdir(QFileInfo(socket).absolutePath());
    }

    void lifecycle()
    {
        const ArgsFile args = ArgsFile::parse(kHeadless);
        auto *runner = new VmRunner(id, tmp.path());
        QSignalSpy failed(runner, &VmRunner::failed);

        QVERIFY(!runner->qmp());
        runner->start(args);
        QCOMPARE(runner->state(), VmRunner::State::Starting);
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Running, 20000);
        QVERIFY(runner->qmp() && runner->qmp()->isReady());
        QVERIFY(read(runner->logPath()).startsWith("qemu-gui-manager: "));
        QVERIFY(qemuPid() > 0);

        runner->pause();
        QTRY_COMPARE(runner->state(), VmRunner::State::Paused);
        runner->resume();
        QTRY_COMPARE(runner->state(), VmRunner::State::Running);
        runner->reset();
        QTest::qWait(200);
        QCOMPARE(runner->state(), VmRunner::State::Running);
        QCOMPARE(failed.size(), 0);

        /* the manager quits, the VM runs on, the next manager finds it */
        delete runner;
        runner = new VmRunner(id, tmp.path());
        runner->attach();
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Running, 10000);
        delete runner;
        runner = new VmRunner(id, tmp.path());
        QSignalSpy failed2(runner, &VmRunner::failed);
        runner->start(args);
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Running, 10000);

        const qint64 pid = qemuPid();
        runner->forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Stopped, 15000);
        QCOMPARE(failed2.size(), 0);
        QVERIFY(!runner->qmp());
        QVERIFY(!QFileInfo::exists(runDir + "/qmp.sock"));
        QVERIFY(!QFileInfo::exists(runDir + "/qemu.pid"));
        QVERIFY(!QFileInfo::exists(QString("/proc/%1").arg(pid)) ||
                read(QString("/proc/%1/stat").arg(pid)).contains(") Z"));
        delete runner;
    }

    void attachWithoutVm()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy states(&runner, &VmRunner::stateChanged);

        runner.attach();
        QTest::qWait(100);
        QCOMPARE(runner.state(), VmRunner::State::Stopped);
        QCOMPARE(states.size(), 0);
    }

    void badArguments()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);

        runner.start(ArgsFile::parse(QByteArray(kHeadless) + "-device nonexistent\n"));
        QVERIFY(failed.wait(15000));
        const QString error = failed[0][0].toString();
        QVERIFY2(error.contains("nonexistent") && error.contains("-device"), qPrintable(error));
        QCOMPARE(runner.state(), VmRunner::State::Stopped);
        QCOMPARE(runner.errorString(), error);
    }

    void unexpectedExit()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);

        runner.start(ArgsFile::parse(kHeadless));
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        ::kill(pid_t(qemuPid()), SIGKILL);
        QVERIFY(failed.wait(15000));
        QVERIFY(failed[0][0].toString().contains("unexpectedly"));
        QCOMPARE(runner.state(), VmRunner::State::Stopped);
    }

    /* a firmware copy deleted from the VM folder comes back at the start */
    void missingFirmwareCopy()
    {
        const QString code = "/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2";
        const QString vars = "/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2";
        const QString dir = tmp.filePath("firmware");
        if (!QFileInfo::exists(code) || !QFileInfo::exists(vars)) {
            QSKIP("no edk2-ovmf with 4M qcow2 images");
        }
        QDir().mkpath(dir);
        QVERIFY(QFile::copy(code, dir + "/OVMF_CODE_4M.qcow2"));
        VmRunner runner(id, dir);
        QSignalSpy failed(&runner, &VmRunner::failed);

        runner.start(ArgsFile::parse(
            QByteArray(kHeadless) +
            "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=OVMF_CODE_4M.qcow2\n"
            "-drive if=pflash,format=qcow2,unit=1,file=OVMF_VARS_4M.qcow2\n"));
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        QVERIFY(QFileInfo(dir + "/OVMF_VARS_4M.qcow2").isWritable());
        QVERIFY(read(runner.logPath())
                    .contains("qemu-gui-manager: OVMF_VARS_4M.qcow2 was missing: a new copy of " +
                              vars + '\n'));
        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 15000);
        QCOMPARE(failed.size(), 0);
    }

    void noQemu()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);

        Paths::setQemuBinary(tmp.filePath("missing"));
        runner.start(ArgsFile::parse(kHeadless));
        Paths::setQemuBinary(testQemu());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(runner.state(), VmRunner::State::Stopped);
    }

    /* #qemu runs the VM with its own QEMU */
    void ownQemu()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);

        Paths::setQemuBinary(tmp.filePath("missing"));
        runner.start(ArgsFile::parse("#qemu " + testQemu() + "\n" + kHeadless));
        Paths::setQemuBinary(testQemu());
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 15000);
        QCOMPARE(failed.size(), 0);

        runner.start(ArgsFile::parse("#qemu " + tmp.filePath("missing") + "\n" + kHeadless));
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed[0][0].toString().contains("#qemu"));
    }

    void sharesNeedSharedMemory()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);

        runner.start(ArgsFile::parse(QByteArray(kHeadless) + "#share tag=t,path=" +
                                     tmp.path().toUtf8() + "\n"));
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed[0][0].toString().contains("memory"));
        QCOMPARE(runner.state(), VmRunner::State::Stopped);
    }

    void helperFails()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);

        Paths::setVirtiofsd(script("failing-virtiofsd", "echo \"cannot share $2\" >&2\nexit 1"));
        runner.start(ArgsFile::parse(
            "-machine q35,memory-backend=mem\n"
            "-object memory-backend-memfd,id=mem,size=128M\n"
            "#share tag=t,path=" + tmp.path() + "\n"));
        QCOMPARE(runner.state(), VmRunner::State::Starting);
        QVERIFY(failed.wait(5000));
        QVERIFY2(failed[0][0].toString().contains("cannot share --shared-dir=" + tmp.path()),
                 qPrintable(failed[0][0].toString()));
        QCOMPARE(runner.state(), VmRunner::State::Stopped);
    }

    void forceOffWhileWaitingForHelper()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);
        const QString pidFile = tmp.filePath("helper.pid");

        Paths::setVirtiofsd(script("slow-virtiofsd", "echo $$ > " + pidFile.toUtf8() +
                                                         "\nwhile :; do sleep 0.1; done"));
        runner.start(ArgsFile::parse(
            "-machine q35,memory-backend=mem\n"
            "-object memory-backend-memfd,id=mem,size=128M\n"
            "#share tag=t,path=" + tmp.path() + "\n"));
        QTRY_VERIFY(read(pidFile).trimmed().toLongLong() > 0);
        const qint64 helper = read(pidFile).trimmed().toLongLong();
        QCOMPARE(runner.state(), VmRunner::State::Starting);
        runner.forceOff();
        QCOMPARE(runner.state(), VmRunner::State::Stopped);
        QCOMPARE(failed.size(), 0);
        QTRY_VERIFY(!QFileInfo::exists(QString("/proc/%1").arg(helper)) ||
                    read(QString("/proc/%1/stat").arg(helper)).contains(") Z"));
    }

    void sharedFolder()
    {
        if (Paths::virtiofsd().isEmpty()) {
            VmRunner runner(id, tmp.path());
            QSignalSpy failed(&runner, &VmRunner::failed);

            runner.start(ArgsFile::parse("-machine q35,memory-backend=mem\n"
                                         "-object memory-backend-memfd,id=mem,size=128M\n"
                                         "#share tag=t,path=/tmp\n"));
            QCOMPARE(failed.size(), 1);
            QVERIFY(failed[0][0].toString().contains("virtiofsd"));
            QSKIP("virtiofsd is not installed");
        }

        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);
        QTemporaryDir shared;

        runner.start(ArgsFile::parse(QByteArray(kHeadless).replace("-machine q35\n", "") +
                                     "-machine q35,memory-backend=mem\n"
                                     "-object memory-backend-memfd,id=mem,size=128M\n"
                                     "#share tag=t,path=" + shared.path().toUtf8() + "\n"));
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        QVERIFY2(failed.isEmpty(), qPrintable(read(runner.logPath())));
        QVERIFY(read(runner.logPath()).contains("virtiofsd"));
        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 15000);
        QVERIFY(failed.isEmpty());
        QTRY_VERIFY(!QFileInfo::exists(runDir + "/fs0.sock"));
    }
};

QTEST_GUILESS_MAIN(TestVmRunner)
#include "test_vmrunner.moc"
