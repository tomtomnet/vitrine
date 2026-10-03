// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmrunner.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <csignal>

#include "core/firmwarefiles.h"
#include "core/guestagent.h"
#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

static const int kPollMs = 50;
/* virtiofsd opening its socket */
static const int kHelperTimeoutMs = 10000;
/* QEMU quitting before SIGTERM, then before SIGKILL */
static const int kQuitTimeoutMs = 5000;
/* QEMU exiting once its QMP socket closed */
static const int kExitTimeoutMs = 10000;

/* A live process, not a zombie */
static bool alive(qint64 pid)
{
    QFile stat(QString("/proc/%1/stat").arg(pid));

    if (pid <= 0 || !stat.open(QIODevice::ReadOnly)) {
        return false;
    }
    /* pid (comm) state ...: comm may hold spaces and parentheses */
    const QByteArray line = stat.readAll();
    const qsizetype paren = line.lastIndexOf(')');
    if (paren < 0 || paren + 2 >= line.size()) {
        return false;
    }
    return line[paren + 2] != 'Z' && line[paren + 2] != 'X';
}

static QStringList cmdline(qint64 pid)
{
    QFile f(QString("/proc/%1/cmdline").arg(pid));
    QStringList args;

    if (!f.open(QIODevice::ReadOnly)) {
        return args;
    }
    for (const QByteArray &arg : f.readAll().split('\0')) {
        args << QString::fromLocal8Bit(arg);
    }
    return args;
}

/* Signals @pid only if it is the process whose arguments include @marker,
   not one that got its pid since */
static void signalIfOurs(qint64 pid, const QString &marker, int sig)
{
    if (alive(pid) && cmdline(pid).contains(marker)) {
        ::kill(pid_t(pid), sig);
    }
}

/* The port of the guest agent the manager adds, for mounting the shares */
static const char kAgentPort[] = "qgm-ga-port";

/*
 * sh -c SCRIPT TAG DIR OPTIONS TAG-AS-IN-MOUNTINFO, as root in the guest:
 * mounts a shared folder where asked, unless the guest has mounted it
 * already, as systemd does at boot from the fstab.extra credential.  Ends
 * with kConfined when SELinux keeps qemu-ga from mounting, as on Fedora:
 * its commands may neither run mount nor make folders in /mnt.
 */
static const int kConfined = 77;     // the script's exit 77
static const char kMount[] =
    "umask 022\n"
    "while read -r line; do\n"
    "    case $line in *\" - virtiofs $3 \"*) exit 0 ;; esac\n"
    "done </proc/self/mountinfo\n"
    "mkdir -p \"$1\" && mount -t virtiofs -o \"$2\" \"$0\" \"$1\" && exit 0\n"
    "grep -qs qemu_ga_t /proc/self/attr/current && exit 77\n"
    "exit 1\n";

/* The shares the guest mounts at start */
static QList<VmConfig::Share> sharesToMount(const ArgsFile &args)
{
    QList<VmConfig::Share> list;

    for (const VmConfig::Share &s : VmConfig::shares(args)) {
        if (!s.mount.isEmpty()) {
            list << s;
        }
    }
    return list;
}

/* Unless the VM has an agent port of its own, which the manager cannot share */
static bool addsAgent(const ArgsFile &args)
{
    return !sharesToMount(args).isEmpty() && !args.toText().contains("org.qemu.guest_agent.0");
}

/* As fstab and /proc/self/mountinfo write spaces and the like: \040 */
static QString mountEscape(QString s)
{
    return s.replace('\\', "\\134").replace(' ', "\\040").replace('\t', "\\011")
        .replace('\n', "\\012");
}

static QString fstabLine(const VmConfig::Share &s)
{
    return QString("%1 %2 virtiofs %3 0 0")
        .arg(mountEscape(s.tag), mountEscape(s.mount), s.readonly ? "ro,nofail" : "nofail");
}

