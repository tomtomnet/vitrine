// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <optional>

#include "core/argsfile.h"

class GpuContexts;
class QLocalSocket;
class QTimer;
class Vm;

/*
 * The guest tools, as VMware Tools are to VMware: what a Fedora guest needs
 * to run well in vitrine (the patched virtio-gpu driver, Mesa with native
 * context, vitrine's KWin, their settings, and an agent), from a medium
 * that guest/build-rpms.sh and guest/build-medium.sh make: a FAT image with
 * a dnf repository and the installer.
 *
 * They get into a guest the first time at its next start: the runner then
 * attaches the medium and passes the guest's systemd (256 and later) a unit
 * through SMBIOS credentials, which installs them before the desktop starts
 * and restarts the guest.  Or by hand: sudo bash .../VITRINETOOL/install.
 * Afterwards the agent, on a virtio-serial port every VM gets, says how the
 * guest is doing: tools and package versions, the driver per kernel.
 */
namespace GuestTools {

/* The medium's FAT label (11 characters at most), which the guest mounts it by */
extern const char kLabel[];
/* The agent's virtio-serial port, as the guest names it */
extern const char kPortName[];
/* Its QEMU id: VSERPORT_CHANGE tells when the guest opens and closes it */
extern const char kPortId[];
/* The Fedora release the tools are built for */
extern const char kFedoraRelease[];

/* A medium build-medium.sh made, as its manifest describes it */
struct Medium {
    QString image;          // empty: none built
    QString mediumId;       // changes with what it installs
    QString fedora;         // the release it is for
    QString tools;          // vitrine-guest-tools' version-release
    QString mesa;           // version-release, empty if none on it
    QString kwin;
    QStringList packages;   // name-version-release.arch
    bool isValid() const { return !image.isEmpty(); }
};

/* ~/.local/share/vitrine/guest-tools: build-medium.sh's output */
QString dataDir();
/* The medium for @release in dataDir(), if built */
Medium medium(const QString &release = kFedoraRelease);
/* The manifest (the .json beside the image), for the image @image */
Medium parseManifest(const QByteArray &json, const QString &image);

/* What the runner adds to a VM's command line */
/* Its virtio-serial controller, for the agents' ports: on x86-64 and ARM
   machines with PCI */
bool hasSerialController(const ArgsFile &args, const QString &qemu);
/* The agent's port: there, unless the VM has it */
bool addsAgentPort(const ArgsFile &args, const QString &qemu);
/* The port, on the runner's virtio-serial controller vitrine-serial */
QStringList agentPortArgs(const QString &socket);
/* The medium, as a read-only virtio disk */
QStringList mediumArgs(const QString &image);
/* The unit that installs the tools at boot, and the multi-user.target
   drop-in that starts it, as SMBIOS credentials for the guest's systemd */
QStringList bootstrapArgs();
QByteArray bootstrapUnit();
QByteArray bootstrapDropIn();
/* The credentials can reach the guest: targets with SMBIOS type 11 */
bool canBootstrap(const ArgsFile &args, const QString &qemu);

/* What the next start of a VM brings, until it starts */
enum class Pending {
    None,
    Bootstrap,      // the medium and the bootstrap: installs at boot
    Medium,         // the medium only: the user installs by hand
};
Pending pending(const QString &vmId);
void setPending(const QString &vmId, Pending pending);

/* What the agent reports (its status object) */
struct Kernel {
    QString version;
    bool headers = false;
    QString driver;         // DKMS: installed, built, missing, no-headers
};
struct Report {
    QString tools;          // vitrine-guest-tools' version-release; empty if not installed
    QString agent;
    QString osId, osVersion, osName;
    QString kernel;
    QList<Kernel> kernels;
    std::optional<bool> secureBoot;     // none without UEFI
    QHash<QString, QString> desktops;   // kde: 6.7.5
    bool driverPresent = true;          // a virtio GPU in the VM (older agents: assumed)
    bool driverLoaded = false;
    bool driverPatched = false;
    QString taint;
    QHash<QString, QString> driverParams;
    QStringList capsets;                // offered by the host: virgl, venus, drm...
    QHash<QString, QString> packages;   // name: version-release
    bool kwinPatched = false;
    QString preempt;
    bool rebootNeeded = false;
    bool installing = false;
    QString installedMedium;            // the medium id of the last install
    /* The installer's last run, recorded in the guest: the host does not hear
       the bootstrap's when the installed agent holds the port */
    QString lastMedium;
    std::optional<bool> lastOk;
    QString lastError;
    /* The install vitrine asked for at this boot: waiting, running, done,
       failed; empty when none was asked for (or an older agent) */
    QString bootstrap;
};
Report parseReport(const QJsonObject &status);

/* A line of the agent */
struct Message {
    enum class Type { Invalid, Hello, Status, Progress, Result, Error };
    Type type = Type::Invalid;
    qint64 id = -1;
    int protocol = 0;
    Report report;          // Hello, Status
    QString step;           // Progress
    int current = 0;
    int total = 0;
    QString command;        // Result
    bool ok = false;
    QString error;          // Result, Error
    bool rebootNeeded = false;
};
Message parseMessage(const QByteArray &line);
/* A command line for the agent: status, install-from-medium, request-reboot, shutdown */
QByteArray commandLine(const QString &command, qint64 id);

/* How the guest is doing, for the banner (MesaNotActive comes from QEMU,
   not evaluate()) */
enum class State {
    Unknown,            // not running, or not told yet
    NotInstalled,       // no agent answered
    Pending,            // installs at the next start
    Installing,
    Failed,             // the last install failed
    RebootNeeded,
    DriverNotActive,    // tools in, the stock virtio-gpu driver runs
    MesaNotActive,      // tools in, the guest draws through virgl (from QEMU's counts)
    UpdateAvailable,
    Installed,
    Unsupported,        // a guest the tools are not for
};
struct Inputs {
    bool running = false;
    Pending pending = Pending::None;
    bool bootstrapRun = false;      // this run started with the bootstrap
    bool bootstrapExpired = false;  // ... long ago: an older agent did not say
    bool agentSeen = false;         // a hello since the guest booted
    bool waited = false;            // long enough without one
    bool installing = false;
    bool failed = false;
    Report report;
    Medium medium;
    /* What the last run of the VM showed, for while it is off: NotInstalled,
       Installed (with the tools' version-release), or Unknown */
    State remembered = State::Unknown;
    QString rememberedTools;
};
State evaluate(const Inputs &in);
/* Why the stock driver runs, from the report */
QString driverProblem(const Report &report);
/* The installer's error from its output (the agent's result) or its record */
QString failureReason(const QString &output);

}

