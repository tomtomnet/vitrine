// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QPair>
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
 * everyone else gets their VMs untuned, never a password dialog at each
 * start: the status bar shows that VMs run untuned and why (untuned()), and
 * the first such start of a run offers to set up the group once
 * (groupSetupSuggested(), setUpGroup()).  The helper watches the QEMU
 * processes and puts everything back after the last one, crash included,
 * even after vitrine has quit (VMs outlive it).  See docs/host-tuning.md.
 */
class HostSettings : public QObject
{
    Q_OBJECT

public:
    /* Why tuning is not active, as far as can be told without asking anyone */
    enum class Problem {
        None,           // active: polkit lets the helper run without a password
        NotInstalled,   // no vitrine-helper where it is installed
        NoPolkit,       // no pkcheck
        NoPolicy,       // the helper's polkit action is not installed
        NoGroup,        // polkit wants a password, and there is no vitrine group
        NotMember,      // polkit wants a password: not in the vitrine group
        NotLocal,       // a member, and polkit still wants a password
        Failed,         // the helper did not run, or did not take a VM: see detail
    };
    struct Status {
        Problem problem = Problem::None;
        QString detail;     // Failed: why
        bool active() const { return problem == Problem::None; }
        /* The vitrine group, set up by setUpGroup(), is what it takes */
        bool needsGroup() const
        {
            return problem == Problem::NoGroup || problem == Problem::NotMember;
        }
        /* Why it is not active, plain text without a period: "" if it is */
        QString why() const;
        bool operator==(const Status &) const = default;
    };
    /*
     * What polkit's answer means: pkcheck's exit status (-1: it did not
     * start) and error output, with the helper installed or not, and the
     * vitrine group's existence and the user's membership
     */
    static Status classify(bool installed, int pkcheckStatus, const QString &pkcheckError,
                           bool groupExists, bool member);
    /*
     * The state now, without interaction (pkcheck, the user database): @done
     * gets it in @context's thread, later or at once
     */
    static void check(QObject *context, const std::function<void(const Status &)> &done);

    /* Tunes for the VMs of @store as they start or are found running, and
       gives vitrine's QEMU its capability: each new build, and one that
       lacks it (built before the group was joined, or with tuning off) */
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
    /* The vitrine group exists, and the calling user is in it (the user
       database's view, as polkit's: not this process's groups, which a
       membership given since the login lacks).  $VITRINE_GROUP names
       another group (tests). */
    static bool vitrineGroupExists();
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
     * The preferences changed (Preferences > Tune the host): applied to the
     * VMs running now, not at the next start - off lets everything go,
     * on or another GPU floor tunes each running VM again
     */
    void preferencesChanged();

    /*
     * Focus priority: the vCPUs of the VM in front - its console or
     * full-screen window has the focus - real-time, those of the other VMs
     * ordinary (nice -5), so that VMs busy in the background cannot take
     * every CPU at real-time priority.  A host window in front changes
     * nothing: the VM last in front keeps them.  Through QEMU's own
     * x-vcpu-priority, or the helper's rt where QEMU lacks CAP_SYS_NICE.
     * A VM started later, or tuned again, goes behind once the helper's rt
     * is done with it.  VMs in QEMU's own window (SDL) are left real-time:
     * which window the desktop has in front is not known here.
     */
    void setFront(const QString &vmId);
    /* The app's (MainWindow makes it), for setFront(); null before */
    static HostSettings *instance();

    /*
     * The status bar's warning: tuning is on, and a VM runs untuned (the
     * helper could not run, or did not take it).  Never while tuning is off.
     */
    bool untuned() const;
    /* Why, while untuned(): that of the VM untuned first */
    Status untunedStatus() const;
    int untunedCount() const;

    /* What came of setUpGroup() */
    enum class Setup {
        Done,       // in the group: the state after is in @now
        Cancelled,  // the password dialog was dismissed, or the password refused
        Failed,     // @error says why
    };
    /*
     * The calling user in the vitrine group (created if need be):
     * "vitrine-helper setup-group" through pkexec, whose action always wants
     * an administrator's password, in the desktop's polkit dialog.  Then
     * polkit is asked again, and the VMs running untuned are tuned if it
     * says yes now.  One at a time: a second call while one runs fails.
     */
    void setUpGroup(const std::function<void(Setup result, const QString &error,
                                             const Status &now)> &done);
    bool settingUpGroup() const { return m_settingUp; }

    /* Tests: run @command instead of pkexec <helper>, without asking polkit */
    void setHelperCommand(const QStringList &command) { m_command = command; }
    /* Tests: where /sys is */
    void setSysRoot(const QString &root) { m_sysRoot = root; }
    bool helperRunning() const { return m_pid > 0; }

signals:
    /*
     * For the status bar, for a while: a setting the helper could not apply
     * here (kernel lockdown...), each text once per run of vitrine.  VMs
     * running untuned are untuned(), not a notice.
     */
    void notice(const QString &text);
    /* Each line the helper writes */
    void helperLine(const QString &line);
    /* The helper ended (after the last VM it watched, or refused) */
    void helperFinished();
    /* untuned() or untunedStatus() changed */
    void untunedChanged();
    /*
     * A VM starts untuned for want of the vitrine group, with tuning on:
     * the time to offer setUpGroup().  Once per run of vitrine, however many
     * VMs start (VMs started at once share one check).
     */
    void groupSetupSuggested(const Status &status);

private:
    /* polkit's answer is not kept: Denied only when pkexec itself refused */
    enum class Access { Unknown, Checking, Denied };

    void watchVm(Vm *vm);
    void vmStateChanged(Vm *vm);
    /* @vm in front or behind, as m_front says */
    void applyFront(Vm *vm);
    void start();
    void spawn(const QStringList &command);
    void readHelper();
    void handleLine(const QString &line);
    void flush();
    void reap();
    void closeHelper();
    /* No helper for the VMs asked for (@pids): for this start only, or
       (@always) for the rest of the run */
    void deny(const Status &status, QSet<qint64> pids, bool always = false);
    /* @pid runs untuned for @status, or (None) is tuned or gone */
    void setUntuned(qint64 pid, const Status &status);
    /* Each VM running untuned tuned again (the group set up, say) */
    void retune();
    /* grantCapability() for the stack's QEMU if it has none, once per run
       and binary; @granted: polkit said yes just now */
    void ensureCapability(bool granted = false);
    void say(const QString &text);

    VmStore *m_store;
    QStringList m_command;
    QString m_sysRoot = QStringLiteral("/sys");
    QHash<QString, qint64> m_tuned;     // VM id -> its QEMU's pid
    QString m_front;                    // the VM last in front
    QSet<qint64> m_expected;            // QEMUs asked to be watched, not ended
    Access m_access = Access::Unknown;
    Status m_denied;                    // why, when Denied
    QSet<QString> m_said;
    QList<QPair<qint64, Status>> m_untuned;   // QEMUs running untuned, the first first
    bool m_groupSuggested = false;
    bool m_settingUp = false;
    QSet<QString> m_capabilityTried;    // ensureCapability(): QEMUs asked for
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
