// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <csignal>

#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/vmrunner.h"

/* Runs $VITRINE_TEST_QEMU, else the qemu-system-x86_64 in PATH, headless */
static QString testQemu()
{
    const QString env = qEnvironmentVariable("VITRINE_TEST_QEMU");
    return env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64") : env;
}

static const char kHeadless[] = "-machine q35\n-m 128\n-nodefaults\n-display none\n";

static QString read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

/*
 * Dead and waiting to be reaped, or reaped: once true it stays true.  The
 * stat first, then kill: the kernel unhashes the pid before it drops
 * /proc/PID, so a stat that no longer reads means kill fails too, while
 * the other way round a zombie reaped between the two looked alive.
 */
static bool gone(qint64 pid)
{
    const QString stat = read(QString("/proc/%1/stat").arg(pid));
    if (stat.contains(") Z") || stat.contains(") X")) {
        return true;
    }
    return ::kill(pid_t(pid), 0) != 0 && errno == ESRCH;
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
            QSKIP("no QEMU build, set VITRINE_TEST_QEMU");
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
        id = QString("vitrine-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(++count);
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
        QCOMPARE(command[4], "socket,id=vitrine-fs0,path=" + runDir + "/fs0.sock");
        QCOMPARE(command[5], "-device");
        QCOMPARE(command[6], "vhost-user-fs-pci,queue-size=1024,chardev=vitrine-fs0,tag=pub");
        QCOMPARE(command[7], "-qmp");
        QCOMPARE(command[8], "unix:" + runDir + "/qmp.sock,server=on,wait=off");
        QCOMPARE(command[9], "-pidfile");
        QCOMPARE(command[10], runDir + "/qemu.pid");
        QVERIFY(runDir.toLocal8Bit().size() < 90);
    }

    /* the view attaches to the D-Bus display through a monitor of its own */
    void displayMonitor()
    {
        const VmRunner runner(id, tmp.path());
        const QStringList embedded =
            runner.commandLine(ArgsFile::parse("-m 1G\n-display dbus,p2p=yes,gl=on\n"));
        const QStringList sdl = runner.commandLine(ArgsFile::parse("-m 1G\n-display sdl,gl=on\n"));
        const QStringList bus = runner.commandLine(ArgsFile::parse("-m 1G\n-display dbus\n"));
        const QString chardev =
            "socket,id=vitrine-display,path=" + runDir + "/display.sock,server=on,wait=off";

        QCOMPARE(embedded.mid(5, 4), QStringList({"-chardev", chardev, "-mon",
                                                  "chardev=vitrine-display,mode=control"}));
        QVERIFY(!sdl.contains(chardev));
        QVERIFY(!bus.contains(chardev));
        QCOMPARE(runner.displaySocket(), "");   // not running
    }

    /* The swap targets of the 3D card, where vm.args leaves them out */
    void computedProperties()
    {
        QStringList asked;
        auto props = [&](const QStringList &known) {
            return [&asked, known](const QString &driver) {
                asked << driver;
                return known;
            };
        };
        auto computed = [&](const QString &args, const QStringList &known = {
                                "blob", "x-vblank-swap-target", "x-vblank-swap-target-zc"}) {
            return VmRunner::withComputedProperties(ArgsFile::parse(args), props(known)).toText();
        };

        /* KDE in QEMU's window */
        QCOMPARE(computed("#guest linux,desktop=kde\n-device virtio-gpu-gl-pci,blob=on\n"
                          "-display sdl,gl=on\n"),
                 "#guest linux,desktop=kde\n-device virtio-gpu-gl-pci,blob=on,"
                 "x-vblank-swap-target=4500,x-vblank-swap-target-zc=3500\n"
                 "-display sdl,gl=on\n");
        QCOMPARE(asked, QStringList({"virtio-gpu-gl-pci"}));
        /* another desktop, or none known, in vitrine's window */
        QCOMPARE(computed("#guest linux,desktop=gnome\n-device virtio-vga-gl\n"
                          "-display dbus,p2p=yes,gl=on\n"),
                 "#guest linux,desktop=gnome\n-device virtio-vga-gl,"
                 "x-vblank-swap-target=6000,x-vblank-swap-target-zc=4500\n"
                 "-display dbus,p2p=yes,gl=on\n");
        QCOMPARE(computed("-device virtio-vga-gl\n"),
                 "-device virtio-vga-gl,x-vblank-swap-target=6000,"
                 "x-vblank-swap-target-zc=3500\n");
        /* the user's values win */
        QCOMPARE(computed("#guest linux,desktop=kde\n"
                          "-device virtio-gpu-gl-pci,x-vblank-swap-target-zc=0\n"),
                 "#guest linux,desktop=kde\n-device virtio-gpu-gl-pci,"
                 "x-vblank-swap-target-zc=0,x-vblank-swap-target=4500\n");
        /* a QEMU without them, or one whose properties are not known */
        QCOMPARE(computed("-device virtio-gpu-gl-pci,blob=on\n", {"blob"}),
                 "-device virtio-gpu-gl-pci,blob=on\n");
        QCOMPARE(computed("-device virtio-gpu-gl-pci\n", {}), "-device virtio-gpu-gl-pci\n");
        QCOMPARE(computed("-device virtio-gpu-gl-pci\n", {"x-vblank-swap-target"}),
                 "-device virtio-gpu-gl-pci,x-vblank-swap-target=6000\n");
        /* other cards: QEMU not even asked */
        asked.clear();
        QCOMPARE(computed("-device virtio-vga\n-device VGA\n-m 1G\n"),
                 "-device virtio-vga\n-device VGA\n-m 1G\n");
        QVERIFY(asked.isEmpty());
    }

    /* The QEMU of the VM tells which properties its card has */
    void computedOnCommandLine()
    {
        const VmRunner runner(id, tmp.path());
        const QString fork = script("fork-qemu", "case \"$2\" in\n"
                                                 "virtio-gpu-gl-pci,help) cat <<EOF\n"
                                                 "virtio-gpu-gl-pci options:\n"
                                                 "  blob=<bool>            - on/off (default: off)\n"
                                                 "  x-vblank-swap-target-zc=<uint32> -  (default: 0)\n"
                                                 "  x-vblank-swap-target=<uint32> -  (default: 6000)\n"
                                                 "EOF\n;;\nesac");
        const QString plain = script("plain-qemu", "echo 'virtio-gpu-gl-pci options:'\n"
                                                   "echo '  blob=<bool>            - on/off'");
        const QString vm = "-device virtio-gpu-gl-pci,blob=on\n#guest linux,desktop=kde\n"
                           "-display dbus,p2p=yes,gl=on\n";

        QCOMPARE(runner.commandLine(ArgsFile::parse("#qemu " + fork + "\n" + vm)).mid(1, 2),
                 QStringList({"-device", "virtio-gpu-gl-pci,blob=on,x-vblank-swap-target=4500,"
                                         "x-vblank-swap-target-zc=4500"}));
        QCOMPARE(runner.commandLine(ArgsFile::parse("#qemu " + plain + "\n" + vm)).mid(1, 2),
                 QStringList({"-device", "virtio-gpu-gl-pci,blob=on"}));
        QCOMPARE(runner.commandLine(ArgsFile::parse("#qemu /nonexistent/qemu\n" + vm)).mid(1, 2),
                 QStringList({"-device", "virtio-gpu-gl-pci,blob=on"}));
    }

    /* QEMU's environment: the SDL window's settings, then #env */
    void environment()
    {
        QCOMPARE(VmRunner::environment(ArgsFile::parse("-display sdl,gl=on\n")),
                 QStringList({"QEMU_SDL_POLL_FOCUSED=1", "QEMU_SDL_ZERO_COPY=1",
                              "QEMU_SDL_ZC_TILED=explicit"}));
        QCOMPARE(VmRunner::environment(ArgsFile::parse(
                     "-display sdl,gl=on\n#env QEMU_SDL_ZERO_COPY=0\n#env A=b\n")),
                 QStringList({"QEMU_SDL_POLL_FOCUSED=1", "QEMU_SDL_ZC_TILED=explicit",
                              "QEMU_SDL_ZERO_COPY=0", "A=b"}));
        QCOMPARE(VmRunner::environment(ArgsFile::parse("-display dbus,p2p=yes,gl=on\n")),
                 QStringList());
        QCOMPARE(VmRunner::environment(ArgsFile::parse("-display gtk\n#env A=b\n")),
                 QStringList({"A=b"}));
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

        QVERIFY(mount.contains("socket,id=vitrine-ga,path=" + runDir + "/qga.sock,server=on,wait=off"));
        QVERIFY(mount.contains("virtserialport,bus=vitrine-serial.0,chardev=vitrine-ga,"
                               "name=org.qemu.guest_agent.0,id=vitrine-ga-port"));
        QCOMPARE(mount.size(), noMount.size() + 6 + 2);     // and -smbios
        QVERIFY(!own.join(' ').contains("vitrine-ga"));
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
        QVERIFY(read(runner->logPath()).startsWith("vitrine: "));
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
        QVERIFY(gone(pid));
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
                    .contains("vitrine: OVMF_VARS_4M.qcow2 was missing: a new copy of " +
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
        QTRY_VERIFY2(gone(helper), qPrintable(read(QString("/proc/%1/stat").arg(helper))));
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