/* The QEMU of the VM's #qemu directive, else the one in the preferences */
static QString qemuFor(const ArgsFile &args)
{
    const QString own = VmConfig::qemuBinary(args);
    return own.isEmpty() ? Paths::qemuBinary() : own;
}

/*
 * The mounts for systemd in the guest, 254 and later, which reads them from
 * SMBIOS at boot and mounts them as if they were in /etc/fstab: they need
 * no guest agent, and SELinux lets systemd mount.  Only targets with
 * -smbios get them; a QEMU named otherwise, like qemu-kvm, is taken for
 * one of the host's architecture.
 */
static QString fstabExtra(const ArgsFile &args)
{
    static const QRegularExpression target("^qemu-system-([a-z0-9_]+)");
    static const QStringList smbios{"x86_64", "i386",    "aarch64",    "arm",
                                    "riscv64", "riscv32", "loongarch64"};
    const QRegularExpressionMatch m = target.match(QFileInfo(qemuFor(args)).fileName());
    QString lines;

    /* a credential of its own; isapc refuses type 11, and has no PCI for virtiofs */
    if (!smbios.contains(m.hasMatch() ? m.captured(1) : Paths::hostArch()) ||
        args.toText().contains("fstab.extra") || VmConfig::machineType(args) == "isapc") {
        return {};
    }
    for (const VmConfig::Share &s : sharesToMount(args)) {
        lines += fstabLine(s) + '\n';
    }
    return lines;
}

static QString shellQuote(const QStringList &args)
{
    static const QRegularExpression plain("^[A-Za-z0-9_@%+=:,./-]+$");
    QStringList out;

    for (const QString &arg : args) {
        out << (plain.match(arg).hasMatch()
                    ? arg : "'" + QString(arg).replace("'", "'\\''") + "'");
    }
    return out.join(' ');
}

struct VmRunner::Private
{
    enum class Phase {
        Idle,       // stopped, or running with QMP ready
        Helpers,    // waiting for the sockets of virtiofsd
        Qemu,       // waiting for QMP
        Attach,     // connecting to a QEMU started earlier
        Exiting,    // QMP closed, waiting for QEMU to end
    };
    struct Helper {
        qint64 pid;
        QString marker;
    };

    VmRunner *q;
    QString id;
    QString dir;
    State state = State::Stopped;
    Phase phase = Phase::Idle;
    QString error;
    QmpClient *qmp;
    QTimer *poll;
    QTimer *killTimer;
    QElapsedTimer clock;
    ArgsFile args;              // of the run being started
    qint64 pid = 0;             // QEMU
    QList<Helper> helpers;      // virtiofsd started for this run
    GuestAgent *agent = nullptr;    // mounting the shares
    bool connecting = false;
    bool stopRequested = false; // the end of the run is no failure
    int killStep = 0;

    QString runDir() const;
    QString qmpPath() const { return runDir() + "/qmp.sock"; }
    QString pidPath() const { return runDir() + "/qemu.pid"; }
    QString sharePath(qsizetype i) const { return runDir() + QString("/fs%1.sock").arg(i); }
    QString agentPath() const { return runDir() + "/qga.sock"; }
    QString qmpArg() const;
    QString logPath() const { return dir + "/qemu.log"; }
    QString logTail(bool qemuErrors = true) const;
    qint64 runningPid() const;
    void removeRuntimeFiles() const;
    bool launch(const QString &program, const QStringList &arguments, qint64 *pid,
                QString *error) const;

    void setState(State s);
    void fail(const QString &message);
    void cleanup();
    void launchQemu();
    void tick();
    void escalate();
    void qmpReady();
    void qmpFailed();
    void qmpClosed();
    void exited();
    void event(const QString &name, const QJsonObject &data);
    void mountShares();
    void mountNext(QList<VmConfig::Share> shares, QStringList mounted, QStringList problems);
    void endMounts(const QStringList &mounted, const QStringList &problems);
    QmpClient::Callback reportErrors(const QString &command);
};

