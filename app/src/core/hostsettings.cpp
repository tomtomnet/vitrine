// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostsettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
#include <optional>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "core/nesteddesktop.h"
#include "core/paths.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

extern char **environ;

#ifndef VITRINE_HELPER_PATH
#define VITRINE_HELPER_PATH "/usr/libexec/vitrine-helper"
#endif

/* polkit's action of the helper's session (its first argument) */
static const char kAction[] = "org.vitrine.helper";
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

bool HostSettings::cpuLatency()
{
    return settings().value("host/cpuLatency", true).toBool();
}

void HostSettings::setCpuLatency(bool on)
{
    settings().setValue("host/cpuLatency", on);
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

QStringList HostSettings::amdProfileCards(const QString &sysRoot)
{
    static const QRegularExpression name("^card[0-9]+$");
    const QDir drm(sysRoot + "/class/drm");
    QStringList cards;

    for (const QString &card : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString device = drm.filePath(card) + "/device/";
        QFile vendor(device + "vendor"), profiles(device + "pp_power_profile_mode"),
            metrics(device + "gpu_metrics");

        if (!name.match(card).hasMatch() || !vendor.open(QIODevice::ReadOnly) ||
            vendor.readAll().trimmed() != "0x1002" || !profiles.open(QIODevice::ReadOnly) ||
            !profiles.readAll().contains("3D_FULL_SCREEN")) {
            continue;
        }
        /* not an APU (gpu_metrics_v2_x or v3_x, as amdCards() tells) */
        const QByteArray header =
            metrics.open(QIODevice::ReadOnly) ? metrics.read(4) : QByteArray();
        if (header.size() >= 4 && (header[2] == 2 || header[2] == 3)) {
            continue;
        }
        cards << card;
    }
    return cards;
}

/* "Real-time QEMU threads are off: @why." */
static QString realtimeOffBecause(const QString &why)
{
    return HostSettings::tr("Real-time QEMU threads are off: %1.").arg(why);
}

/* The helper's reason ("kernel lockdown (integrity)"...), in plain words */
static QString realtimeOffFor(const QString &helperWhy)
{
    if (helperWhy.contains("lockdown")) {
        return realtimeOffBecause(
            HostSettings::tr("the kernel's lockdown (Secure Boot) blocks the fair server"));
    }
    if (helperWhy.contains("no fair server")) {
        return realtimeOffBecause(
            HostSettings::tr("this kernel has no fair server to keep them from starving the "
                             "host (Linux 6.12 and later have one)"));
    }
    if (helperWhy.contains("sched_ext")) {
        return realtimeOffBecause(
            HostSettings::tr("a sched_ext scheduler runs, and this kernel has no server to keep "
                             "them from starving its tasks (Linux 7.0 and later have one)"));
    }
    if (helperWhy.contains("debugfs is not mounted")) {
        return realtimeOffBecause(
            HostSettings::tr("debugfs, where the fair server is set, is not mounted"));
    }
    return realtimeOffBecause(
        HostSettings::tr("the fair server could not be set (%1)").arg(helperWhy));
}

QString HostSettings::realtimeLimit(const QString &sysRoot, const QString &kernelRelease)
{
    static const QRegularExpression version("^(\\d+)\\.(\\d+)");
    QString release = kernelRelease;
    if (release.isEmpty()) {
        struct utsname u;
        release = ::uname(&u) == 0 ? QString::fromLatin1(u.release) : QString();
    }
    const QRegularExpressionMatch m = version.match(release);
    /* major * 1000 + minor; 0: not known, not held against it */
    const int kernel = m.hasMatch() ? m.captured(1).toInt() * 1000 + m.captured(2).toInt() : 0;
    QFile lockdown(sysRoot + "/kernel/security/lockdown"), scx(sysRoot + "/kernel/sched_ext/state");
    const QByteArray level = lockdown.open(QIODevice::ReadOnly) ? lockdown.readAll() : QByteArray();
    const QByteArray state = scx.open(QIODevice::ReadOnly) ? scx.readAll().trimmed() : QByteArray();

    /* "none [integrity] confidentiality": the level in brackets */
    if (level.contains('[') && !level.contains("[none]")) {
        return realtimeOffFor("lockdown");
    }
    if (kernel && kernel < 6012) {
        return realtimeOffFor("no fair server");
    }
    if (!state.isEmpty() && state != "disabled" && kernel && kernel < 7000) {
        return realtimeOffFor("sched_ext");
    }
    return QString();
}

QString HostSettings::realtimeOff() const
{
    return m_realtimeKnown ? m_realtimeOff : realtimeLimit(m_sysRoot);
}

int HostSettings::onlineCpus(const QString &sysRoot)
{
    static const QRegularExpression range("^(\\d+)(?:-(\\d+))?$");
    static const QRegularExpression cpu("^cpu\\d+$");
    QFile f(sysRoot + "/devices/system/cpu/online");
    const QString list = f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed()
                                                     : QString();
    int n = 0;

    /* "0-3,8-11", as the helper reads it */
    for (const QString &part : list.split(',', Qt::SkipEmptyParts)) {
        const QRegularExpressionMatch m = range.match(part);
        const int first = m.captured(1).toInt();
        const int last = m.captured(2).isEmpty() ? first : m.captured(2).toInt();
        if (!m.hasMatch() || last < first) {
            n = 0;
            break;
        }
        n += last - first + 1;
    }
    if (n == 0) {
        for (const QString &entry :
             QDir(sysRoot + "/devices/system/cpu").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            n += cpu.match(entry).hasMatch();
        }
    }
    return qMax(n, 1);
}

/*
 * QEMU shows the VM in windows of its own: SDL's or GTK's, or the nested
 * desktop's own windows (QEMU's cross-domain is their Wayland client).
 * Which one has the keyboard is the desktop's to say.
 */
static bool ownWindows(const ArgsFile &args)
{
    return VmConfig::screen(args) == VmConfig::Screen::OwnWindow ||
           NestedDesktop::mode(args) == NestedDesktop::Mode::OwnWindow;
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

/* A thread of this process, still there: the view's threads end with it */
static bool threadAlive(qint64 tid)
{
    return tid > 0 && QFileInfo::exists(QString("/proc/self/task/%1").arg(tid));
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
void HostSettings::checkAction(const QString &action, QObject *context,
                               const std::function<void(const Status &)> &done)
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
    checkAction(QString::fromLatin1(kAction), context, done);
}

QStringList HostSettings::stripCapabilities(const QString &stack)
{
    QStringList stripped;

    /* the builds' folders, not `current`, a link to one of them */
    for (const QString &build : QDir(stack).entryList(QDir::Dirs | QDir::NoDotAndDotDot |
                                                      QDir::NoSymLinks)) {
        const QDir bin(stack + '/' + build + "/bin");
        for (const QString &name :
             bin.entryList({"qemu-system-*"}, QDir::Files | QDir::NoSymLinks)) {
            const QByteArray path = QFile::encodeName(bin.filePath(name));
            struct stat st;

            if (::getxattr(path.constData(), "security.capability", nullptr, 0) < 0 ||
                ::lstat(path.constData(), &st) < 0 || !S_ISREG(st.st_mode) ||
                st.st_uid != getuid()) {
                continue;
            }
            /* the kernel clears a file's capabilities when it changes owner,
               to the same one included; no write, so a VM may run from it */
            if (::chown(path.constData(), st.st_uid, st.st_gid) == 0 &&
                ::getxattr(path.constData(), "security.capability", nullptr, 0) < 0 &&
                errno == ENODATA) {
                stripped << bin.filePath(name);
            }
        }
    }
    return stripped;
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
        connect(store, &VmStore::removed, this, [this](const QString &id) {
            m_viewThreads.remove(id);
            setUntuned(m_tuned.take(id), {});
        });
    }
    /*
     * QEMU gets no privilege any more: the helper sets its threads'
     * scheduling.  The cap_sys_nice an older vitrine gave its builds goes,
     * with what came with it (no core dumps, no debugger, no
     * LD_LIBRARY_PATH): at the next start of each VM.
     */
    for (const QString &qemu : stripCapabilities(Paths::stackDir())) {
        qInfo("vitrine: cap_sys_nice removed from %s", qPrintable(qemu));
    }
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
        const qint64 ended = m_tuned.take(vm->id());
        m_udmabuf.remove(ended);
        setUntuned(ended, {});
        qemuGone(ended);
        updateWindowFocus();
    } else if ((state == VmRunner::State::Running || state == VmRunner::State::Paused) &&
               pid > 0 && m_tuned.value(vm->id()) != pid) {
        /* a new run, started here or found running: tuned once */
        m_tuned[vm->id()] = pid;
        tune(pid, VmConfig::graphics(vm->runner()->runArgs()).udmabuf());
        updateWindowFocus();
    }
}

