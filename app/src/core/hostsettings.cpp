// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostsettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/stackbuilder.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

extern char **environ;

#ifndef VITRINE_HELPER_PATH
#define VITRINE_HELPER_PATH "/usr/libexec/vitrine-helper"
#endif

/* polkit's actions: one per verb of the helper (its first argument) */
static const char kAction[] = "org.vitrine.helper";                 // session
static const char kSetcapAction[] = "org.vitrine.helper.setcap";
/* pkexec's line when it does not run the program: "...: Not authorized" */
static const char kPkexecError[] = "Error executing command as another user: ";
/* A helper that ends while VMs it tuned run is started again, this many
   times at most per run of vitrine */
static const int kRestarts = 5;

static QPointer<HostSettings> s_instance;

static QSettings settings()
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat);
}

bool HostSettings::enabled()
{
    return settings().value("host/tune", true).toBool();
}

void HostSettings::setEnabled(bool on)
{
    settings().setValue("host/tune", on);
}

QString HostSettings::gpuFloor()
{
    static const QRegularExpression mhz("^[1-9][0-9]{1,4}$");
    const QString floor = settings().value("host/gpuFloor", "auto").toString();

    return floor == "off" || mhz.match(floor).hasMatch() ? floor : QString("auto");
}

void HostSettings::setGpuFloor(const QString &floor)
{
    settings().setValue("host/gpuFloor", floor);
}

QString HostSettings::helperPath()
{
    const QString env = qEnvironmentVariable("VITRINE_HELPER");
    return env.isEmpty() ? QString(VITRINE_HELPER_PATH) : env;
}

bool HostSettings::helperInstalled()
{
    return QFileInfo(helperPath()).isExecutable();
}

/* The group polkit's rule names; $VITRINE_GROUP for tests */
static QByteArray groupName()
{
    const QByteArray env = qgetenv("VITRINE_GROUP");
    return env.isEmpty() ? QByteArray("vitrine") : env;
}

bool HostSettings::vitrineGroupExists()
{
    return getgrnam(groupName().constData()) != nullptr;
}

bool HostSettings::inVitrineGroup()
{
    const struct group *gr = getgrnam(groupName().constData());
    const struct passwd *pw = getpwuid(getuid());

    if (!gr || !pw) {
        return false;
    }
    const gid_t vitrine = gr->gr_gid;
    std::vector<gid_t> groups(64);
    int n = int(groups.size());
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups.data(), &n) < 0) {
        groups.resize(size_t(n));
        if (getgrouplist(pw->pw_name, pw->pw_gid, groups.data(), &n) < 0) {
            return false;
        }
    }
    for (int i = 0; i < n; i++) {
        if (groups[size_t(i)] == vitrine) {
            return true;
        }
    }
    return false;
}

QStringList HostSettings::amdCards(const QString &sysRoot, bool apus)
{
    static const QRegularExpression name("^card[0-9]+$");
    const QDir drm(sysRoot + "/class/drm");
    QStringList cards;

    for (const QString &card : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString device = drm.filePath(card) + "/device/";
        QFile vendor(device + "vendor"), metrics(device + "gpu_metrics");

        if (!name.match(card).hasMatch() || !vendor.open(QIODevice::ReadOnly) ||
            vendor.readAll().trimmed() != "0x1002" ||
            !QFileInfo(device + "pp_od_clk_voltage").isReadable()) {
            continue;
        }
        /* amdgpu lays out an APU's metrics as gpu_metrics_v2_x or v3_x: the
           third byte of the header is that format revision */
        if (apus) {
            const QByteArray header = metrics.open(QIODevice::ReadOnly) ? metrics.read(4)
                                                                         : QByteArray();
            if (header.size() < 4 || (header[2] != 2 && header[2] != 3)) {
                continue;
            }
        }
        cards << card;
    }
    return cards;
}