QString VmRunner::Private::runDir() const
{
    /* sun_path holds 107 bytes: leave room for the socket names */
    if ((Paths::runtimeDir() + '/' + id).toLocal8Bit().size() > 88) {
        return Paths::vmRuntimeDir(QString::fromLatin1(
            QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex().left(16)));
    }
    return Paths::vmRuntimeDir(id);
}

QString VmRunner::Private::qmpArg() const
{
    return QString("unix:%1,server=on,wait=off").arg(OptionValue::escape(qmpPath()));
}

/*
 * The last lines of the log: with @qemuErrors, QEMU's error messages if it
 * printed some ("qemu-system-x86_64: ...", "qemu: ..."), else its other
 * lines rather than those of virtiofsd
 */
QString VmRunner::Private::logTail(bool qemuErrors) const
{
    static const QRegularExpression qemuMessage("^qemu[\\w.-]*:");
    QFile f(logPath());
    QStringList lines, errors, own;

    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    if (f.size() > 16384) {
        f.seek(f.size() - 16384);
    }
    const QString prefix = QFileInfo(qemuFor(args)).fileName() + ':';
    for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
        const QString t = line.trimmed();
        if (t.isEmpty() || t.startsWith("qemu-gui-manager:")) {
            continue;
        }
        lines << t;
        if (t.startsWith(prefix) || qemuMessage.match(t).hasMatch()) {
            errors << t;
        }
        /* [2026-09-25T06:16:39Z WARN  virtiofsd::limits] ... */
        if (!(t.startsWith('[') && t.contains(" virtiofsd"))) {
            own << t;
        }
    }
    const QStringList &pick = !qemuErrors ? lines
                              : !errors.isEmpty() ? errors
                              : !own.isEmpty() ? own : lines;
    return pick.mid(qMax<qsizetype>(0, pick.size() - 5)).join('\n');
}

/* The QEMU of this VM from an earlier run, if it still runs */
qint64 VmRunner::Private::runningPid() const
{
    QFile f(pidPath());

    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const qint64 found = f.readAll().trimmed().toLongLong();
    return alive(found) && cmdline(found).contains(qmpArg()) ? found : 0;
}

void VmRunner::Private::removeRuntimeFiles() const
{
    QDir rt(runDir());

    for (const QString &name : rt.entryList({"qmp.sock", "qemu.pid", "fs*.sock*", "qga.sock"},
                                            QDir::AllEntries | QDir::System |
                                                QDir::Hidden)) {
        rt.remove(name);
    }
    QDir().rmdir(rt.path());
}

bool VmRunner::Private::launch(const QString &program, const QStringList &arguments,
                               qint64 *pid, QString *error) const
{
    QProcess p;

    p.setProgram(program);
    p.setArguments(arguments);
    p.setWorkingDirectory(dir);
    p.setStandardInputFile(QProcess::nullDevice());
    p.setStandardOutputFile(logPath(), QIODevice::Append);
    p.setStandardErrorFile(logPath(), QIODevice::Append);
    if (!p.startDetached(pid)) {
        *error = VmRunner::tr("Cannot run %1: %2").arg(program, p.errorString());
        return false;
    }
    return true;
}

void VmRunner::Private::setState(State s)
{
    if (state != s) {
        state = s;
        emit q->stateChanged(s);
    }
}

void VmRunner::Private::fail(const QString &message)
{
    cleanup();
    error = message;
    setState(State::Stopped);
    emit q->failed(message);
}

/* The run is over: QEMU is gone, or never started */
void VmRunner::Private::cleanup()
{
    poll->stop();
    killTimer->stop();
    phase = Phase::Idle;
    connecting = false;
    qmp->disconnectFromSocket();
    for (const Helper &h : std::as_const(helpers)) {
        signalIfOurs(h.pid, h.marker, SIGTERM);
    }
    helpers.clear();
    if (agent) {
        agent->deleteLater();
        agent = nullptr;
    }
    pid = 0;
    removeRuntimeFiles();
}