void HostSettings::tune(qint64 pid, bool udmabuf)
{
    if (udmabuf) {
        m_udmabuf.insert(pid);
    }
    if (!enabled()) {
        /* turned off meanwhile: the helper lets go of everything now */
        if (m_fd >= 0) {
            m_out += "release\n";
            flush();
        }
        if (m_udmabuf.contains(pid)) {
            emit udmabufAnswered(pid, false, tr("host tuning is off"));
        }
        return;
    }
    if (m_access == Access::Denied) {
        if (alive(pid)) {
            setUntuned(pid, m_denied);
        }
        return;
    }
    queue(pid);
    start();
}

void HostSettings::queue(qint64 pid)
{
    const QString n = QString::number(pid);
    const QString floor = gpuFloor();

    m_expected.insert(pid);
    /* huge pages for its memfd RAM, as for every VM's */
    m_out += ("watch " + n + "\nfair-server on\nthp on\n").toLatin1();
    /* the CPU ready to wake up; off lets go of what the helper holds (the
       preference turned off while VMs run) */
    m_out += cpuLatency() ? "cpu-latency on\nepp on\n" : "cpu-latency off\nepp off\n";
    /* auto: on APUs only (measured on a Radeon 780M), none on the other
       cards, where a fixed one chosen before goes; off: let a floor go */
    const QStringList apus = amdCards(m_sysRoot, true);
    for (const QString &card : amdCards(m_sysRoot, false)) {
        const QString value = floor != "auto" ? floor : apus.contains(card) ? "auto" : "off";
        m_out += QString("gpu-floor %1 %2\n").arg(card, value).toLatin1();
    }
    /* the dedicated AMD GPUs' 3D_FULL_SCREEN power profile, with the
       floor's Automatic too (measured on an RX 6900 XT); Off or a fixed
       floor: none */
    for (const QString &card : amdProfileCards(m_sysRoot)) {
        m_out += QString("gpu-profile %1 %2\n")
                     .arg(card, floor == "auto" ? "on" : "off")
                     .toLatin1();
    }
    if (m_udmabuf.contains(pid)) {
        m_out += ("udmabuf " + n + '\n').toLatin1();
        m_udmabufAsked << pid;
    }
    /*
     * Real-time in front, ordinary behind it.  In front: this VM, when no
     * VM is (the first one; QEMUs found running), or when its window is the
     * active one already - QEMU's own window shows before its VM is tuned.
     * A VM whose run ended is no longer in front (qemuGone()), and the
     * QEMU in front may be waiting for its turn here (a helper started
     * again): alive is what counts.
     */
    if (pid != m_frontPid && (m_frontPid <= 0 || !alive(m_frontPid) || pid == m_activePid)) {
        m_frontPid = pid;
    }
    if (pid == m_frontPid) {
        sendFront(pid);
    } else {
        m_out += ("behind " + n + '\n').toLatin1();
    }
}