/* Running, not a zombie (QEMU is not vitrine's child: init reaps it) */
static bool alive(qint64 pid)
{
    QFile f(QString("/proc/%1/stat").arg(pid));
    const QByteArray stat = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    const qsizetype end = stat.lastIndexOf(')');

    return pid > 0 && end > 0 && stat.size() > end + 2 && stat[end + 2] != 'Z' &&
           stat[end + 2] != 'X';
}

/* The process has CAP_SYS_NICE in effect (the stack's QEMU, setcap'ed) */
static bool hasSysNice(qint64 pid)
{
    QFile f(QString("/proc/%1/status").arg(pid));
    const QList<QByteArray> lines = f.open(QIODevice::ReadOnly) ? f.readAll().split('\n')
                                                                : QList<QByteArray>();

    for (const QByteArray &line : lines) {
        if (line.startsWith("CapEff:")) {
            bool ok;
            const qulonglong caps = line.mid(7).trimmed().toULongLong(&ok, 16);
            return ok && (caps >> 23 & 1);     // CAP_SYS_NICE
        }
    }
    return false;
}

/* The calling process for pkcheck: pid, start time, uid (no pid reuse) */
static QString subject()
{
    QFile f("/proc/self/stat");
    const QString stat = f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()) : QString();
    /* after the command's ")": the state is field 3, the start time 22 */
    const QStringList fields = stat.mid(stat.lastIndexOf(')') + 2).split(' ');

    return QString("%1,%2,%3").arg(getpid()).arg(fields.value(19)).arg(getuid());
}

QString HostSettings::Status::why() const
{
    switch (problem) {
    case Problem::None:
        return QString();
    case Problem::NotInstalled:
        return tr("vitrine-helper is not installed");
    case Problem::NoPolkit:
        return tr("polkit's pkcheck is not installed");
    case Problem::NoPolicy:
        return tr("the helper's polkit policy is not installed");
    case Problem::NoGroup:
        return tr("you are not in the vitrine group, which does not exist yet");
    case Problem::NotMember:
        return tr("you are not in the vitrine group");
    case Problem::NotLocal:
        return tr("polkit wants a password here: the vitrine group's rule applies in a local, "
                  "active desktop session only");
    case Problem::Failed:
        break;
    }
    return detail;
}

HostSettings::Status HostSettings::classify(bool installed, int pkcheckStatus,
                                            const QString &pkcheckError, bool groupExists,
                                            bool member)
{
    const QString error = pkcheckError.trimmed().section('\n', 0, 0);

    if (!installed) {
        return {Problem::NotInstalled, {}};
    }
    switch (pkcheckStatus) {
    case -1:
        return {Problem::NoPolkit, {}};
    case 0:
        return {};
    case 2:
    case 3:
        /* a password would be needed: the group's rule wants a member at a
           local, active session (49-vitrine.rules) */
        return {!groupExists ? Problem::NoGroup : !member ? Problem::NotMember : Problem::NotLocal,
                {}};
    case 1:
        /* polkit says no, as an administrator's rule may */
        return {Problem::Failed, error.isEmpty() ? tr("polkit does not allow it here") : error};
    }
    if (error.contains("not registered")) {
        return {Problem::NoPolicy, {}};
    }
    return {Problem::Failed, error.isEmpty() ? tr("polkit did not answer") : error};
}

/* A message as part of a sentence: without its period */
static QString clause(QString text)
{
    text = text.trimmed();
    if (text.endsWith('.') && !text.endsWith("..")) {
        text.chop(1);
    }
    return text;
}

/*
 * Asks polkit whether this process may run the helper's @action without
 * anyone typing a password, and with the user database says why not
 */