void VmRunner::Private::launchQemu()
{
    const QStringList command = q->commandLine(args);
    QString launchError;

    if (!launch(command.first(), command.mid(1), &pid, &launchError)) {
        fail(launchError);
        return;
    }
    phase = Phase::Qemu;
    poll->start();
}

void VmRunner::Private::tick()
{
    switch (phase) {
    case Phase::Helpers: {
        bool listening = true;

        for (qsizetype i = 0; i < helpers.size(); i++) {
            if (!alive(helpers[i].pid)) {
                fail(VmRunner::tr("virtiofsd stopped:\n%1").arg(logTail(false)));
                return;
            }
            listening &= QFileInfo::exists(sharePath(i));
        }
        if (listening) {
            poll->stop();
            launchQemu();
        } else if (clock.elapsed() > kHelperTimeoutMs) {
            fail(VmRunner::tr("virtiofsd did not open its socket:\n%1").arg(logTail(false)));
        }
        break;
    }
    case Phase::Qemu: {
        QFile pidFile(pidPath());

        /* the pid file tells the right process if QEMU daemonizes */
        if (pidFile.open(QIODevice::ReadOnly)) {
            const qint64 filePid = pidFile.readAll().trimmed().toLongLong();
            if (filePid > 0) {
                pid = filePid;
            }
        }
        if (!alive(pid)) {
            if (stopRequested) {
                cleanup();
                setState(State::Stopped);
            } else {
                const QString tail = logTail();
                fail(tail.isEmpty() ? VmRunner::tr("QEMU stopped") : tail);
            }
            return;
        }
        if (!connecting && QFileInfo::exists(qmpPath())) {
            connecting = true;
            qmp->connectToSocket(qmpPath());
        }
        break;
    }
    case Phase::Exiting:
        if (!alive(pid) || clock.elapsed() > kExitTimeoutMs) {
            exited();
        }
        break;
    default:
        poll->stop();
        break;
    }
}

/* QEMU did not quit: SIGTERM, then SIGKILL */
void VmRunner::Private::escalate()
{
    if (!alive(pid)) {
        return;
    }
    signalIfOurs(pid, qmpArg(), killStep == 0 ? SIGTERM : SIGKILL);
    if (killStep++ == 0) {
        killTimer->start(kQuitTimeoutMs);
    }
}

void VmRunner::Private::qmpReady()
{
    poll->stop();
    phase = Phase::Idle;
    connecting = false;
    qmp->execute("query-status", {}, [this](const QJsonValue &result, const QString &err) {
        if (err.isEmpty() && state != State::Stopping) {
            setState(result["running"].toBool() ? State::Running : State::Paused);
        }
    });
}

void VmRunner::Private::qmpFailed()
{
    connecting = false;
    if (phase != Phase::Attach) {
        /* while starting, tick() tries again as long as QEMU runs */
        return;
    }
    phase = Phase::Idle;
    if (alive(pid)) {
        error = VmRunner::tr("QEMU runs (process %1) but does not answer on %2")
                    .arg(pid).arg(qmpPath());
        emit q->failed(error);
    } else {
        pid = 0;
        removeRuntimeFiles();
    }
}

void VmRunner::Private::qmpClosed()
{
    killTimer->stop();
    phase = Phase::Exiting;
    setState(State::Stopping);
    clock.restart();
    poll->start();
}

void VmRunner::Private::exited()
{
    const bool expected = stopRequested;
    const QString tail = logTail();

    cleanup();
    if (expected) {
        setState(State::Stopped);
        return;
    }
    error = tail.isEmpty() ? VmRunner::tr("QEMU stopped unexpectedly")
                           : VmRunner::tr("QEMU stopped unexpectedly:\n%1").arg(tail);
    setState(State::Stopped);
    emit q->failed(error);
}