void HostSettings::preferencesChanged()
{
    if (!enabled()) {
        /* off: everything back now, not after the last VM, and nothing
           queued for a helper still starting (its check or its "ready") */
        m_out.clear();
        m_expected.clear();
        m_udmabufAsked.clear();
        /* the udmabuf limits go back too, for the VMs that need them */
        for (const qint64 pid : QSet(m_udmabuf)) {
            if (alive(pid)) {
                emit udmabufAnswered(pid, false, tr("host tuning is off"));
            } else {
                m_udmabuf.remove(pid);
            }
        }
        if (m_fd >= 0) {
            m_out = "release\n";
            flush();
        }
        /* focus priority with it: the helper's release put the threads back,
           the view's too */
        m_frontPid = 0;
        m_viewAsked.clear();
        updateWindowFocus();
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
    const qint64 pid = vmId.isEmpty() ? 0 : m_tuned.value(vmId);

    if (pid > 0 && enabled()) {
        setFrontPid(pid);
    }
}

void HostSettings::setViewThreads(const QString &vmId, const QList<qint64> &tids)
{
    if (tids.isEmpty()) {
        m_viewThreads.remove(vmId);
    } else {
        m_viewThreads.insert(vmId, tids);
    }
    /* the VM in front's: to its helper, running or starting */
    if (m_frontPid > 0 && !vmId.isEmpty() && m_tuned.value(vmId) == m_frontPid) {
        sendView();
        flush();
    }
}

void HostSettings::windowActivated(qint64 pid)
{
    m_activePid = pid;
    /* a VM's QEMU; vitrine's own windows say it through setFront(), the
       host's others change nothing */
    if (pid > 0 && enabled() &&
        (m_expected.contains(pid) || std::find(m_tuned.cbegin(), m_tuned.cend(), pid) != m_tuned.cend())) {
        setFrontPid(pid);
    }
}

void HostSettings::setFrontPid(qint64 pid)
{
    if (pid == m_frontPid) {
        return;
    }
    m_frontPid = pid;
    sendFront(pid);
    flush();
}

void HostSettings::sendFront(qint64 pid)
{
    /*
     * Never two VMs real-time at once: every other QEMU watched behind
     * first - in QEMU's own window too, whose focus may not be known here -
     * then this one real-time, if it is watched (else when queue() watches
     * it).  The requests go after those of a helper still starting, if it
     * is: they watch these QEMUs first.
     */
    for (const qint64 other : std::as_const(m_expected)) {
        /* not one whose end the helper is about to say */
        if (other != pid && alive(other)) {
            m_out += "behind " + QByteArray::number(other) + '\n';
        }
    }
    /* the view of the VM in front before went back with it: the helper puts
       a view's threads back once no VM is in front, at that behind or as
       that QEMU ended */
    m_viewAsked.clear();
    if (m_expected.contains(pid)) {
        m_out += "rt " + QByteArray::number(pid) + '\n';
        sendView();
    }
}

void HostSettings::sendView()
{
    QList<qint64> want;

    /* the view of the VM in front, to the helper that watches its QEMU (the
       helper refuses them while none is in front, or without the bound) */
    if (enabled() && m_frontPid > 0 && m_expected.contains(m_frontPid)) {
        for (auto it = m_tuned.cbegin(); it != m_tuned.cend(); ++it) {
            if (it.value() == m_frontPid) {
                want = m_viewThreads.value(it.key());
            }
        }
    }
    /* threads that ended are no longer the helper's to put back */
    m_viewAsked.removeIf([](qint64 tid) { return !threadAlive(tid); });
    /* one still there that is no longer the view's (its view gone: GDBus's
       worker, which stays): all back, the others again */
    if (std::any_of(m_viewAsked.cbegin(), m_viewAsked.cend(),
                    [&want](qint64 tid) { return !want.contains(tid); })) {
        m_out += "view-behind\n";
        m_viewAsked.clear();
    }
    for (const qint64 tid : std::as_const(want)) {
        if (!m_viewAsked.contains(tid) && threadAlive(tid)) {
            m_out += "view-rt " + QByteArray::number(tid) + '\n';
            m_viewAsked.insert(tid);
        }
    }
}

void HostSettings::qemuGone(qint64 pid)
{
    QList<qint64> left;

    if (pid > 0 && pid == m_frontPid) {
        m_frontPid = 0;
        /* its view's threads: the helper put them back as it ended */
        m_viewAsked.clear();
    }
    if (m_frontPid > 0 && alive(m_frontPid)) {
        return;
    }
    /* none in front, and a VM left alone: real-time, as a single VM always is */
    for (const qint64 other : std::as_const(m_expected)) {
        if (other != pid && alive(other)) {
            left << other;
        }
    }
    if (left.size() == 1) {
        setFrontPid(left.first());
    }
}

bool HostSettings::wantsWindowFocus() const
{
    if (!enabled() || !m_store) {
        return false;
    }
    for (Vm *vm : m_store->vms()) {
        if (m_tuned.contains(vm->id()) && ownWindows(vm->runner()->runArgs())) {
            return true;
        }
    }
    return false;
}

void HostSettings::updateWindowFocus()
{
    const bool wanted = wantsWindowFocus();

    if (wanted != m_windowFocus) {
        m_windowFocus = wanted;
        /* not followed now: what it said last grows stale */
        if (!wanted) {
            m_activePid = 0;
        }
        emit windowFocusWanted(wanted);
    }
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
        /* the group joined since vitrine started, say: the VMs that ran
           untuned until now too */
        for (const auto &entry : QList(m_untuned)) {
            if (!m_expected.contains(entry.first) && alive(entry.first)) {
                queue(entry.first);
            }
        }
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
    static const QRegularExpression capped(
        "^skip rt (\\d+): (\\d+) vCPUs, more than half of the (\\d+) online cpus");
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
            setUntuned(pid, {Problem::Failed,
                             tr("vitrine-helper does not take %1: %2")
                                 .arg(vmName(pid), line.section(':', 1).trimmed())});
        }
        m_expected.remove(pid);
        /* no longer in front; a VM left alone comes to the front */
        qemuGone(pid);
        if (m_expected.isEmpty() && m_out.isEmpty() && m_fd >= 0) {
            /* nothing left to watch: it ends, putting everything back */
            ::shutdown(m_fd, SHUT_WR);
        }
    } else if (line.startsWith("ok udmabuf ") || line.startsWith("skip udmabuf ") ||
               line.startsWith("error udmabuf")) {
        udmabufLine(line);
    } else if (word == "error" && (line.endsWith(": watch a QEMU first") ||
                                   line.endsWith(": watch the process first"))) {
        /* after a watch that failed: said already, if worth it */
    } else if (line.startsWith("ok fair-server on") || line.startsWith("skip fair-server") ||
               line.startsWith("error fair-server")) {
        fairServerLine(line);
    } else if (line == "error view-rt: unknown request" ||
               line == "error view-behind: unknown request") {
        /* a helper installed before vitrine raised its view's threads: the
           VMs are tuned as before */
        say(tr("Host tuning: the installed vitrine-helper is older than Vitrine and cannot make "
               "its view of the VM in front real-time: install it again."));
    } else if (line.startsWith("skip view-rt ") || line.startsWith("ok view-")) {
        /* skipped: no bound (its line said why), or no VM in front any more */
    } else if (line.startsWith("error view-rt ")) {
        /* "error view-rt TID: why": a thread that ended meanwhile is no news */
        if (threadAlive(line.section(' ', 2, 2).section(':', 0, 0).toLongLong())) {
            say(tr("Host tuning: the threads of the VM view stay ordinary: %1.")
                    .arg(line.section(':', 1).trimmed()));
        }
    } else if (const QRegularExpressionMatch m = capped.match(line); m.hasMatch()) {
        /* more processors than half of the CPUs: the helper lowered it as
           a VM behind, in front too */
        say(tr("Host tuning: %1 has %2 processors, more than half of this computer's %3: it runs "
               "without real-time threads, even in front.")
                .arg(vmName(m.captured(1).toLongLong()), m.captured(2), m.captured(3)));
    } else if (line.startsWith("skip rt ")) {
        /* no real-time threads without the fair server: its line said why */
    } else if ((line.startsWith("error rt ") || line.startsWith("error behind ")) &&
               !alive(line.section(' ', 2, 2).section(':', 0, 0).toLongLong())) {
        /* a QEMU that ended as it was asked: no news, its "exited" follows */
    } else if (line == "error behind: unknown request") {
        /* a helper installed before vitrine sent the VMs behind through it */
        say(tr("Host tuning: the installed vitrine-helper is older than Vitrine and cannot "
               "make the threads of the VMs behind ordinary: install it again."));
    } else if (line == "error thp: unknown request") {
        /* a helper installed before vitrine asked for huge pages */
        say(tr("Host tuning: the installed vitrine-helper is older than Vitrine and cannot "
               "turn on huge pages for VM memory: install it again."));
    } else if (line.startsWith("skip thp: ") || line.startsWith("error thp: ")) {
        /* "skip thp: this kernel has no transparent huge pages" */
        say(tr("Host tuning: no huge pages for VM memory: %1.").arg(line.section(':', 1).trimmed()));
    } else if (line == "error cpu-latency: unknown request" || line == "error epp: unknown request") {
        /* a helper installed before vitrine kept the CPU ready to wake up:
           worth saying only when asked for (off is sent too) */
        if (cpuLatency()) {
            say(tr("Host tuning: the installed vitrine-helper is older than Vitrine and cannot "
                   "keep the CPU ready to wake up while VMs run: install it again."));
        }
    } else if (line.startsWith("skip cpu-latency: ") || line.startsWith("error cpu-latency: ")) {
        /* "skip cpu-latency: this kernel has no /dev/cpu_dma_latency" */
        say(tr("Host tuning: the CPU is not kept out of its deeper idle states: %1.")
                .arg(line.section(':', 1).trimmed()));
    } else if (line == "error gpu-profile: unknown request") {
        /* a helper installed before vitrine set the power profile: worth
           saying only when asked for (off is sent too) */
        if (gpuFloor() == "auto") {
            say(tr("Host tuning: the installed vitrine-helper is older than Vitrine and cannot "
                   "set the GPU's power profile while VMs run: install it again."));
        }
    } else if (line.startsWith("skip epp: ") || line.startsWith("error epp: ")) {
        /* "skip epp: acpi-cpufreq has no energy performance preference (...)" */
        say(tr("Host tuning: the CPU's energy performance preference stays as it is: %1.")
                .arg(line.section(':', 1).trimmed()));
    } else if (word == "skip" || word == "error") {
        /* "skip gpu-floor card1: no overdrive clock table (pp_od_clk_voltage)" */
        say(tr("Host tuning: %1").arg(line.section(' ', 1)));
    }
}

