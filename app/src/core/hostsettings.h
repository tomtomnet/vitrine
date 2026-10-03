// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>

#include <sys/types.h>

class QSocketNotifier;
class QTimer;
class Vm;
class VmStore;

/*
 * The host settings that keep VMs smooth, for as long as they run: the
 * kernel's fair server at 10 ms / 1 ms, a GPU clock floor on AMD APUs and
 * real-time QEMU threads.  They need root: vitrine-helper applies them,
 * started with pkexec when a VM starts.  Members of the vitrine group need
 * no password; polkit is asked first, without interaction, each time the
 * helper starts (a membership given or taken meanwhile counts), so that
 * everyone else gets their VMs untuned and a word in the status bar once,
 * never a password dialog at each start.  The helper watches the QEMU
 * processes and puts everything back after the last one, crash included,
 * even after vitrine has quit (VMs outlive it).  See docs/host-tuning.md.
 */
class HostSettings : public QObject
{
    Q_OBJECT

public:
    /* Tunes for the VMs of @store as they start or are found running, and
       gives each new build of vitrine's QEMU its capability */
    explicit HostSettings(VmStore *store, QObject *parent = nullptr);
    /* Lets the helper go on alone: it keeps the settings while VMs run */
    ~HostSettings() override;

    /* Preferences: tuning, on by default */
    static bool enabled();
    static void setEnabled(bool on);
    /* "auto" (the default: 1800 MHz on AMD APUs), "off" or a clock in MHz */
    static QString gpuFloor();
    static void setGpuFloor(const QString &floor);

    /* The installed helper, the one polkit's action names and the only one
       pkexec runs; $VITRINE_HELPER names another for helperInstalled() (tests) */
    static QString helperPath();
    static bool helperInstalled();
    /* The calling user is in the vitrine group (the user database's view,
       as polkit's) */
    static bool inVitrineGroup();
    /*
     * The AMD cards the helper can set a clock floor on (card1...) under
     * @sysRoot: those with amdgpu's overdrive table, and only APUs if @apus
     */
    static QStringList amdCards(const QString &sysRoot, bool apus);

    /*
     * cap_sys_nice=ep on @qemu, a QEMU of vitrine's stack, through the
     * helper: QEMU may then make its vCPUs real-time itself and get
     * high-priority amdgpu contexts.  Only without a password (members of
     * the vitrine group).  @done gets an empty string, or why not.
     */
    static void grantCapability(const QString &qemu, QObject *context,
                                const std::function<void(const QString &error)> &done);

    /* What a VM start does, for the QEMU @pid */
    void tune(qint64 pid);

    /*
     * Focus priority: the vCPUs of the VM in front - its console or
     * full-screen window has the focus - real-time, those of the other VMs
     * ordinary (nice -5), so that VMs busy in the background cannot take
     * every CPU at real-time priority.  A host window in front changes
     * nothing: the VM last in front keeps them.  Through QEMU's own
     * x-vcpu-priority, or the helper's rt where QEMU lacks CAP_SYS_NICE.
     */
    void setFront(const QString &vmId);
    /* The app's (MainWindow makes it), for setFront(); null before */
    static HostSettings *instance();

    /* Tests: run @command instead of pkexec <helper>, without asking polkit */
    void setHelperCommand(const QStringList &command) { m_command = command; }
    /* Tests: where /sys is */
    void setSysRoot(const QString &root) { m_sysRoot = root; }
    bool helperRunning() const { return m_pid > 0; }

signals:
    /*
     * For the status bar: why the host is not tuned, or a setting the
     * helper could not apply here; each text once per run of vitrine
     */
    void notice(const QString &text);
    /* Each line the helper writes */
    void helperLine(const QString &line);
    /* The helper ended (after the last VM it watched, or refused) */
    void helperFinished();

private:
    /* polkit's answer is not kept: Denied only when pkexec itself refused */
    enum class Access { Unknown, Checking, Denied };

    void watchVm(Vm *vm);
    void vmStateChanged(Vm *vm);
    void start();
    void spawn(const QStringList &command);
    void readHelper();
    void handleLine(const QString &line);
    void flush();
    void reap();
    void closeHelper();
    /* No helper for the VMs asked for: for this start only, or (@always)
       for the rest of the run */
    void deny(const QString &why, bool always = false);
    void say(const QString &text);

    VmStore *m_store;
    QStringList m_command;
    QString m_sysRoot = QStringLiteral("/sys");
    QHash<QString, qint64> m_tuned;     // VM id -> its QEMU's pid
    QString m_front;                    // the VM last in front
    QSet<qint64> m_expected;            // QEMUs asked to be watched, not ended
    Access m_access = Access::Unknown;
    QSet<QString> m_said;
    QSet<QString> m_refusals;           // said by deny()
    int m_restarts = 0;

    /* the helper: its stdin, stdout and stderr are one end of a socket pair */
    pid_t m_pid = 0;
    int m_fd = -1;
    int m_pidfd = -1;
    QSocketNotifier *m_readable = nullptr;
    QSocketNotifier *m_writable = nullptr;
    QSocketNotifier *m_exited = nullptr;
    QTimer *m_exitPoll = nullptr;
    QByteArray m_in;
    QByteArray m_out;                   // requests not sent yet
    bool m_ready = false;
    QStringList m_foreign;              // lines not of the protocol: pkexec's
};