static void checkAction(const char *action, QObject *context,
                        const std::function<void(const HostSettings::Status &)> &done)
{
    using Problem = HostSettings::Problem;

    if (!HostSettings::helperInstalled()) {
        done({Problem::NotInstalled, {}});
        return;
    }
    auto *check = new QProcess(context);

    QObject::connect(check, &QProcess::finished, context,
                     [check, done](int code, QProcess::ExitStatus status) {
        const QString err = QString::fromLocal8Bit(check->readAllStandardError());
        check->deleteLater();
        done(HostSettings::classify(true, status == QProcess::NormalExit ? code : 127, err,
                                    HostSettings::vitrineGroupExists(),
                                    HostSettings::inVitrineGroup()));
    });
    QObject::connect(check, &QProcess::errorOccurred, context,
                     [check, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            check->deleteLater();
            done(HostSettings::classify(true, -1, QString(), false, false));
        }
    });
    /* no --allow-user-interaction: never a dialog from here */
    check->start("pkcheck", {"--action-id", action, "--process", subject()});
}

void HostSettings::check(QObject *context, const std::function<void(const Status &)> &done)
{
    checkAction(kAction, context, done);
}

/* vitrine-helper setcap @path, once polkit said yes */
static void setcap(const QString &path, QObject *context,
                   const std::function<void(const QString &error)> &done)
{
    auto *run = new QProcess(context);
    run->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(run, &QProcess::finished, context, [run, path, done](int code) {
        const QString last = QString::fromUtf8(run->readAll()).trimmed().section('\n', -1);
        const QString prefix = "error setcap " + path + ": ";
        run->deleteLater();
        /* "error setcap PATH: why", or pkexec's own message */
        done(code == 0 ? QString()
             : last.startsWith(prefix) ? last.mid(prefix.size())
             : last.isEmpty() ? HostSettings::tr("vitrine-helper failed") : last);
    });
    QObject::connect(run, &QProcess::errorOccurred, context,
                     [run, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            run->deleteLater();
            done(HostSettings::tr("pkexec is not installed"));
        }
    });
    run->start("pkexec", {"--disable-internal-agent", VITRINE_HELPER_PATH, "setcap", path});
}

void HostSettings::grantCapability(const QString &qemu, QObject *context,
                                   const std::function<void(const QString &error)> &done)
{
    /* the helper refuses links: the stack's `current` among others */
    const QString path = QFileInfo(qemu).canonicalFilePath();

    if (!helperInstalled()) {
        done(tr("vitrine-helper is not installed"));
        return;
    }
    if (path.isEmpty()) {
        done(tr("%1 does not exist").arg(qemu));
        return;
    }
    /* its own action, which pkexec will use: a rule may grant one and not the other */
    checkAction(kSetcapAction, context, [path, context, done](const Status &status) {
        if (!status.active()) {
            done(status.why());
            return;
        }
        setcap(path, context, done);
    });
}

void HostSettings::ensureCapability()
{
    /* canonical: the build itself, not `current` */
    const QString qemu = Paths::stackQemu();
    char value[64];

    if (!enabled() || !helperInstalled() || qemu.isEmpty() || m_capabilityTried.contains(qemu) ||
        getxattr(QFile::encodeName(qemu).constData(), "security.capability", value,
                 sizeof(value)) >= 0 ||
        errno != ENODATA) {
        return;
    }
    auto grant = [this, qemu]() {
        m_capabilityTried.insert(qemu);
        setcap(qemu, this, [this](const QString &error) {
            if (!error.isEmpty()) {
                say(tr("Vitrine's QEMU cannot make its threads real-time: %1.").arg(clause(error)));
            }
        });
    };
    /* polkit's no is shown at a VM start, not here */
    checkAction(kSetcapAction, this, [grant](const Status &status) {
        if (status.active()) {
            grant();
        }
    });
}

HostSettings *HostSettings::instance()
{
    return s_instance;
}

HostSettings::HostSettings(VmStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    if (!s_instance) {
        s_instance = this;
    }
    if (store) {
        for (Vm *vm : store->vms()) {
            watchVm(vm);
        }
        connect(store, &VmStore::added, this, &HostSettings::watchVm);
        connect(store, &VmStore::removed, this,
                [this](const QString &id) { setUntuned(m_tuned.take(id), {}); });
    }
    /*
     * Each build is a new file, without the capability of the one before:
     * given after each build, and, when it lacks it, now - a QEMU built
     * before joining the vitrine group or while tuning was off.  It takes
     * effect at the next start of each VM: a running QEMU keeps what it has.
     */
    ensureCapability();
    connect(StackBuilder::instance(), &StackBuilder::built, this, [this](const QString &qemu) {
        /* without the helper, a VM start says it */
        if (!enabled() || !helperInstalled()) {
            return;
        }
        grantCapability(qemu, this, [this](const QString &error) {
            if (!error.isEmpty()) {
                say(tr("Vitrine's QEMU cannot make its threads real-time: %1.").arg(clause(error)));
            }
        });
    });
}