QString HostSettings::vmName(qint64 pid) const
{
    for (Vm *vm : m_store ? m_store->vms() : QList<Vm *>()) {
        if (m_tuned.value(vm->id()) == pid) {
            return vm->name();
        }
    }
    return tr("process %1").arg(pid);
}

void HostSettings::udmabufLine(const QString &line)
{
    /* "ok udmabuf PID: ...", "skip udmabuf PID: why", "error udmabuf PID: why" */
    const QString word = line.section(' ', 0, 0);
    bool numbered = false;
    const qint64 pid = line.section(' ', 2, 2).section(':', 0, 0).toLongLong(&numbered);

    if (numbered) {
        m_udmabufAsked.removeOne(pid);
        if (m_udmabuf.contains(pid)) {
            emit udmabufAnswered(pid, word == "ok", word == "ok" ? QString()
                                                                 : line.section(':', 1).trimmed());
        }
        return;
    }
    /* without a pid: the answer to the oldest request */
    const qint64 oldest = m_udmabufAsked.isEmpty() ? 0 : m_udmabufAsked.takeFirst();
    if (line.endsWith(": unknown request") && m_udmabuf.contains(oldest)) {
        /* a helper installed before vitrine knew of udmabuf */
        emit udmabufAnswered(oldest, false,
                             tr("the installed vitrine-helper is older than Vitrine and cannot "
                                "raise them: install it again"));
    }
    /* "watch the process first", "watch a QEMU first": its watch failed,
       which leaves it untuned (setUntuned() answers) */
}