void VmRunner::Private::event(const QString &name, const QJsonObject &data)
{
    /* qemu-ga opened its port: the guest has booted */
    if (name == "VSERPORT_CHANGE") {
        if (data["id"].toString() == kAgentPort && data["open"].toBool()) {
            mountShares();
        }
        return;
    }
    if (name == "SHUTDOWN") {
        stopRequested = true;
        setState(State::Stopping);
    } else if (state == State::Stopping) {
        return;
    } else if (name == "STOP" || name == "SUSPEND") {
        setState(State::Paused);
    } else if (name == "RESUME" || name == "WAKEUP") {
        setState(State::Running);
    }
}

void VmRunner::Private::mountShares()
{
    const QList<VmConfig::Share> shares = sharesToMount(args);

    if (!addsAgent(args)) {
        return;
    }
    if (agent) {
        agent->deleteLater();
    }
    agent = new GuestAgent(q);
    QObject::connect(agent, &GuestAgent::ready, q, [this, shares]() {
        mountNext(shares, {}, {});
    });
    QObject::connect(agent, &GuestAgent::failed, q, [this](const QString &error) {
        endMounts({}, {error});
    });
    agent->connectToSocket(agentPath());
}

void VmRunner::Private::mountNext(QList<VmConfig::Share> shares, QStringList mounted,
                                  QStringList problems)
{
    if (shares.isEmpty()) {
        endMounts(mounted, problems);
        return;
    }
    const VmConfig::Share share = shares.takeFirst();
    agent->exec("/bin/sh",
                {"-c", kMount, share.tag, share.mount, share.readonly ? "ro" : "rw",
                 mountEscape(share.tag)},
                [=, this](const GuestAgent::ExecResult &r) mutable {
        if (!r.error.isEmpty()) {
            problems << QString("%1: %2").arg(share.mount, r.error);
        } else if (r.exitCode == kConfined) {
            problems << QString("%1: %2").arg(
                share.mount,
                VmRunner::tr("the guest did not mount it at boot (systemd 254 and later do), "
                             "and SELinux keeps the guest agent from mounting it. Mount it "
                             "with this line in the guest's /etc/fstab:\n%1")
                    .arg(fstabLine(share)));
        } else if (r.exitCode != 0) {
            problems << QString("%1: %2").arg(
                share.mount, r.err.trimmed().isEmpty()
                                 ? VmRunner::tr("mount ended with %1").arg(r.exitCode)
                                 : r.err.trimmed());
        } else {
            mounted << share.mount;
        }
        mountNext(shares, mounted, problems);
    });
}

void VmRunner::Private::endMounts(const QStringList &mounted, const QStringList &problems)
{
    if (agent) {
        agent->disconnectFromSocket();
        agent->deleteLater();
        agent = nullptr;
    }
    emit q->sharesMounted(mounted, problems);
}

/* Errors of QEMU, not those of the connection closing with QEMU */
QmpClient::Callback VmRunner::Private::reportErrors(const QString &command)
{
    return [this, command](const QJsonValue &, const QString &err) {
        if (!err.isEmpty() && qmp->isReady()) {
            emit q->failed(QString("%1: %2").arg(command, err));
        }
    };
}

VmRunner::VmRunner(const QString &id, const QString &dir, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->q = this;
    d->id = id;
    d->dir = dir;
    d->qmp = new QmpClient(this);
    d->poll = new QTimer(this);
    d->poll->setInterval(kPollMs);
    d->killTimer = new QTimer(this);
    d->killTimer->setSingleShot(true);
    connect(d->poll, &QTimer::timeout, this, [this]() { d->tick(); });
    connect(d->killTimer, &QTimer::timeout, this, [this]() { d->escalate(); });
    connect(d->qmp, &QmpClient::ready, this, [this]() { d->qmpReady(); });
    connect(d->qmp, &QmpClient::connectionFailed, this, [this]() { d->qmpFailed(); });
    connect(d->qmp, &QmpClient::disconnected, this, [this]() { d->qmpClosed(); });
    connect(d->qmp, &QmpClient::qmpEvent, this,
            [this](const QString &name, const QJsonObject &data) { d->event(name, data); });
}