HostSettings::~HostSettings()
{
    /* the helper sees its input end and goes on while the VMs it watches
       run: they outlive vitrine.  Not waited for: it is root's process */
    closeHelper();
    if (m_pid > 0) {
        waitpid(m_pid, nullptr, WNOHANG);
    }
}

void HostSettings::watchVm(Vm *vm)
{
    connect(vm->runner(), &VmRunner::stateChanged, this, [this, vm]() { vmStateChanged(vm); });
    vmStateChanged(vm);
}

void HostSettings::vmStateChanged(Vm *vm)
{
    const VmRunner::State state = vm->runner()->state();
    const qint64 pid = vm->runner()->pid();

    if (state == VmRunner::State::Stopped) {
        /* no longer untuned either */
        setUntuned(m_tuned.take(vm->id()), {});
        if (m_front == vm->id()) {
            m_front.clear();
        }
    } else if ((state == VmRunner::State::Running || state == VmRunner::State::Paused) &&
               pid > 0 && m_tuned.value(vm->id()) != pid) {
        /* a new run, started here or found running: tuned once */
        m_tuned[vm->id()] = pid;
        tune(pid);
    }
}

void HostSettings::tune(qint64 pid)
{
    const QString n = QString::number(pid);
    const QString floor = gpuFloor();

    if (!enabled()) {
        /* turned off meanwhile: the helper lets go of everything now */
        if (m_fd >= 0) {
            m_out += "release\n";
            flush();
        }
        return;
    }
    if (m_access == Access::Denied) {
        if (alive(pid)) {
            setUntuned(pid, m_denied);
        }
        return;
    }
    m_expected.insert(pid);
    m_out += ("watch " + n + "\nfair-server on\n").toLatin1();
    /* auto: on APUs only (measured on a Radeon 780M), none on the other
       cards, where a fixed one chosen before goes; off: let a floor go */
    const QStringList apus = amdCards(m_sysRoot, true);
    for (const QString &card : amdCards(m_sysRoot, false)) {
        const QString value = floor != "auto" ? floor : apus.contains(card) ? "auto" : "off";
        m_out += QString("gpu-floor %1 %2\n").arg(card, value).toLatin1();
    }
    m_out += ("rt " + n + '\n').toLatin1();
    start();
}

void HostSettings::preferencesChanged()
{
    if (!enabled()) {
        /* off: everything back now, not after the last VM, and nothing
           queued for a helper still starting (its check or its "ready") */
        m_out.clear();
        m_expected.clear();
        if (m_fd >= 0) {
            m_out = "release\n";
            flush();
        }
        /* and focus priority: the vCPUs QEMU itself made real-time, or
           ordinary at nice -5, are not the helper's to put back */
        for (Vm *vm : m_store && !m_front.isEmpty() ? m_store->vms() : QList<Vm *>()) {
            QmpClient *qmp = vm->runner()->qmp();
            if (qmp && m_tuned.contains(vm->id()) &&
                VmConfig::screen(vm->runner()->runArgs()) != VmConfig::Screen::OwnWindow) {
                qmp->execute("x-vcpu-priority", QJsonObject{{"realtime", false}, {"nice", 0}});
            }
        }
        m_front.clear();
        /* and nothing to show: off is the user's choice */
        if (!m_untuned.isEmpty()) {
            m_untuned.clear();
            emit untunedChanged();
        }
        return;
    }
    /* on, or another floor: each running VM tuned again - a run that
       started untuned among them; what the helper holds already, it says
       so ("already"), and a floor it holds it replaces */
    ensureCapability();
    if (!m_store) {
        return;
    }
    m_tuned.clear();
    for (Vm *vm : m_store->vms()) {
        vmStateChanged(vm);
    }
}