void HostSettings::fairServerLine(const QString &line)
{
    /* "ok fair-server on: ...", "skip fair-server: why", "error fair-server: why" */
    m_realtimeKnown = true;
    m_realtimeOff = line.startsWith("ok ") ? QString()
                                           : realtimeOffFor(line.section(':', 1).trimmed());
    if (!m_realtimeOff.isEmpty()) {
        say(m_realtimeOff);
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
        if (line.startsWith(kPkexecError)) {
            why = line;
            break;
        }
    }
    /* a reason in a sentence: "...: No authentication agent found." */
    why = clause(why);
    m_pid = 0;
    closeHelper();
    m_out.clear();
    m_expected.clear();
    m_udmabufAsked.clear();
    /* it put the view's threads back as it ended */
    m_viewAsked.clear();
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
    m_udmabufAsked.clear();
    m_viewAsked.clear();
    for (qint64 pid : std::as_const(pids)) {
        if (alive(pid)) {
            setUntuned(pid, status);
        }
    }
    /* the VMs untuned before: the state is what it is now for them too (the
       helper installed since, say) */
    restate(status);
    /* the group would do: offered at the first VM start of the run that
       lacks it, and only then - several VMs starting at once share the check
       that got here (start() waits for it), later ones do not ask again */
    offerGroup(status);
}

