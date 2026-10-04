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
#include <QSaveFile>
#include <QTimer>

#include <csignal>

#include "core/firmwarefiles.h"
#include "core/guestagent.h"
#include "core/guesttools.h"
#include "core/hostkvm.h"
#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/qmpclient.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

static const int kPollMs = 50;
/* virtiofsd opening its socket */
static const int kHelperTimeoutMs = 10000;
/*
 * QEMU quitting before SIGTERM, then ending after it before the user is
 * asked; twice that for QEMU exiting once its QMP socket closed (its disks
 * closed already)
 */
static const int kQuitTimeoutMs = 5000;

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
static const char kAgentPort[] = "vitrine-ga-port";

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
static QString fstabExtra(const ArgsFile &args, const QString &qemu)
{
    static const QRegularExpression target("^qemu-system-([a-z0-9_]+)");
    static const QStringList smbios{"x86_64", "i386",    "aarch64",    "arm",
                                    "riscv64", "riscv32", "loongarch64"};
    const QRegularExpressionMatch m = target.match(QFileInfo(qemu).fileName());
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
    /*
     * The QEMU of the run and its command line, made once at start: the
     * preferences or `current` may change while virtiofsd starts, and the
     * log must tell what runs
     */
    QString qemu;
    QStringList command;
    bool embedded = false;      // its screen in vitrine's window: displaySocket()
    qint64 pid = 0;             // QEMU
    QList<Helper> helpers;      // virtiofsd started for this run
    GuestAgent *agent = nullptr;    // mounting the shares
    std::function<bool()> shutdownHandler;
    bool connecting = false;
    bool suspended = false;     // Paused by the guest's own suspend (S3)
    bool stopRequested = false; // the end of the run is no failure
    bool forceRequested = false; // forceOff() was used in this run
    /* forceOff()'s steps: 0 none, 1 SIGTERM sent, 2 notResponding() said */
    int killStep = 0;
    int quitTimeoutMs = kQuitTimeoutMs;

    QString runDir() const;
    QString qmpPath() const { return runDir() + "/qmp.sock"; }
    QString pidPath() const { return runDir() + "/qemu.pid"; }
    QString sharePath(qsizetype i) const { return runDir() + QString("/fs%1.sock").arg(i); }
    QString agentPath() const { return runDir() + "/qga.sock"; }
    QString toolsAgentPath() const { return runDir() + "/agent.sock"; }
    QString displayPath() const { return runDir() + "/display.sock"; }
    /* the run's arguments, kept for the next manager's attach() */
    QString argsPath() const { return runDir() + "/run.args"; }
    QString qmpArg() const;
    QString displayArg() const;
    /* @problems: the cards whose properties could not be read, for the log */
    QStringList commandLine(const ArgsFile &args, const QString &qemu,
                            QStringList *problems = nullptr) const;
    QString logPath() const { return dir + "/qemu.log"; }
    QString logTail(bool qemuErrors = true) const;
    qint64 runningPid() const;
    void removeRuntimeFiles() const;
    bool launch(const QString &program, const QStringList &arguments, qint64 *pid,
                QString *error, const QStringList &environment = {}) const;

    void setState(State s);
    void setSuspended(bool on);
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

/* The display's monitor, which only an embedded screen's run has */
QString VmRunner::Private::displayArg() const
{
    return QString("unix:%1,server=on,wait=off").arg(OptionValue::escape(displayPath()));
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
    const QString prefix = QFileInfo(qemu.isEmpty() ? qemuFor(args) : qemu).fileName() + ':';
    for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
        const QString t = line.trimmed();
        if (t.isEmpty() || t.startsWith("vitrine:")) {
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

    for (const QString &name : rt.entryList({"qmp.sock", "qemu.pid", "fs*.sock*", "qga.sock",
                                             "display.sock", "agent.sock", "run.args"},
                                            QDir::AllEntries | QDir::System |
                                                QDir::Hidden)) {
        rt.remove(name);
    }
    QDir().rmdir(rt.path());
}

bool VmRunner::Private::launch(const QString &program, const QStringList &arguments,
                               qint64 *pid, QString *error,
                               const QStringList &environment) const
{
    QProcess p;

    if (!environment.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &var : environment) {
            const qsizetype eq = var.indexOf('=');
            env.insert(var.left(eq), var.mid(eq + 1));
        }
        p.setProcessEnvironment(env);
    }
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

void VmRunner::Private::setSuspended(bool on)
{
    if (suspended != on) {
        suspended = on;
        emit q->suspendedChanged(on);
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
    suspended = false;
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
    QString launchError;

    if (!launch(command.first(), command.mid(1), &pid, &launchError,
                VmRunner::environment(args))) {
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
        if (!alive(pid)) {
            exited();
        } else if (forceRequested && clock.elapsed() > 2 * quitTimeoutMs && killStep < 2) {
            /*
             * Forced off, its monitor closed, and it runs on: stuck on its
             * way out, or still writing.  Not killed: the user decides.
             * Without Force Off (the guest shut down, say) it is waited
             * for, quietly: QEMU may take long to write a large disk.
             * Never Stopped while it runs: a new start would find its disks
             * locked.
             */
            killTimer->stop();
            killStep = 2;
            emit q->notResponding();
        }
        break;
    default:
        poll->stop();
        break;
    }
}

/*
 * QEMU did not quit: SIGTERM, which it takes as a request to shut down
 * (its disks flushed and closed); still there after that, the user is
 * asked.  Never SIGKILL here: what QEMU had not written yet - a qcow2
 * image's cached metadata among it - would be lost.
 */
void VmRunner::Private::escalate()
{
    if (!alive(pid)) {
        return;
    }
    if (killStep == 0) {
        killStep = 1;
        signalIfOurs(pid, qmpArg(), SIGTERM);
        killTimer->start(quitTimeoutMs);
    } else if (killStep == 1) {
        killStep = 2;
        emit q->notResponding();
    }
}

void VmRunner::Private::qmpReady()
{
    poll->stop();
    phase = Phase::Idle;
    connecting = false;
    qmp->execute("query-status", {}, [this](const QJsonValue &result, const QString &err) {
        if (err.isEmpty() && state != State::Stopping) {
            setSuspended(result["status"].toString() == "suspended");
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
        /* before the state: what follows it reads both */
        setSuspended(name == "SUSPEND");
        setState(State::Paused);
    } else if (name == "RESUME" || name == "WAKEUP") {
        setSuspended(false);
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

bool VmRunner::isSuspended() const
{
    return d->state == State::Paused && d->suspended;
}

QString VmRunner::errorString() const
{
    return d->error;
}

QString VmRunner::logPath() const
{
    return d->logPath();
}

void VmRunner::appendNote(const QString &text) const
{
    QFile log(d->logPath());
    QByteArray lines;

    for (const QString &line : text.split('\n')) {
        lines += ("vitrine: " + line + '\n').toUtf8();
    }
    /* QEMU appends to it at the same time: the lines in one write, at the end */
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)) {
        log.write(lines);
    }
}

QStringList VmRunner::commandLine(const ArgsFile &args) const
{
    /* the QEMU resolved once: `current` may switch to a new build meanwhile */
    return d->commandLine(args, qemuFor(args));
}

/*
 * What @args set that @qemu lacks, where vitrine's QEMU has more than
 * others: the virtio-gpu cards and their properties (native context, the
 * vblank timing), the properties of -global and those of the accelerators
 * (honor-guest-pat), and the network backends (passt, which new VMs take
 * as vitrine's QEMU has it).  Asked from @qemu, a few milliseconds each,
 * and kept; what it does not answer about counts as there, for QEMU to
 * report.
 */
static QStringList lacking(const ArgsFile &args, const QString &qemu)
{
    static const QStringList cards = {
        "virtio-vga-gl", "virtio-gpu-gl-pci", "virtio-gpu-gl", "virtio-gpu-gl-device",
        "virtio-vga",    "virtio-gpu-pci",    "virtio-gpu",    "virtio-gpu-device",
    };
    QStringList out;
    const auto add = [&out](const QString &name) {
        if (!out.contains(name)) {
            out << name;
        }
    };
    /* the keys of @v but @skip that @known lacks; a bare key is KEY=on, noKEY KEY=off */
    const auto check = [&add](const OptionValue &v, const QStringList &known,
                              const QStringList &skip) {
        for (const OptionValue::Item &item : v.items()) {
            if (item.key.isEmpty() || skip.contains(item.key) || known.contains(item.key) ||
                (item.bare && item.key.startsWith("no") && known.contains(item.key.mid(2)))) {
                continue;
            }
            add(item.key);
        }
    };

    for (int i : args.indexesOf("device")) {
        const OptionValue v = args.valueAt(i);
        const QString driver = v.implied().isEmpty() ? v.get("driver") : v.implied();
        QString error;

        if (!cards.contains(driver)) {
            continue;
        }
        const QStringList known = QemuInfo::probeProperties(qemu, driver, &error);
        if (!error.isEmpty()) {
            continue;
        }
        /* QEMU answers nothing for a device it lacks */
        if (known.isEmpty()) {
            add(driver);
            continue;
        }
        /* qdev's own, which are no properties */
        check(v, known, {"driver", "bus", "id"});
    }
    for (int i : args.indexesOf("global")) {
        const OptionValue g = args.valueAt(i);
        QString driver, property, error;

        if (g.has("property")) {
            driver = g.get("driver");
            property = g.get("property");
        } else if (!g.items().isEmpty()) {
            /* DRIVER.PROPERTY=VALUE, split at the first dot as QEMU does */
            driver = g.items().first().key.section('.', 0, 0);
            property = g.items().first().key.section('.', 1);
        }
        if (driver.isEmpty() || property.isEmpty()) {
            continue;
        }
        /* nothing for an abstract type such as virtio-gpu-base: not known */
        const QStringList known = QemuInfo::probeProperties(qemu, driver, &error);
        if (error.isEmpty() && !known.isEmpty() && !known.contains(property)) {
            add(property);
        }
    }
    for (int i : args.indexesOf("accel")) {
        const OptionValue v = args.valueAt(i);
        const QString accel = v.implied().isEmpty() ? v.get("accel") : v.implied();
        QString error;

        if (accel.isEmpty() || v.items().size() < 2) {
            continue;
        }
        const QStringList known = QemuInfo::probeObjectProperties(qemu, accel + "-accel", &error);
        if (error.isEmpty() && !known.isEmpty()) {
            check(v, known, {"accel"});
        }
    }
    for (const char *option : {"netdev", "nic"}) {
        for (int i : args.indexesOf(option)) {
            const OptionValue v = args.valueAt(i);
            const QString type = v.implied().isEmpty() ? v.get("type") : v.implied();
            QString error;

            if (type.isEmpty() || type == "none") {
                continue;
            }
            const QStringList known = QemuInfo::probeList(qemu, "netdev", &error);
            if (error.isEmpty() && !known.isEmpty() && !known.contains(type)) {
                add(type);
            }
        }
    }
    return out;
}

bool VmRunner::needsQemuBuild(const ArgsFile &args, QString *why)
{
    QString system;
    QStringList lacks;

    if (!VmConfig::qemuBinary(args).isEmpty() || !Paths::customQemuBinary().isEmpty() ||
        !Paths::stackQemu().isEmpty()) {
        return false;
    }
    system = Paths::defaultQemuBinary();
    if (system.isEmpty()) {
        if (why) {
            *why = tr("Vitrine's QEMU is not built yet, and %1 is not in PATH: build it with "
                      "File > Build QEMU, or choose a QEMU in the preferences")
                       .arg(Paths::qemuSystemName());
        }
        return true;
    }
    lacks = lacking(args, system);
    if (lacks.isEmpty()) {
        return false;
    }
    if (why) {
        *why = tr("This VM uses %1, which the system's QEMU lacks: build Vitrine's QEMU with "
                  "File > Build QEMU, or choose another QEMU in the preferences")
                   .arg(lacks.join(", "));
    }
    return true;
}

QStringList VmRunner::Private::commandLine(const ArgsFile &args, const QString &qemu,
                                          QStringList *problems) const
{
    const QList<VmConfig::Share> shares = VmConfig::shares(args);
    QStringList command{qemu};

    /* honor-guest-pat=on, which QEMU refuses where KVM cannot do it, as auto there */
    QString patNote;
    const ArgsFile hostArgs = HostKvm::withHostPat(args, HostKvm::canHonorGuestPat, &patNote);
    if (!patNote.isEmpty() && problems) {
        *problems << patNote;
    }
    command += withComputedProperties(hostArgs, [&qemu, problems](const QString &driver) {
                   QString error;
                   const QStringList names = QemuInfo::probeProperties(qemu, driver, &error);
                   if (!error.isEmpty() && problems) {
                       *problems << VmRunner::tr("cannot read the properties of %1 from %2 "
                                                 "(%3): it runs without those vitrine "
                                                 "computes, such as its swap targets")
                                        .arg(driver, qemu, error);
                   }
                   return names;
               }).argv();
    for (qsizetype i = 0; i < shares.size(); i++) {
        command << "-chardev"
                << QString("socket,id=vitrine-fs%1,path=%2")
                       .arg(QString::number(i), OptionValue::escape(sharePath(i)))
                << "-device"
                << QString("vhost-user-fs-pci,queue-size=1024,chardev=vitrine-fs%1,tag=%2")
                       .arg(QString::number(i), OptionValue::escape(shares[i].tag));
    }
    /* systemd in the guest mounts the shares at boot, else qemu-ga does */
    const QString fstab = fstabExtra(args, qemu);
    if (!fstab.isEmpty()) {
        command << "-smbios"
                << "type=11,value=io.systemd.credential.binary:fstab.extra=" +
                       QString::fromLatin1(fstab.toUtf8().toBase64());
    }
    if (addsAgent(args)) {
        command << "-chardev"
                << QString("socket,id=vitrine-ga,path=%1,server=on,wait=off")
                       .arg(OptionValue::escape(agentPath()))
                << "-device" << "virtio-serial-pci,id=vitrine-serial"
                << "-device"
                << QString("virtserialport,bus=vitrine-serial.0,chardev=vitrine-ga,"
                           "name=org.qemu.guest_agent.0,id=%1").arg(kAgentPort);
    }
    if (VmConfig::screen(args) == VmConfig::Screen::Embedded) {
        /* a second -qmp; -mon is deprecated */
        command << "-qmp" << displayArg();
    }
    /* the guest tools' agent, on qemu-ga's controller if there is one */
    if (GuestTools::addsAgentPort(args, qemu)) {
        if (!addsAgent(args)) {
            command << "-device" << "virtio-serial-pci,id=vitrine-serial";
        }
        command += GuestTools::agentPortArgs(toolsAgentPath());
    }
    /* the guest tools asked for: the medium, and the unit that installs them at boot */
    const GuestTools::Pending tools = GuestTools::pending(id);
    const GuestTools::Medium medium = GuestTools::medium();
    if (tools != GuestTools::Pending::None && medium.isValid() &&
        GuestTools::canBootstrap(args, qemu)) {
        command += GuestTools::mediumArgs(medium.image);
        if (tools == GuestTools::Pending::Bootstrap) {
            command += GuestTools::bootstrapArgs();
        }
    }
    command << "-qmp" << qmpArg() << "-pidfile" << pidPath();
    return command;
}

ArgsFile VmRunner::withComputedProperties(
    const ArgsFile &args, const std::function<QStringList(const QString &driver)> &propertiesOf)
{
    static const QStringList cards = {
        "virtio-vga-gl", "virtio-gpu-gl-pci", "virtio-gpu-gl", "virtio-gpu-gl-device",
    };
    const bool kde = VmConfig::guest(args).desktop == "kde";
    const bool embedded = VmConfig::screen(args) == VmConfig::Screen::Embedded;
    const std::pair<QString, QString> computed[] = {
        /*
         * The frames of a burst are swapped this long before the vblank
         * that shows them: the host's KWin takes a frame ~3.9 ms before
         * its vblank at 240 Hz; KDE's commits in the guest are steady
         * enough for 4.5 ms, other desktops keep a margin of 6 ms
         */
        {"x-vblank-swap-target", kde ? "4500" : "6000"},
        /*
         * Those that go to the screen as they are (zero copy) need no
         * copy before the host takes them, but must come before the
         * host compositor's frame start (3.5 ms had the fewest late
         * frames); the D-Bus display's frames take one hop more, the
         * client's commit, worth 1 ms
         */
        {"x-vblank-swap-target-zc", embedded ? "4500" : "3500"},
    };
    ArgsFile out = args;
    QStringList global;

    /*
     * Those the user sets with -global: QEMU applies them when it creates
     * the card, and the -device line's own after, which would win.  Any
     * driver counts, as the card's inner device and parent types take them
     * too, and only vitrine's virtio-gpu has these properties.
     */
    for (int i : args.indexesOf("global")) {
        const OptionValue g = args.valueAt(i);
        if (g.has("property")) {
            global << g.get("property");
        } else if (!g.items().isEmpty()) {
            /* DRIVER.PROPERTY=VALUE, split at the first dot as QEMU does */
            global << g.items().first().key.section('.', 1);
        }
    }
    for (int i : out.indexesOf("device")) {
        OptionValue v = out.valueAt(i);
        QStringList known;
        bool changed = false;

        if (!cards.contains(v.implied())) {
            continue;
        }
        known = propertiesOf(v.implied());
        for (const auto &[key, value] : computed) {
            if (!v.has(key) && !global.contains(key) && known.contains(key)) {
                v.set(key, value);
                changed = true;
            }
        }
        if (changed) {
            out.setValueAt(i, v);
        }
    }
    return out;
}

/*
 * QEMU's SDL window as the research launcher runs it: input read at the
 * refresh rate while it has the focus, and in full screen the guest's
 * buffers go to the screen as they are (the guest's driver must hold them,
 * else QEMU copies them, as other QEMUs ignore the variables).  A VM's
 * "#env NAME=0" turns each off in Vitrine's QEMU.
 */
static const QStringList kSdlEnvironment = {
    "QEMU_SDL_POLL_FOCUSED=1",
    "QEMU_SDL_ZERO_COPY=1",
    "QEMU_SDL_ZC_TILED=explicit",
};

QStringList VmRunner::environment(const ArgsFile &args)
{
    QStringList env;

    if (VmConfig::screen(args) == VmConfig::Screen::OwnWindow &&
        VmConfig::graphics(args).display != "gtk") {
        env = kSdlEnvironment;
    }
    for (const VmConfig::EnvVar &var : VmConfig::environment(args)) {
        env.removeIf([&var](const QString &s) { return s.startsWith(var.name + '='); });
        env << var.name + '=' + var.value;
    }
    return env;
}

QString VmRunner::agentSocket() const
{
    return isActive() ? d->toolsAgentPath() : QString();
}

QStringList VmRunner::shellAssignments(const QStringList &environment)
{
    QStringList out;

    for (const QString &var : environment) {
        const qsizetype eq = var.indexOf('=');
        out << (eq < 0 ? shellQuote({var}) : var.left(eq + 1) + shellQuote({var.mid(eq + 1)}));
    }
    return out;
}

QString VmRunner::displaySocket() const
{
    return isActive() && d->embedded ? d->displayPath() : QString();
}

ArgsFile VmRunner::runArgs() const
{
    return d->args;
}

void VmRunner::start(const ArgsFile &args)
{
    const QString qemu = qemuFor(args);
    const QList<VmConfig::Share> shares = VmConfig::shares(args);
    QString virtiofsd;

    if (isActive() || d->phase != Private::Phase::Idle) {
        return;
    }
    /* left running by an earlier run of the manager: with its own arguments */
    if (d->runningPid() > 0) {
        attach(args);
        return;
    }

    d->error.clear();
    d->stopRequested = false;
    d->forceRequested = false;
    d->killStep = 0;
    d->args = args;
    d->qemu = qemu;
    d->command.clear();
    d->embedded = VmConfig::screen(args) == VmConfig::Screen::Embedded;
    if (QString why; needsQemuBuild(args, &why)) {
        d->fail(why);
        return;
    }
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
    /* what this run is, for a manager that finds it running: vm.args may
       say otherwise by then (the settings apply at the next start) */
    QSaveFile runArgs(d->argsPath());
    if (!runArgs.open(QIODevice::WriteOnly) || runArgs.write(args.toText().toUtf8()) < 0 ||
        !runArgs.commit()) {
        d->fail(tr("Cannot write %1: %2").arg(d->argsPath(), runArgs.errorString()));
        return;
    }
    QFile log(d->logPath());
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        d->fail(tr("Cannot write %1: %2").arg(d->logPath(), log.errorString()));
        return;
    }
    const QStringList environment = VmRunner::environment(args);
    /* what launchQemu() runs, after virtiofsd if any */
    QStringList problems;
    d->command = d->commandLine(args, qemu, &problems);
    log.write(QString("vitrine: %1 %2%3\n")
                  .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                       environment.isEmpty()
                           ? QString() : shellAssignments(environment).join(' ') + ' ',
                       shellQuote(d->command))
                  .toUtf8());
    const QStringList notes = remade + problems;
    for (const QString &line : notes) {
        log.write(("vitrine: " + line + '\n').toUtf8());
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
    const qint64 pid = d->runningPid();
    if (pid <= 0) {
        d->args = args;
        d->removeRuntimeFiles();
        return;
    }
    /*
     * The run's own arguments, not vm.args: the shares to mount, what the
     * window offers.  A QEMU started by a vitrine that did not keep them has
     * vm.args.  Whether its screen can show here is what its command line
     * says, the display's -qmp: reading the arguments again with this
     * vitrine's rules may not give the answer the one that started it had.
     */
    const QStringList running = cmdline(pid);
    QFile runArgs(d->argsPath());
    if (runArgs.open(QIODevice::ReadOnly)) {
        d->args = ArgsFile::parse(QString::fromUtf8(runArgs.readAll()));
    } else {
        d->args = args;
    }
    d->embedded = running.contains(d->displayArg());
    d->pid = pid;
    /* the binary it runs, not the one the preferences may name now */
    d->qemu = running.value(0);
    d->command.clear();
    d->error.clear();
    d->stopRequested = false;
    d->forceRequested = false;
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
    if (d->shutdownHandler && isActive() && d->shutdownHandler()) {
        return;
    }
    pressPowerButton();
}

void VmRunner::pressPowerButton()
{
    if (d->qmp->isReady()) {
        d->qmp->execute("system_powerdown", {}, d->reportErrors("system_powerdown"));
    }
}

void VmRunner::setShutdownHandler(const std::function<bool()> &handler)
{
    d->shutdownHandler = handler;
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
    if (!isActive()) {
        return;
    }
    d->forceRequested = true;
    if (d->killStep == 2) {
        /* said already, and the user chose to wait: asked again */
        if (alive(d->pid)) {
            emit notResponding();
        }
        return;
    }
    if (d->killTimer->isActive()) {
        /* quit or SIGTERM sent: the steps go on */
        return;
    }
    d->stopRequested = true;
    if (d->phase == Private::Phase::Exiting) {
        /* its monitor closed and it runs on: SIGTERM, then the user */
        d->escalate();
        return;
    }
    d->setState(State::Stopping);
    if (d->qmp->isReady()) {
        d->qmp->execute("quit");
        d->killTimer->start(d->quitTimeoutMs);
    } else {
        /* still starting: no QMP to ask QEMU to quit */
        d->escalate();
    }
}

void VmRunner::killQemu()
{
    if (!isActive() || d->phase == Private::Phase::Helpers || !alive(d->pid)) {
        return;
    }
    d->stopRequested = true;
    d->killTimer->stop();
    appendNote(tr("QEMU killed (SIGKILL) at the user's request: it did not respond"));
    /* its end follows as any other: the monitor closes, or the poll sees it */
    signalIfOurs(d->pid, d->qmpArg(), SIGKILL);
}

void VmRunner::setQuitTimeout(int ms)
{
    d->quitTimeoutMs = ms;
}

qint64 VmRunner::pid() const
{
    return isActive() ? d->pid : 0;
}

QmpClient *VmRunner::qmp() const
{
    return isActive() ? d->qmp : nullptr;
}