void HostSettings::setFront(const QString &vmId)
{
    Vm *front = m_store && !vmId.isEmpty() ? m_store->find(vmId) : nullptr;

    if (!front || vmId == m_front || !m_tuned.contains(vmId) || !enabled()) {
        return;
    }
    m_front = vmId;
    for (Vm *vm : m_store->vms()) {
        applyFront(vm);
    }
}

void HostSettings::applyFront(Vm *vm)
{
    const qint64 pid = m_tuned.value(vm->id());
    QmpClient *qmp = vm->runner()->qmp();
    const bool front = vm->id() == m_front;

    if (m_front.isEmpty() || !pid || !qmp) {
        return;
    }
    /* SDL: its window's focus is not known here, behind or not */
    if (!front && VmConfig::screen(vm->runner()->runArgs()) == VmConfig::Screen::OwnWindow) {
        return;
    }
    const bool capable = hasSysNice(pid);
    QJsonObject arguments{{"realtime", front}};
    if (front && !capable) {
        /* QEMU may not make its threads real-time again: the helper may */
        if (m_fd >= 0) {
            m_out += "rt " + QByteArray::number(pid) + '\n';
            flush();
        }
        return;
    }
    if (!front) {
        /* the patch stops at the first thread it fails on: no lower nice
           than this QEMU may set */
        arguments["nice"] = capable ? -5 : 0;
    }
    /* a QEMU without the command (not vitrine's) says so: nothing to do */
    qmp->execute("x-vcpu-priority", arguments);
}

void HostSettings::start()
{
    if (m_fd >= 0) {
        flush();
        return;
    }
    if (m_access == Access::Checking) {
        /* the check's end goes on */
        return;
    }
    if (!m_command.isEmpty()) {
        spawn(m_command);
        return;
    }
    if (!helperInstalled()) {
        deny({Problem::NotInstalled, {}}, m_expected);
        return;
    }
    /*
     * polkit, asked before each start of the helper and never remembered:
     * a membership of the vitrine group given since counts (no restart of
     * vitrine), and one taken back too - pkexec would hand a request polkit
     * wants a password for to the desktop's agent, a dialog at a VM start
     * (--disable-internal-agent only turns off its own).  A membership
     * taken between the check and pkexec is the window left.  pkexec runs
     * the installed helper only, the one the action names.
     */
    m_access = Access::Checking;
    check(this, [this](const Status &status) {
        m_access = Access::Unknown;
        if (!status.active()) {
            deny(status, m_expected);
            return;
        }
        /* the group joined since vitrine started, say */
        ensureCapability();
        if (!m_out.isEmpty()) {
            spawn({"pkexec", "--disable-internal-agent", VITRINE_HELPER_PATH, "session"});
        }
    });
}

void HostSettings::spawn(const QStringList &command)
{
    std::vector<QByteArray> args;
    std::vector<char *> argv;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    sigset_t none, all;
    int sv[2], err;
    pid_t pid;

    /* a socket, not a pipe: writing to a helper that has gone away gives
       EPIPE (MSG_NOSIGNAL), not a SIGPIPE that would end vitrine */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        deny({Problem::Failed, QString::fromLocal8Bit(strerror(errno))}, m_expected);
        return;
    }
    for (const QString &arg : command) {
        args.push_back(arg.toLocal8Bit());
    }
    for (QByteArray &arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDERR_FILENO);
    posix_spawnattr_init(&attr);
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    /* a session of its own: Ctrl+C in vitrine's terminal, or the terminal
       closing, is not for the helper of VMs that run on */
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK |
                                        POSIX_SPAWN_SETSIGDEF);
    err = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(sv[1]);
    if (err) {
        ::close(sv[0]);
        deny({Problem::Failed,
              tr("cannot run %1: %2").arg(command.first(), QString::fromLocal8Bit(strerror(err)))},
             m_expected);
        return;
    }
    m_pid = pid;
    m_fd = sv[0];
    fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    m_ready = false;
    m_in.clear();
    m_foreign.clear();
    m_readable = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_readable, &QSocketNotifier::activated, this, &HostSettings::readHelper);
    m_writable = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    m_writable->setEnabled(false);
    connect(m_writable, &QSocketNotifier::activated, this, &HostSettings::flush);
    m_pidfd = int(syscall(SYS_pidfd_open, pid, 0));
    if (m_pidfd >= 0) {
        m_exited = new QSocketNotifier(m_pidfd, QSocketNotifier::Read, this);
        connect(m_exited, &QSocketNotifier::activated, this, &HostSettings::reap);
    } else {
        /* kernels before 5.3 */
        m_exitPoll = new QTimer(this);
        connect(m_exitPoll, &QTimer::timeout, this, &HostSettings::reap);
        m_exitPoll->start(500);
    }
}