void HostSettings::restate(const Status &now)
{
    using enum Problem;

    /* Failed: this VM's own (the helper would not take it), or this start's */
    if (now.active() || now.problem == Failed) {
        return;
    }
    for (const auto &[pid, status] : QList(m_untuned)) {
        if (status.problem != Failed && alive(pid)) {
            setUntuned(pid, now);
        }
    }
}

void HostSettings::offerGroup(const Status &status)
{
    if (status.needsGroup() && !m_groupSuggested && !m_settingUp && enabled() && untuned()) {
        m_groupSuggested = true;
        emit groupSetupSuggested(status);
    }
}

void HostSettings::recheck(QObject *context, const std::function<void(const Status &)> &done)
{
    const bool answer = context && done;
    QPointer<QObject> guard(context);

    if (!answer && (m_rechecks > 0 || !enabled() || !untuned())) {
        return;
    }
    m_rechecks++;
    check(this, [this, answer, guard, done](const Status &now) {
        m_rechecks--;
        /* a caller that shows the state offers Set Up itself: not a
           question on top of its box, nor one when it closes */
        apply(now, !answer);
        if (answer && now.needsGroup() && enabled() && untuned()) {
            m_groupSuggested = true;
        }
        if (answer && guard) {
            done(now);
        }
    });
}