VmRunner::~VmRunner()
{
    /* QEMU runs on without the manager, but not virtiofsd waiting for a
       QEMU that will never come */
    if (d->phase == Private::Phase::Helpers) {
        d->cleanup();
    }
    delete d->qmp;
    delete d;
}

VmRunner::State VmRunner::state() const
{
    return d->state;
}

bool VmRunner::isActive() const
{
    return d->state != State::Stopped;
}

QString VmRunner::errorString() const
{
    return d->error;
}

QString VmRunner::logPath() const
{
    return d->logPath();
}

QStringList VmRunner::commandLine(const ArgsFile &args) const
{
    const QList<VmConfig::Share> shares = VmConfig::shares(args);
    QStringList command{qemuFor(args)};

    command += args.argv();
    for (qsizetype i = 0; i < shares.size(); i++) {
        command << "-chardev"
                << QString("socket,id=qgm-fs%1,path=%2")
                       .arg(QString::number(i), OptionValue::escape(d->sharePath(i)))
                << "-device"
                << QString("vhost-user-fs-pci,queue-size=1024,chardev=qgm-fs%1,tag=%2")
                       .arg(QString::number(i), OptionValue::escape(shares[i].tag));
    }
    /* systemd in the guest mounts the shares at boot, else qemu-ga does */
    const QString fstab = fstabExtra(args);
    if (!fstab.isEmpty()) {
        command << "-smbios"
                << "type=11,value=io.systemd.credential.binary:fstab.extra=" +
                       QString::fromLatin1(fstab.toUtf8().toBase64());
    }
    if (addsAgent(args)) {
        command << "-chardev"
                << QString("socket,id=qgm-ga,path=%1,server=on,wait=off")
                       .arg(OptionValue::escape(d->agentPath()))
                << "-device" << "virtio-serial-pci,id=qgm-serial"
                << "-device"
                << QString("virtserialport,bus=qgm-serial.0,chardev=qgm-ga,"
                           "name=org.qemu.guest_agent.0,id=%1").arg(kAgentPort);
    }
    command << "-qmp" << d->qmpArg() << "-pidfile" << d->pidPath();
    return command;
}