void HostSettings::readHelper()
{
    char buf[4096];
    qsizetype nl;

    for (;;) {
        const ssize_t n = ::read(m_fd, buf, sizeof(buf));
        if (n > 0) {
            m_in.append(buf, n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            if (n == 0) {
                /* the end of its output: its exit follows */
                m_readable->setEnabled(false);
            }
            break;
        }
    }
    while ((nl = m_in.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_in.left(nl)).trimmed();
        m_in.remove(0, nl + 1);
        handleLine(line);
    }
}

void HostSettings::handleLine(const QString &line)
{
    static const QStringList protocol{"ready", "ok",       "skip", "error",
                                      "exited", "restored", "left", "bye"};
    const QString word = line.section(' ', 0, 0);

    emit helperLine(line);
    if (!protocol.contains(word)) {
        /* pkexec's, before the helper runs: why it did not */
        if (!line.isEmpty()) {
            m_foreign << line;
        }
    } else if (word == "ready") {
        m_ready = true;
        flush();
    } else if (line.startsWith("ok watch ")) {
        /* "ok watch PID", "ok watch PID: already": tuned, at last */
        setUntuned(line.section(' ', 2, 2).section(':', 0, 0).toLongLong(), {});
    } else if (word == "exited" || line.startsWith("error watch ")) {
        /* "exited PID", "error watch PID: why" */
        const qint64 pid = line.section(' ', word == "exited" ? 1 : 2).section(':', 0, 0).toLongLong();
        /* a VM that ended at once is no news; one the helper would not take
           (another program than QEMU) runs untuned */
        if (word == "error" && alive(pid)) {
            QString name = tr("process %1").arg(pid);
            for (Vm *vm : m_store ? m_store->vms() : QList<Vm *>()) {
                if (m_tuned.value(vm->id()) == pid) {
                    name = vm->name();
                }
            }
            setUntuned(pid, {Problem::Failed,
                             tr("vitrine-helper does not take %1: %2")
                                 .arg(name, line.section(':', 1).trimmed())});
        }
        m_expected.remove(pid);
        if (m_expected.isEmpty() && m_out.isEmpty() && m_fd >= 0) {
            /* nothing left to watch: it ends, putting everything back */
            ::shutdown(m_fd, SHUT_WR);
        }
    } else if (line.startsWith("ok rt ")) {
        /*
         * "ok rt PID: N of M threads real-time": every thread of that QEMU,
         * the vCPUs of a VM behind the one in front among them - a VM that
         * started since, or tuned again, or one whose x-vcpu-priority went
         * out before the helper got to it.  Behind again, now.
         */
        const qint64 pid = line.section(' ', 2, 2).section(':', 0, 0).toLongLong();
        for (Vm *vm : m_store ? m_store->vms() : QList<Vm *>()) {
            if (pid > 0 && m_tuned.value(vm->id()) == pid && vm->id() != m_front) {
                applyFront(vm);
            }
        }
    } else if (word == "error" && (line.endsWith(": watch a QEMU first") ||
                                   line.endsWith(": watch the process first"))) {
        /* after a watch that failed: said already, if worth it */
    } else if (word == "skip" || word == "error") {
        /* "skip fair-server: kernel lockdown (integrity)" */
        say(tr("Host tuning: %1").arg(line.section(' ', 1)));
    }
}

void HostSettings::flush()
{
    if (m_fd < 0 || !m_ready) {
        return;
    }
    while (!m_out.isEmpty()) {
        const ssize_t n = ::send(m_fd, m_out.constData(), size_t(m_out.size()),
                                 MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            m_out.remove(0, n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            /* full (the rest when it can take it), or gone (reap() follows) */
            break;
        }
    }
    m_writable->setEnabled(!m_out.isEmpty() && errno == EAGAIN);
}

void HostSettings::reap()
{
    int status;

    if (m_pid <= 0 || waitpid(m_pid, &status, WNOHANG) == 0) {
        return;
    }
    /* what it said last */
    readHelper();
    const bool ran = m_ready;
    /* asked to be watched, neither ended nor refused: a VM that started as
       the helper was ending (its requests came too late), or those of a
       helper that failed */
    const QSet<qint64> pending = m_expected;
    /* pkexec's own line, not what follows it ("This incident has been
       reported.") */
    QString why = m_foreign.isEmpty() ? tr("vitrine-helper did not start") : m_foreign.first();
    for (const QString &line : std::as_const(m_foreign)) {
        if (line.startsWith("Error executing command")) {
            why = line;
            break;
        }
    }
    m_pid = 0;
    closeHelper();
    m_out.clear();
    m_expected.clear();
    emit helperFinished();
    if (!ran) {
        /* pkexec refused it, though polkit said yes, or it could not start:
           not again in this run */
        deny({Problem::Failed, why}, pending, true);
        return;
    }
    /*
     * It ends after the last VM it watched.  Started again for those only:
     * not for every VM that runs, among which one it refused to watch (a
     * QEMU of another name), which would have it started and refused again
     * until the restarts of the run were used up.
     */
    if (!m_store || !enabled() || pending.isEmpty() || m_restarts >= kRestarts) {
        return;
    }
    m_restarts++;
    for (Vm *vm : m_store->vms()) {
        const qint64 pid = m_tuned.value(vm->id());
        if (pending.contains(pid) && alive(pid)) {
            tune(pid);
        }
    }
}

void HostSettings::closeHelper()
{
    delete m_readable;
    delete m_writable;
    delete m_exited;
    delete m_exitPoll;
    m_readable = m_writable = m_exited = nullptr;
    m_exitPoll = nullptr;
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_pidfd >= 0) {
        ::close(m_pidfd);
        m_pidfd = -1;
    }
    m_ready = false;
}

void HostSettings::deny(const Status &status, QSet<qint64> pids, bool always)
{
    if (always) {
        m_access = Access::Denied;
        m_denied = status;
    }
    /* the VMs asked for go untuned: none of them is waited for (a VM that
       ended at once is no news) */
    m_out.clear();
    m_expected.clear();
    for (qint64 pid : std::as_const(pids)) {
        if (alive(pid)) {
            setUntuned(pid, status);
        }
    }
    /* the group would do: offered at the first VM start of the run that
       lacks it, and only then - several VMs starting at once share the check
       that got here (start() waits for it), later ones do not ask again */
    if (status.needsGroup() && !m_groupSuggested && enabled() && untuned()) {
        m_groupSuggested = true;
        emit groupSetupSuggested(status);
    }
}

void HostSettings::setUntuned(qint64 pid, const Status &status)
{
    if (pid <= 0) {
        return;
    }
    /* the gone ones count no more, and their pids may come back as others */
    m_untuned.removeIf([pid](const auto &entry) { return entry.first != pid && !alive(entry.first); });
    const auto it = std::find_if(m_untuned.begin(), m_untuned.end(),
                                 [pid](const auto &entry) { return entry.first == pid; });

    if (status.active()) {
        if (it == m_untuned.end()) {
            return;
        }
        m_untuned.erase(it);
    } else if (it != m_untuned.end()) {
        if (it->second == status) {
            return;
        }
        it->second = status;
    } else {
        m_untuned.append({pid, status});
    }
    emit untunedChanged();
}

bool HostSettings::untuned() const
{
    return untunedCount() > 0;
}

int HostSettings::untunedCount() const
{
    if (!enabled()) {
        return 0;
    }
    return int(std::count_if(m_untuned.begin(), m_untuned.end(),
                             [](const auto &entry) { return alive(entry.first); }));
}

HostSettings::Status HostSettings::untunedStatus() const
{
    for (const auto &[pid, status] : m_untuned) {
        if (alive(pid)) {
            return status;
        }
    }
    return {};
}

void HostSettings::retune()
{
    if (!enabled()) {
        return;
    }
    /* pkexec refused earlier in the run: worth another try now */
    if (m_access == Access::Denied) {
        m_access = Access::Unknown;
    }
    const auto untuned = m_untuned;
    for (const auto &[pid, status] : untuned) {
        if (alive(pid)) {
            tune(pid);
        }
    }
}

void HostSettings::setUpGroup(const std::function<void(Setup, const QString &, const Status &)> &done)
{
    if (m_settingUp) {
        done(Setup::Failed, tr("the vitrine group is being set up already"), {});
        return;
    }
    if (!helperInstalled()) {
        done(Setup::Failed, tr("vitrine-helper is not installed"), {});
        return;
    }
    m_settingUp = true;
    auto *run = new QProcess(this);
    run->setProcessChannelMode(QProcess::MergedChannels);
    connect(run, &QProcess::finished, this, [this, run, done](int code, QProcess::ExitStatus exit) {
        const QStringList lines = QString::fromUtf8(run->readAll()).split('\n', Qt::SkipEmptyParts);
        bool ok = false;
        QString error;

        run->deleteLater();
        for (const QString &line : lines) {
            ok |= line.startsWith("ok setup-group: ");
            if (line.startsWith("error setup-group: ")) {
                error = line.section(':', 1).trimmed();
            } else if (error.isEmpty() && line.startsWith("Error executing command")) {
                /* pkexec's: not authorized, no polkit agent... */
                error = line.trimmed();
            }
        }
        if (exit == QProcess::NormalExit && code == 0 && ok) {
            /* polkit reads the group from the user database at each check */
            check(this, [this, done](const Status &now) {
                m_settingUp = false;
                if (now.active()) {
                    retune();
                } else {
                    /* what the VMs wait for now */
                    for (const auto &[pid, status] : QList(m_untuned)) {
                        if (status.needsGroup()) {
                            setUntuned(pid, now);
                        }
                    }
                }
                done(Setup::Done, QString(), now);
            });
            return;
        }
        m_settingUp = false;
        /* pkexec: 126 when the password dialog was dismissed, says its
           manual; KDE's agent gives "Not authorized" (127) for Cancel, as
           for a password that did not do */
        if (exit == QProcess::NormalExit &&
            ((error.isEmpty() && code == 126) ||
             (code == 127 && error.endsWith(": Not authorized")))) {
            done(Setup::Cancelled, QString(), {});
            return;
        }
        if (error.isEmpty()) {
            error = lines.isEmpty() ? tr("vitrine-helper failed") : lines.first().trimmed();
        }
        done(Setup::Failed, error, {});
    });
    connect(run, &QProcess::errorOccurred, this, [this, run, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            run->deleteLater();
            m_settingUp = false;
            done(Setup::Failed, tr("pkexec is not installed"), {});
        }
    });
    /* the installed helper, the one the action names; the desktop's polkit
       agent asks for the password, never a terminal */
    run->start("pkexec", {"--disable-internal-agent", VITRINE_HELPER_PATH, "setup-group"});
}

void HostSettings::say(const QString &text)
{
    if (!m_said.contains(text)) {
        m_said.insert(text);
        emit notice(text);
    }
}
