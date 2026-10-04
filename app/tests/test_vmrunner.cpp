// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cerrno>
#include <csignal>

#include <unistd.h>

#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmtemplate.h"

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

/*
 * Starts a stand-in for a QEMU found running, and waits until it runs its
 * program: waitForStarted() may return while the kernel still sets the
 * program up (a vfork'ed child lets its parent go at exec), and a runner
 * that reads /proc/PID/cmdline then finds it empty - a QEMU found running
 * wrote its pid file long after its exec
 */
static bool startStandIn(QProcess &process, const QString &program, const QStringList &arguments)
{
    process.start(program, arguments);
    if (!process.waitForStarted()) {
        return false;
    }
    const QString path = QString("/proc/%1/cmdline").arg(process.processId());
    QElapsedTimer clock;
    clock.start();
    while (!read(path).contains(arguments.last()) && clock.elapsed() < 5000) {
        QThread::msleep(5);
    }
    return read(path).contains(arguments.last());
}

/*
 * Ends a stand-in started in a process group of its own, with what it
 * started.  Only while QProcess still runs it: its pid, then, is that
 * group's (processId() is 0 once it finished, and kill(0) would hit the
 * test's own group).
 */
static void stopGroup(QProcess &process)
{
    const pid_t group = pid_t(process.processId());

    if (process.state() != QProcess::NotRunning && group > 0) {
        ::kill(-group, SIGKILL);
    }
    process.waitForFinished();
}

/* QEMU's end of QMP: answers every command, query-status with @status;
   events on demand */
class FakeMonitor : public QObject
{
public:
    explicit FakeMonitor(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            m_peers << peer;
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() { read(peer); });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }
    QString status = "running";
    /* to every monitor connection */
    void event(const QString &name)
    {
        for (QLocalSocket *peer : std::as_const(m_peers)) {
            if (peer->state() == QLocalSocket::ConnectedState) {
                peer->write(QJsonDocument(QJsonObject{{"event", name}, {"data", QJsonObject()}})
                                .toJson(QJsonDocument::Compact) + '\n');
            }
        }
    }
    void close()
    {
        for (QLocalSocket *peer : std::as_const(m_peers)) {
            peer->disconnectFromServer();
        }
    }