void VmRunner::start(const ArgsFile &args)
{
    const QString qemu = qemuFor(args);
    const QList<VmConfig::Share> shares = VmConfig::shares(args);
    QString virtiofsd;

    if (isActive() || d->phase != Private::Phase::Idle) {
        return;
    }
    /* left running by an earlier run of the manager */
    if (d->runningPid() > 0) {
        attach();
        return;
    }

    d->error.clear();
    d->stopRequested = false;
    d->killStep = 0;
    d->args = args;
    if (qemu.isEmpty() || !QFileInfo(qemu).isExecutable()) {
        d->fail(VmConfig::qemuBinary(args).isEmpty()
                    ? tr("QEMU was not found: set its path in the preferences")
                    : tr("%1 was not found: it is the QEMU of this VM, from the #qemu line "
                         "of its arguments").arg(qemu));
        return;
    }
    if (!shares.isEmpty()) {
        virtiofsd = Paths::virtiofsd();
        if (!VmConfig::hasSharedMemory(args)) {
            d->fail(tr("Shared folders need the guest memory to be shared with "
                       "virtiofsd: turn it on in the Shared Folders settings"));
            return;
        }
        if (virtiofsd.isEmpty()) {
            d->fail(tr("Shared folders need virtiofsd, which is not installed "
                       "(sudo dnf install virtiofsd)"));
            return;
        }
        for (const VmConfig::Share &s : shares) {
            if (s.tag.isEmpty()) {
                d->fail(tr("The shared folder %1 has no tag").arg(s.path));
                return;
            }
            if (!QFileInfo(s.path).isDir()) {
                d->fail(tr("The shared folder %1 does not exist").arg(s.path));
                return;
            }
        }
    }

    /* firmware copies that were deleted: new ones from the templates they came from */
    QStringList remade;
    for (const FirmwareFiles::File &f : FirmwareFiles::list(args, d->dir)) {
        QString error;
        if (QFileInfo::exists(f.path)) {
            continue;
        }
        remade << (FirmwareFiles::recreate(f, &error)
                       ? tr("%1 was missing: a new copy of %2").arg(f.name(), f.templatePath)
                       : tr("%1 is missing: %2").arg(f.name(), error));
    }

    d->removeRuntimeFiles();
    QFile log(d->logPath());
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        d->fail(tr("Cannot write %1: %2").arg(d->logPath(), log.errorString()));
        return;
    }
    log.write(QString("qemu-gui-manager: %1 %2\n")
                  .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                       shellQuote(commandLine(args)))
                  .toUtf8());
    for (const QString &line : std::as_const(remade)) {
        log.write(("qemu-gui-manager: " + line + '\n').toUtf8());
    }
    log.close();

    d->setState(State::Starting);
    d->clock.start();
    for (qsizetype i = 0; i < shares.size(); i++) {
        QStringList arguments{"--socket-path=" + d->sharePath(i),
                              "--shared-dir=" + shares[i].path,
                              "--cache=" + shares[i].cache};
        qint64 pid = 0;
        QString launchError;

        if (shares[i].readonly) {
            arguments << "--readonly";
        }
        if (!d->launch(virtiofsd, arguments, &pid, &launchError)) {
            d->fail(launchError);
            return;
        }
        d->helpers << Private::Helper{pid, arguments.first()};
    }
    if (shares.isEmpty()) {
        d->launchQemu();
    } else {
        d->phase = Private::Phase::Helpers;
        d->poll->start();
    }
}

void VmRunner::attach(const ArgsFile &args)
{
    if (isActive() || d->phase != Private::Phase::Idle) {
        return;
    }
    d->args = args;
    const qint64 pid = d->runningPid();
    if (pid <= 0) {
        d->removeRuntimeFiles();
        return;
    }
    d->pid = pid;
    d->error.clear();
    d->stopRequested = false;
    d->killStep = 0;
    d->phase = Private::Phase::Attach;
    d->connecting = true;
    d->qmp->connectToSocket(d->qmpPath());
}

void VmRunner::pause()
{
    if (d->qmp->isReady()) {
        d->qmp->execute("stop", {}, d->reportErrors("stop"));
    }
}

void VmRunner::resume()
{
    if (d->qmp->isReady()) {
        d->qmp->execute("cont", {}, d->reportErrors("cont"));
    }
}

void VmRunner::powerdown()
{
    if (d->qmp->isReady()) {
        d->qmp->execute("system_powerdown", {}, d->reportErrors("system_powerdown"));
    }
}

void VmRunner::reset()
{
    if (d->qmp->isReady()) {
        d->qmp->execute("system_reset", {}, d->reportErrors("system_reset"));
    }
}

void VmRunner::forceOff()
{
    if (d->phase == Private::Phase::Helpers) {
        /* QEMU is not running yet */
        d->stopRequested = true;
        d->cleanup();
        d->setState(State::Stopped);
        return;
    }
    if (!isActive() || d->phase == Private::Phase::Exiting) {
        return;
    }
    d->stopRequested = true;
    d->killStep = 0;
    if (d->qmp->isReady()) {
        d->qmp->execute("quit");
        d->setState(State::Stopping);
        d->killTimer->start(kQuitTimeoutMs);
    } else {
        /* still starting: no QMP to ask QEMU to quit */
        d->escalate();
    }
}

qint64 VmRunner::pid() const
{
    return isActive() ? d->pid : 0;
}

QmpClient *VmRunner::qmp() const
{
    return isActive() ? d->qmp : nullptr;
}