/*
 * The guest tools of one VM while it runs: connects to its agent's socket,
 * keeps the agent's report, and tells the state.
 */
class GuestToolsMonitor : public QObject
{
    Q_OBJECT

public:
    /* The one of @vm, made on first use (a child of it) */
    static GuestToolsMonitor *of(Vm *vm);

    GuestTools::State state() const;
    GuestTools::Inputs inputs() const { return m_in; }
    const GuestTools::Report &report() const { return m_in.report; }
    /* The agent answered since the guest booted */
    bool hasAgent() const { return m_in.agentSeen; }
    /* While installing: the installer's step */
    QString step() const { return m_step; }
    int stepCurrent() const { return m_current; }
    int stepTotal() const { return m_total; }
    /* Why the last install failed */
    QString error() const { return m_error; }
    /* The banner's text (rich text) for the state, empty when nothing to say */
    QString text() const;

    void requestStatus();
    /* The agent installs from the medium attached to the VM */
    void installFromMedium();
    /* The agent restarts the guest */
    void requestReboot();
    /* The next start brings the medium and the bootstrap, or the medium only */
    void setPending(GuestTools::Pending pending);

signals:
    void changed();
    void installFinished(bool ok, const QString &error, bool rebootNeeded);
    /* The agent's answer to a command: install-from-medium, request-reboot, shutdown */
    void commandFinished(const QString &command, bool ok, const QString &error);

private:
    explicit GuestToolsMonitor(Vm *vm);
    void runnerChanged();
    void connectSocket();
    void read();
    void handle(const GuestTools::Message &m);
    void qmpEvent(const QString &name, const QJsonObject &data);
    void guestRestarted();
    void send(const QString &command);
    /* The runner's shutdown handler: the agent powers the guest off at once;
       @answer says whether it took the request */
    bool shutDownThroughAgent(const std::function<void(bool took)> &answer);
    void answerShutdown(bool took);
    /* What this run showed, for while the VM is off */
    void remember();

    Vm *m_vm;
    QLocalSocket *m_socket;
    QTimer *m_retry;
    QTimer *m_grace;
    /* the guest's 3D contexts in QEMU (x-drm-contexts, x-virgl-contexts) */
    GpuContexts *m_contexts;
    QTimer *m_poll;
    GuestTools::Inputs m_in;
    /* this run started here, with what was pending then */
    bool m_started = false;
    GuestTools::Pending m_startPending = GuestTools::Pending::None;
    QByteArray m_buffer;
    QString m_step;
    int m_current = 0;
    int m_total = 0;
    QString m_error;
    qint64 m_nextId = 1;
    int m_polls = 0;
    /* the next way, if the agent does not answer the shutdown */
    QTimer *m_shutdownFallback;
    std::function<void(bool took)> m_shutdownAnswer;
    /* an install asked for in this run (setPending()), over the tools the
       guest has: a report of those does not take it back */
    bool m_pendingAsked = false;
    /* how long an older agent's report is taken as before the bootstrap */
    QTimer *m_bootstrapTimer;
};