private:
    void read(QLocalSocket *peer)
    {
        QByteArray &buffer = m_buffers[peer];
        buffer += peer->readAll();
        qsizetype nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            const QJsonObject command = QJsonDocument::fromJson(buffer.left(nl)).object();
            QJsonObject reply{{"return", QJsonObject()}, {"id", command["id"]}};
            buffer.remove(0, nl + 1);
            if (command["execute"] == "query-status") {
                reply["return"] = QJsonObject{{"running", status == "running"}, {"status", status}};
            }
            peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
        }
    }

    QLocalServer m_server;
    QList<QLocalSocket *> m_peers;
    QHash<QLocalSocket *, QByteArray> m_buffers;
};

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

        QCOMPARE(command.size(), 17);
        QCOMPARE(command[0], testQemu());
        QCOMPARE(command.mid(1, 2), QStringList({"-m", "1G"}));
        QCOMPARE(command[3], "-chardev");
        QCOMPARE(command[4], "socket,id=vitrine-fs0,path=" + runDir + "/fs0.sock");
        QCOMPARE(command[5], "-device");
        QCOMPARE(command[6], "vhost-user-fs-pci,queue-size=1024,chardev=vitrine-fs0,tag=pub");
        /* the guest tools' agent (test_guesttools) */
        QCOMPARE(command.mid(7, 2), QStringList({"-device", "virtio-serial-pci,id=vitrine-serial"}));
        QCOMPARE(command[13], "-qmp");
        QCOMPARE(command[14], "unix:" + runDir + "/qmp.sock,server=on,wait=off");
        QCOMPARE(command[15], "-pidfile");
        QCOMPARE(command[16], runDir + "/qemu.pid");
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
        const QString monitor = "unix:" + runDir + "/display.sock,server=on,wait=off";

        QCOMPARE(embedded.mid(5, 2), QStringList({"-qmp", monitor}));
        QVERIFY(!sdl.contains(monitor));
        QVERIFY(!bus.contains(monitor));
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
        /* and those of -global, in either form, which QEMU would apply first */
        QCOMPARE(computed("-device virtio-gpu-gl-pci\n"
                          "-global virtio-gpu-gl-pci.x-vblank-swap-target=5000\n"),
                 "-device virtio-gpu-gl-pci,x-vblank-swap-target-zc=3500\n"
                 "-global virtio-gpu-gl-pci.x-vblank-swap-target=5000\n");
        QCOMPARE(computed("-global driver=virtio-gpu-gl-device,property=x-vblank-swap-target-zc,"
                          "value=0\n-device virtio-vga-gl\n"),
                 "-global driver=virtio-gpu-gl-device,property=x-vblank-swap-target-zc,"
                 "value=0\n-device virtio-vga-gl,x-vblank-swap-target=6000\n");
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

    /* A QEMU that could not answer once, e.g. on a host short of memory, is
       asked again; the run's log says what its card went without */
    void failedProbesAreNotKept()
    {
        VmRunner runner(id, tmp.path());
        const QByteArray once = tmp.filePath("answered-once").toUtf8();
        const QString qemu = script("flaky-qemu", "case \"$2\" in\n"
                                                  "*,help)\n"
                                                  "  [ -e " + once + " ] || { : > " + once + "; "
                                                  "echo 'out of memory' >&2; exit 1; }\n"
                                                  "  echo 'virtio-gpu-gl-pci options:'\n"
                                                  "  echo '  x-vblank-swap-target=<uint32>'\n"
                                                  "  exit 0 ;;\n"
                                                  "esac\n"
                                                  "exit 1");
        const ArgsFile args = ArgsFile::parse("#qemu " + qemu + "\n-device virtio-gpu-gl-pci\n");

        QFile::remove(QString::fromUtf8(once));
        runner.start(args);
        QTRY_COMPARE(runner.state(), VmRunner::State::Stopped);
        const QString log = read(runner.logPath());
        QVERIFY2(log.contains("vitrine: cannot read the properties of virtio-gpu-gl-pci from " +
                              qemu + " (out of memory)"),
                 qPrintable(log));
        QCOMPARE(runner.commandLine(args).mid(1, 2),
                 QStringList({"-device", "virtio-gpu-gl-pci,x-vblank-swap-target=6000"}));
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

    /* The environment as the log and Show Command Line print it: a shell
       must take it as assignments, with the values QEMU gets */
    void environmentForAShell()
    {
        const QStringList env = VmRunner::environment(ArgsFile::parse(
            "-display gtk\n#env PULSE_PROP=media.role=game application.name=vm\n"
            "#env EMPTY=\n#env TILDE=~/x\n#env QUOTE=it's $HOME\n#env PLAIN=1\n"));
        const QStringList assignments = VmRunner::shellAssignments(env);

        QCOMPARE(assignments,
                 QStringList({"PULSE_PROP='media.role=game application.name=vm'", "EMPTY=''",
                              "TILDE='~/x'", "QUOTE='it'\\''s $HOME'", "PLAIN=1"}));
        QProcess sh;
        sh.start("/bin/sh", {"-c", assignments.join(' ') +
                                       " printenv PULSE_PROP EMPTY TILDE QUOTE PLAIN"});
        QVERIFY(sh.waitForFinished(10000));
        QCOMPARE(sh.exitCode(), 0);
        QCOMPARE(QString::fromUtf8(sh.readAllStandardOutput()),
                 "media.role=game application.name=vm\n\n~/x\nit's $HOME\n1\n");
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
        /* and -smbios; the guest tools' agent shares the controller */
        QCOMPARE(mount.size(), noMount.size() + 6 + 2 - 2);
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
        const QStringList command = runner.commandLine({});
        const QString socket = command.at(command.indexOf("-qmp") + 1).mid(5).section(',', 0, 0);

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
        QCOMPARE(read(runDir + "/run.args"), args.toText());

        runner->pause();
        QTRY_COMPARE(runner->state(), VmRunner::State::Paused);
        runner->resume();
        QTRY_COMPARE(runner->state(), VmRunner::State::Running);
        runner->reset();
        QTest::qWait(200);
        QCOMPARE(runner->state(), VmRunner::State::Running);
        QCOMPARE(failed.size(), 0);

        /* the manager quits, the VM runs on, the next manager finds it, with
           the arguments it runs with, whatever vm.args says now */
        const ArgsFile edited = ArgsFile::parse("-display dbus,p2p=yes,gl=on\n");
        delete runner;
        runner = new VmRunner(id, tmp.path());
        runner->attach(edited);
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Running, 10000);
        QCOMPARE(runner->runArgs().toText(), args.toText());
        QCOMPARE(runner->displaySocket(), "");
        /* one started by a vitrine that did not keep them: what it serves */
        delete runner;
        QVERIFY(QFile::remove(runDir + "/run.args"));
        runner = new VmRunner(id, tmp.path());
        runner->attach(edited);
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Running, 10000);
        QCOMPARE(runner->displaySocket(), "");
        delete runner;
        /* kept again: start() on the QEMU found running takes them over
           vm.args, and they go with the other runtime files when it stops */
        QFile kept(runDir + "/run.args");
        QVERIFY(kept.open(QIODevice::WriteOnly));
        kept.write(args.toText().toUtf8());
        kept.close();
        runner = new VmRunner(id, tmp.path());
        QSignalSpy failed2(runner, &VmRunner::failed);
        runner->start(edited);
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Running, 10000);
        QCOMPARE(runner->runArgs().toText(), args.toText());
        QCOMPARE(runner->displaySocket(), "");
        QVERIFY(QFileInfo::exists(runDir + "/run.args"));

        const qint64 pid = qemuPid();
        runner->forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner->state(), VmRunner::State::Stopped, 15000);
        QCOMPARE(failed2.size(), 0);
        QVERIFY(!runner->qmp());
        QVERIFY(!QFileInfo::exists(runDir + "/qmp.sock"));
        QVERIFY(!QFileInfo::exists(runDir + "/qemu.pid"));
        QVERIFY(!QFileInfo::exists(runDir + "/run.args"));
        QVERIFY(gone(pid));
        delete runner;
    }

    /* What vitrine finds out later goes at the end of the log, as notes */
    void appendNote()
    {
        VmRunner runner(id, tmp.path());
        QFile log(runner.logPath());
        QVERIFY(log.open(QIODevice::WriteOnly | QIODevice::Truncate));
        log.write("vitrine: started\nqemu-system-x86_64: a line of QEMU's\n");
        log.close();
        runner.appendNote("the first\nthe second");
        QCOMPARE(read(runner.logPath()),
                 QString("vitrine: started\nqemu-system-x86_64: a line of QEMU's\n"
                         "vitrine: the first\nvitrine: the second\n"));
        QVERIFY(QFile::remove(runner.logPath()));
    }

    /*
     * A guest's own suspend to RAM is no pause from here: QEMU takes input
     * there.  A QEMU stand-in found running, its monitor sending the events.
     */
    void suspended()
    {
        const QStringList command = VmRunner(id, tmp.path()).commandLine({});
        FakeMonitor monitor(runDir + "/qmp.sock");
        QProcess qemu;
        QVERIFY(startStandIn(qemu, FAKE_QEMU, {"-qmp", command[command.size() - 3]}));
        QFile pid(runDir + "/qemu.pid");
        QVERIFY(pid.open(QIODevice::WriteOnly));
        pid.write(QByteArray::number(qemu.processId()) + '\n');
        pid.close();

        VmRunner runner(id, tmp.path());
        QSignalSpy suspended(&runner, &VmRunner::suspendedChanged);
        runner.attach(ArgsFile::parse(kHeadless));
        QTRY_COMPARE(runner.state(), VmRunner::State::Running);
        QVERIFY(!runner.isSuspended());
        monitor.event("SUSPEND");
        QTRY_COMPARE(runner.state(), VmRunner::State::Paused);
        QVERIFY(runner.isSuspended());
        QCOMPARE(suspended.size(), 1);
        /* paused from here as it sleeps: QEMU drops input now */
        monitor.event("STOP");
        QTRY_COMPARE(suspended.size(), 2);
        QCOMPARE(runner.state(), VmRunner::State::Paused);
        QVERIFY(!runner.isSuspended());
        monitor.event("RESUME");
        QTRY_COMPARE(runner.state(), VmRunner::State::Running);
        monitor.event("SUSPEND");
        QTRY_VERIFY(runner.isSuspended());
        monitor.event("WAKEUP");
        QTRY_COMPARE(runner.state(), VmRunner::State::Running);
        QVERIFY(!runner.isSuspended());

        /* found asleep */
        monitor.status = "suspended";
        {
            VmRunner again(id, tmp.path());
            again.attach(ArgsFile::parse(kHeadless));
            QTRY_COMPARE(again.state(), VmRunner::State::Paused);
            QVERIFY(again.isSuspended());
        }

        monitor.close();
        qemu.kill();
        qemu.waitForFinished();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
        QVERIFY(!runner.isSuspended());
    }

    /*
     * A QEMU found running is what it was started as, not what vm.args says
     * now: its screen in this window or in QEMU's, its shares.  One started
     * by a vitrine that did not keep its arguments has vm.args, and its own
     * command line says whether its screen can show here.  Start finds it
     * running too.
     */
    void foundRunning_data()
    {
        QTest::addColumn<QString>("kept");          // run.args; null: none
        QTest::addColumn<bool>("displayMonitor");   // on QEMU's command line
        QTest::addColumn<QString>("vmArgs");
        QTest::addColumn<bool>("viaStart");
        QTest::addColumn<QString>("expected");      // runArgs()
        QTest::addColumn<bool>("embedded");         // displaySocket()

        const QString embedded = "-m 1G\n-display dbus,p2p=yes,gl=on\n";
        const QString sdl = "-m 1G\n-display sdl,gl=on\n";
        QTest::newRow("embedded, now SDL") << embedded << true << sdl << false << embedded << true;
        QTest::newRow("SDL, now embedded") << sdl << false << embedded << false << sdl << false;
        QTest::newRow("by start") << embedded << true << sdl << true << embedded << true;
        /* p2p=true, which an older vitrine's rules did not take for on: no display -qmp */
        const QString older = "-m 1G\n-display dbus,p2p=true,gl=on\n";
        QTest::newRow("kept, not embedded when started")
            << older << false << embedded << false << older << false;
        QTest::newRow("older vitrine, embedded")
            << QString() << true << sdl << false << sdl << true;
        QTest::newRow("older vitrine, SDL")
            << QString() << false << embedded << false << embedded << false;
        QTest::newRow("older vitrine, by start")
            << QString() << true << embedded << true << embedded << true;
    }
    void foundRunning()
    {
        QFETCH(QString, kept);
        QFETCH(bool, displayMonitor);
        QFETCH(QString, vmArgs);
        QFETCH(bool, viaStart);
        QFETCH(QString, expected);
        QFETCH(bool, embedded);
        const QStringList command = VmRunner(id, tmp.path()).commandLine({});
        QStringList arguments{"-qmp", command[command.size() - 3]};
        if (displayMonitor) {
            arguments << "-qmp" << "unix:" + runDir + "/display.sock,server=on,wait=off";
        }
        FakeMonitor monitor(runDir + "/qmp.sock");
        QProcess qemu;
        QVERIFY(startStandIn(qemu, FAKE_QEMU, arguments));
        QFile pid(runDir + "/qemu.pid");
        QVERIFY(pid.open(QIODevice::WriteOnly));
        pid.write(QByteArray::number(qemu.processId()) + '\n');
        pid.close();
        if (!kept.isNull()) {
            QFile f(runDir + "/run.args");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(kept.toUtf8());
        }

        VmRunner runner(id, tmp.path());
        if (viaStart) {
            runner.start(ArgsFile::parse(vmArgs));
        } else {
            runner.attach(ArgsFile::parse(vmArgs));
        }
        QTRY_COMPARE(runner.state(), VmRunner::State::Running);
        QCOMPARE(runner.runArgs().toText(), expected);
        QCOMPARE(runner.displaySocket(), embedded ? runDir + "/display.sock" : QString());

        monitor.close();
        qemu.kill();
        qemu.waitForFinished();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
        QVERIFY(!QFileInfo::exists(runDir + "/run.args"));
    }

    void attachWithoutVm()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy states(&runner, &VmRunner::stateChanged);

        runner.attach(ArgsFile::parse(kHeadless));
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

    /*
     * No QEMU chosen, by #qemu or the preferences, and vitrine's not built:
     * a VM runs with the system's QEMU meanwhile, unless that lacks what
     * the VM uses of vitrine's, or there is none.  Then it waits for
     * vitrine's: start() refuses it.
     */
    void vitrinesQemuToBuild()
    {
        const QString qemu = testQemu();
        const QByteArray path = qgetenv("PATH");
        const QString dir = tmp.filePath("unbuilt");
        const QString stack = Paths::stackDir();
        /* QEMU 10.2: no native context, vblank timing or honor-guest-pat */
        const QString system = tmp.filePath("system/" + Paths::qemuSystemName());
        const QString broken = tmp.filePath("broken/" + Paths::qemuSystemName());
        const auto withHeadless = [](const QString &lines) {
            return ArgsFile::parse(QString(kHeadless) + lines);
        };
        const ArgsFile headless = ArgsFile::parse(kHeadless);
        VmTemplate::Options o;
        QString why;

        /* as the other tests have it, whatever fails here */
        const auto restore = qScopeGuard([=]() {
            qputenv("PATH", path);
            QFile::remove(stack + "/current");
            QDir(stack + "/0123456789abcdef").removeRecursively();
            Paths::setQemuBinary(qemu);
        });

        /* new VMs, made for vitrine's QEMU: Linux gets the 3D card */
        o.name = "Linux";
        o.arch = "x86_64";
        const ArgsFile linuxVm = VmTemplate::build(o);
        QVERIFY(linuxVm.toText().contains(",drm_native_context=on,x-host-vblank=on,"));
        QVERIFY(linuxVm.toText().contains("-accel kvm,honor-guest-pat=auto\n"));
        o.os = VmTemplate::Os::Windows11;
        o.graphics = VmTemplate::defaults(o.os).graphics;
        const ArgsFile windows = VmTemplate::build(o);
        QVERIFY(windows.toText().contains("-device virtio-vga\n"));
        o.os = VmTemplate::Os::Other;
        o.graphics = VmTemplate::defaults(o.os).graphics;
        const ArgsFile other = VmTemplate::build(o);
        ArgsFile own = linuxVm;
        VmConfig::setQemuBinary(own, system);

        QVERIFY(QDir().mkpath(dir));
        QVERIFY(QDir().mkpath(QFileInfo(system).path()));
        QVERIFY(QDir().mkpath(QFileInfo(broken).path()));
        script("system/" + Paths::qemuSystemName(),
               "case \"$1 $2\" in\n"
               "'-device virtio-gpu-gl-pci,help')\n"
               "  echo 'virtio-gpu-gl-pci options:'\n"
               "  echo '  addr=<str>             - Slot and optional function number'\n"
               "  echo '  blob=<bool>            - on/off (default: off)'\n"
               "  echo '  hostmem=<size>         -  (default: 0)'\n"
               "  echo '  venus=<bool>           - on/off (default: off)' ;;\n"
               "'-device virtio-vga,help')\n"
               "  echo 'virtio-vga options:'\n"
               "  echo '  edid=<bool>            - on/off (default: on)'\n"
               "  echo '  xres=<uint32>          -  (default: 1280)' ;;\n"
               "'-netdev help')\n"
               "  echo 'Available netdev backend types:'\n"
               "  echo 'user'; echo 'tap' ;;\n"
               "'-object kvm-accel,help')\n"
               "  echo 'kvm-accel options:'\n"
               "  echo '  kernel-irqchip=<on|off|split> - Configure KVM in-kernel irqchip'\n"
               "  echo '  kvm-shadow-mem=<int>   - KVM shadow MMU size' ;;\n"
               "'-object '*)\n"
               "  echo \"qemu: -object $2: Parameter 'qom-type' does not accept value\" >&2\n"
               "  exit 1 ;;\n"
               "*)\n"
               "  echo \"qemu: -device $2: Device not found\" >&2 ;;\n"
               "esac");
        script("broken/" + Paths::qemuSystemName(), "exit 1");
        /* the test's data: no stack built */
        QVERIFY(stack.startsWith(QDir::homePath() + "/.qttest/"));
        QVERIFY(Paths::stackQemu().isEmpty());
        /* the preferences' QEMU, as for the other tests */
        QVERIFY(!VmRunner::needsQemuBuild(linuxVm));
        QVERIFY(!VmRunner::needsQemuBuild(own));

        Paths::setQemuBinary({});
        qputenv("PATH", QFileInfo(system).path().toLocal8Bit());
        QCOMPARE(Paths::qemuBinary(), system);
        /* what the system's QEMU has: it runs them */
        QVERIFY(linuxVm.toText().contains("-netdev user,"));
        QVERIFY(!VmRunner::needsQemuBuild(headless));
        QVERIFY(!VmRunner::needsQemuBuild(windows));
        QVERIFY(!VmRunner::needsQemuBuild(other));
        QVERIFY(!VmRunner::needsQemuBuild(
            withHeadless("-accel kvm,kernel-irqchip=split\n"
                         "-device virtio-gpu-gl-pci,id=gpu,bus=pcie.0,addr=02.0,blob=on\n"
                         "-global virtio-gpu-gl-pci.hostmem=4G\n"
                         "-global driver=virtio-vga,property=xres,value=1920\n"
                         "-display sdl,gl=on\n")));
        /* what it cannot tell, for QEMU to report */
        QVERIFY(!VmRunner::needsQemuBuild(withHeadless("-global virtio-gpu-base.x-host-vblank=on\n"
                                                       "-accel whpx,kernel-irqchip=off\n")));

        /* what it lacks: vitrine's, to build */
        QVERIFY(VmRunner::needsQemuBuild(linuxVm, &why));
        QCOMPARE(why, "This VM uses drm_native_context, x-host-vblank, x-vblank-lead, "
                      "x-vblank-lead-auto, honor-guest-pat, which the system's QEMU lacks: "
                      "build Vitrine's QEMU with File > Build QEMU, or choose another QEMU in "
                      "the preferences");
        QVERIFY(VmRunner::needsQemuBuild(withHeadless("-device virtio-vga,x-host-vblank=on\n")));
        QVERIFY(VmRunner::needsQemuBuild(withHeadless("-accel kvm,honor-guest-pat=on\n")));
        QVERIFY(VmRunner::needsQemuBuild(
            withHeadless("-global virtio-gpu-gl-pci.x-vblank-lead=3000\n"), &why));
        QVERIFY(why.startsWith("This VM uses x-vblank-lead, which"));
        QVERIFY(VmRunner::needsQemuBuild(withHeadless(
            "-global driver=virtio-gpu-gl-pci,property=drm_native_context,value=on\n")));
        /* NAT through passt, which new VMs take as vitrine's QEMU has it */
        QVERIFY(VmRunner::needsQemuBuild(withHeadless("-nic none\n-netdev passt,id=net0\n"),
                                         &why));
        QVERIFY(why.startsWith("This VM uses passt, which"));
        QVERIFY(VmRunner::needsQemuBuild(withHeadless("-nic passt,model=virtio-net-pci\n")));
        QVERIFY(!VmRunner::needsQemuBuild(withHeadless("-nic user,model=virtio-net-pci\n")));
        /* a bare key is on */
        QVERIFY(VmRunner::needsQemuBuild(
            withHeadless("-device virtio-gpu-gl-pci,blob,drm_native_context\n"), &why));
        QVERIFY(why.startsWith("This VM uses drm_native_context, which"));
        /* a card it lacks altogether */
        QVERIFY(VmRunner::needsQemuBuild(withHeadless("-device virtio-vga-gl\n"), &why));
        QVERIFY(why.startsWith("This VM uses virtio-vga-gl, which"));
        /* a QEMU of the VM's own, or of the preferences: as the user chose */
        QVERIFY(!VmRunner::needsQemuBuild(own));
        Paths::setQemuBinary(system);
        QVERIFY(!VmRunner::needsQemuBuild(linuxVm));
        Paths::setQemuBinary({});

        /* refused before anything runs */
        {
            VmRunner runner(id, dir);
            QSignalSpy failed(&runner, &VmRunner::failed);
            QSignalSpy states(&runner, &VmRunner::stateChanged);

            runner.start(linuxVm);
            QCOMPARE(failed.size(), 1);
            QVERIFY(failed[0][0].toString().contains("drm_native_context"));
            QVERIFY(failed[0][0].toString().contains("File > Build QEMU"));
            QCOMPARE(runner.state(), VmRunner::State::Stopped);
            QVERIFY(states.isEmpty());
            /* nothing ran, nor was written */
            QVERIFY(!QFileInfo::exists(runner.logPath()));
            QCOMPARE(qemuPid(), 0);
            /* the arguments of the run refused, for the window to offer the build */
            QVERIFY(VmRunner::needsQemuBuild(runner.runArgs()));
        }

        /* a system's QEMU that does not answer: QEMU says what is wrong */
        qputenv("PATH", QFileInfo(broken).path().toLocal8Bit());
        QCOMPARE(Paths::qemuBinary(), broken);
        QVERIFY(!VmRunner::needsQemuBuild(linuxVm));

        /* none: vitrine's, to build, whatever the VM */
        qputenv("PATH", tmp.filePath("nowhere").toLocal8Bit());
        QVERIFY(Paths::qemuBinary().isEmpty());
        QVERIFY(VmRunner::needsQemuBuild(headless, &why));
        QVERIFY(why.contains(Paths::qemuSystemName() + " is not in PATH"));
        QVERIFY(why.contains("File > Build QEMU"));
        QVERIFY(VmRunner::needsQemuBuild(windows));
        QVERIFY(!VmRunner::needsQemuBuild(own));

        /* built: the VMs run with it */
        const QString binary = stack + "/0123456789abcdef/bin/" + Paths::qemuSystemName();
        QVERIFY(QDir().mkpath(QFileInfo(binary).path()));
        QFile built(binary);
        QVERIFY(built.open(QIODevice::WriteOnly) && built.write("#!/bin/sh\n") > 0);
        built.close();
        QVERIFY(built.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QVERIFY(QFile::link("0123456789abcdef", stack + "/current"));
        QVERIFY(!Paths::stackQemu().isEmpty());
        QVERIFY(!VmRunner::needsQemuBuild(headless));
        QVERIFY(!VmRunner::needsQemuBuild(linuxVm));
        QVERIFY(!VmRunner::needsQemuBuild(own));
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

    /*
     * A QEMU that takes neither QMP quit nor SIGTERM: never killed by the
     * runner (a SIGKILL loses its disks' last writes), said to the user,
     * killed only when asked.  A shell stands in, its monitor answering
     * every command, in a process group of its own (its sleep goes with it).
     */
    void forceOffNeverKills()
    {
        const QStringList command = VmRunner(id, tmp.path()).commandLine({});
        FakeMonitor monitor(runDir + "/qmp.sock");
        QProcess qemu;
        qemu.setChildProcessModifier([]() { setpgid(0, 0); });
        QVERIFY(startStandIn(qemu, "/bin/sh",
                             {"-c", "trap '' TERM; while :; do sleep 0.1; done", "sh", "-qmp",
                              command[command.size() - 3]}));
        const auto end = qScopeGuard([&qemu]() { stopGroup(qemu); });
        /* processId() is 0 once it ended */
        const qint64 qemuPid = qemu.processId();
        QFile pid(runDir + "/qemu.pid");
        QVERIFY(pid.open(QIODevice::WriteOnly));
        pid.write(QByteArray::number(qemuPid) + '\n');
        pid.close();

        VmRunner runner(id, tmp.path());
        QSignalSpy notResponding(&runner, &VmRunner::notResponding), failed(&runner, &VmRunner::failed);
        runner.setQuitTimeout(200);
        runner.attach(ArgsFile::parse(kHeadless));
        QTRY_COMPARE(runner.state(), VmRunner::State::Running);
        runner.forceOff();
        QCOMPARE(runner.state(), VmRunner::State::Stopping);
        /* quit, then SIGTERM, then the user is told: nothing more */
        QTRY_COMPARE(notResponding.size(), 1);
        QTest::qWait(1000);
        QCOMPARE(notResponding.size(), 1);
        QVERIFY(!gone(qemuPid));
        QCOMPARE(runner.state(), VmRunner::State::Stopping);
        QVERIFY(QFileInfo::exists(runDir + "/qemu.pid"));
        /* Force Off again, the user having waited: asked again */
        runner.forceOff();
        QCOMPARE(notResponding.size(), 2);
        QVERIFY(!gone(qemuPid));

        /* the user's choice */
        runner.killQemu();
        QTRY_VERIFY(gone(qemuPid));
        monitor.close();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
        QCOMPARE(failed.size(), 0);
        QVERIFY(read(runner.logPath()).contains("vitrine: QEMU killed (SIGKILL) at the user's request"));
    }

    /*
     * Its monitor closed and it runs on (stuck on its way out, or writing a
     * large disk): not Stopped while it runs, not killed, and not said
     * unless the user forces it off - then SIGTERM, then the user decides
     */
    void exitingButRunning()
    {
        const QStringList command = VmRunner(id, tmp.path()).commandLine({});
        FakeMonitor monitor(runDir + "/qmp.sock");
        QProcess qemu;
        qemu.setChildProcessModifier([]() { setpgid(0, 0); });
        QVERIFY(startStandIn(qemu, "/bin/sh",
                             {"-c", "trap '' TERM; while :; do sleep 0.1; done", "sh", "-qmp",
                              command[command.size() - 3]}));
        const auto end = qScopeGuard([&qemu]() { stopGroup(qemu); });
        /* processId() is 0 once it ended */
        const qint64 qemuPid = qemu.processId();
        QFile pid(runDir + "/qemu.pid");
        QVERIFY(pid.open(QIODevice::WriteOnly));
        pid.write(QByteArray::number(qemuPid) + '\n');
        pid.close();

        VmRunner runner(id, tmp.path());
        QSignalSpy notResponding(&runner, &VmRunner::notResponding);
        runner.setQuitTimeout(200);
        runner.attach(ArgsFile::parse(kHeadless));
        QTRY_COMPARE(runner.state(), VmRunner::State::Running);
        monitor.close();
        QTRY_COMPARE(runner.state(), VmRunner::State::Stopping);
        QTest::qWait(1000);
        QCOMPARE(notResponding.size(), 0);
        QCOMPARE(runner.state(), VmRunner::State::Stopping);
        QVERIFY(!gone(qemuPid));
        /* forced off: SIGTERM (ignored here), then the user is told */
        runner.forceOff();
        QTRY_COMPARE(notResponding.size(), 1);
        QVERIFY(!gone(qemuPid));
        /* it ends at last: stopped then */
        stopGroup(qemu);
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
        QCOMPARE(notResponding.size(), 1);
    }

    /* The QEMU of a start is the one it began with, even if the preferences
       or `current` change while virtiofsd starts */
    void oneQemuPerStart()
    {
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);
        const QByteArray go = tmp.filePath("go").toUtf8();
        const QByteArray ran = tmp.filePath("ran").toUtf8();
        const QString first = script("qemu-first", "echo first > " + ran);
        const QString second = script("qemu-second", "echo second > " + ran);

        QFile::remove(QString::fromUtf8(go));
        QFile::remove(QString::fromUtf8(ran));
        /* opens its "socket" when told to */
        Paths::setVirtiofsd(script("waiting-virtiofsd",
                                   "while [ ! -e " + go + " ]; do sleep 0.05; done\n"
                                   ": >\"${1#--socket-path=}\"\n"
                                   "while :; do sleep 0.1; done"));
        Paths::setQemuBinary(first);
        runner.start(ArgsFile::parse("-machine q35,memory-backend=mem\n"
                                     "-object memory-backend-memfd,id=mem,size=128M\n"
                                     "#share tag=t,path=" + tmp.path() + "\n"));
        QCOMPARE(runner.state(), VmRunner::State::Starting);
        Paths::setQemuBinary(second);
        QFile f(QString::fromUtf8(go));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        /* the stand-in ends at once: QEMU stopped */
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QCOMPARE(read(QString::fromUtf8(ran)).trimmed(), "first");
        const QString log = read(runner.logPath());
        QVERIFY2(log.section('\n', 0, 0).contains(first), qPrintable(log));
        QVERIFY(!log.contains(second));
        Paths::setQemuBinary(testQemu());
        Paths::setVirtiofsd({});
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