void HostSettings::apply(const Status &now, bool offer)
{
    /* a VM start's check, or Set Up's, does the same with its own answer */
    if (!enabled() || !untuned() || m_access == Access::Checking || m_settingUp) {
        return;
    }
    if (now.active()) {
        retune();
        return;
    }
    restate(now);
    if (offer) {
        offerGroup(now);
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
    if (!status.active() && m_udmabuf.contains(pid)) {
        emit udmabufAnswered(pid, false, status.why());
    }
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
    std::optional<Status> first;

    /* one Set Up fixes before a VM's own reason: what the warning offers */
    for (const auto &[pid, status] : m_untuned) {
        if (alive(pid)) {
            if (status.needsGroup()) {
                return status;
            }
            if (!first) {
                first = status;
            }
        }
    }
    return first.value_or(Status());
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
    /* offered: the VM start's question would come on top of this one */
    m_groupSuggested = true;
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
                error = clause(line.section(':', 1));
            } else if (error.isEmpty() && line.startsWith(kPkexecError)) {
                /* pkexec's: not authorized, no polkit agent... */
                error = clause(line.mid(int(strlen(kPkexecError))));
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
                    restate(now);
                }
                done(Setup::Done, QString(), now);
            });
            return;
        }
        m_settingUp = false;
        /* pkexec: 126 when the password dialog was dismissed ("Request
           dismissed", GNOME Shell's agent); KDE's agent gives "Not
           authorized" (127) for Cancel, as for a password that did not do.
           The helper never exits with 126. */
        if (exit == QProcess::NormalExit &&
            (code == 126 || (code == 127 && error == "Not authorized"))) {
            done(Setup::Cancelled, QString(), {});
            return;
        }
        if (error.isEmpty()) {
            error = lines.isEmpty() ? tr("vitrine-helper failed") : clause(lines.first());
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
