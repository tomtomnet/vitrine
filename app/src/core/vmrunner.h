// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>

#include "core/argsfile.h"
#include "core/guestshutdown.h"

class QmpClient;

/*
 * Runs one VM: a virtiofsd per #share directive, then QEMU with the
 * arguments plus a QMP socket, both detached so that the VM outlives the
 * manager.  QEMU runs in the VM folder, so relative paths in the arguments
 * are relative to it.  The state comes from QMP.
 */
class VmRunner : public QObject
{
    Q_OBJECT

public:
    enum class State { Stopped, Starting, Running, Paused, Stopping };
    Q_ENUM(State)

    VmRunner(const QString &id, const QString &dir, QObject *parent = nullptr);
    ~VmRunner() override;

    State state() const;
    /* Not Stopped */
    bool isActive() const;
    /*
     * Paused because the guest suspended itself to RAM (ACPI S3), where
     * QEMU still takes input and a key wakes the guest - unlike a pause
     * from here (stop), where it drops it
     */
    bool isSuspended() const;
    /* Why the last start failed or QEMU stopped abnormally */
    QString errorString() const;
    /*
     * Why the VM may not start now, while something else has its files:
     * its disks being copied for a clone.  start() refuses it meanwhile,
     * with that as the error; empty: free.
     */
    QString busy() const;
    void setBusy(const QString &why);
    /* <dir>/qemu.log: the output of QEMU and virtiofsd during the last run */
    QString logPath() const;
    /*
     * @text in the log, each of its lines as a "vitrine: " note, as start()
     * writes what it did: for what vitrine finds out about the run later
     */
    void appendNote(const QString &text) const;
    /* The full QEMU command line for @args, as start() would run it */
    QStringList commandLine(const ArgsFile &args) const;
    /*
     * A VM of @args waits for vitrine's QEMU, which start() refuses it
     * without: neither its #qemu line nor the preferences choose a QEMU,
     * vitrine's is not built yet, and the system's QEMU, which it would run
     * with meanwhile, is not in PATH or lacks what @args use of vitrine's,
     * such as the 3D card's native context and vblank properties, asked
     * from that QEMU.  @why: the message for the user.
     */
    static bool needsQemuBuild(const ArgsFile &args, QString *why = nullptr);
    /*
     * @args with the properties vitrine computes for the 3D card, those
     * that @args leaves out (on the card's line, and with -global) and the
     * card has in the VM's QEMU
     * (@propertiesOf a driver; empty when not known, and then none):
     * x-vblank-swap-target by the guest's desktop, 4500 us for KDE, 6000
     * otherwise; x-vblank-swap-target-zc 3500 us, 4500 in vitrine's window
     */
    static ArgsFile withComputedProperties(
        const ArgsFile &args, const std::function<QStringList(const QString &driver)> &propertiesOf);
    /*
     * @args with x-render-node=@node, the GPU the VMs render on when they
     * choose none (HostGpus::automaticNode()), on its virtio-gpu-rutabaga
     * cards that set none, on their line or with -global, and have it in
     * the VM's QEMU (@propertiesOf, as above).  Nothing for an empty @node,
     * as on a host with one GPU: the command line stays as it is.
     */
    static ArgsFile withRenderNode(
        const ArgsFile &args, const QString &node,
        const std::function<QStringList(const QString &driver)> &propertiesOf);
    /*
     * @args with rendernode=@node on its D-Bus display with OpenGL
     * (-display dbus,gl=on), unless it sets one: the GPU of QEMU's EGL,
     * which draws the VM's screen for vitrine's window and which the 3D of
     * a card without x-render-node (virtio-vga-gl) follows, vrend and the
     * native context alike: the desktop's on a host with several
     * (HostGpus::displayNode()), in every VM.  Nothing for an empty @node.
     */
    static ArgsFile withDisplayRenderNode(const ArgsFile &args, const QString &node);
    /*
     * Why the GPU @args chose for its rutabaga card (x-render-node, also
     * with -global) cannot be used on this computer: nothing there now (a
     * GPU taken out), something other than a GPU's render node, or a GPU
     * without the native context the card asks for (x-drm).  Empty when it
     * can, and without a choice.  start() refuses such a VM with it, in
     * plainer words than QEMU's refusal.
     */
    static QString renderNodeProblem(const ArgsFile &args);
    /*
     * The environment variables QEMU gets on top of the manager's, as
     * NAME=VALUE: DISABLE_RTKIT=1 (PipeWire's RTKit fallback would give
     * QEMU a real-time time limit that kills it once the helper makes its
     * threads real-time), those of the display, then the #env directives
     */
    static QStringList environment(const ArgsFile &args);
    /*
     * @environment (NAME=VALUE) as shell assignments before a command: the
     * values quoted where needed, never the names, since a shell takes a
     * quoted 'NAME=VALUE' for the name of the command
     */
    static QStringList shellAssignments(const QStringList &environment);
    /*
     * The QMP socket of the VM's display, for the view to attach with
     * getfd + add_client: a monitor of its own, since the runner keeps the
     * main one.  Empty unless the running QEMU shows in vitrine's window.
     */
    QString displaySocket() const;
    /*
     * The socket of the nested desktop shown in vitrine's window
     * (NestedDesktop::Mode::Integrated), which QEMU's cross-domain connects
     * to, while the VM is active: the console serves it (its Desktop view).
     * Empty for other runs.
     */
    QString nestedSocket() const;
    /*
     * How that socket is served, as it must be before QEMU starts: @serve
     * listens on @socket, or says why it cannot.  Without it, or when it
     * fails, such a VM does not start.
     */
    void setNestedHost(const std::function<bool(const QString &socket, QString *error)> &serve);
    /* The socket of the vitrine agent's port (guest tools), while active */
    QString agentSocket() const;
    /* The socket of the port of QEMU's guest agent (qemu-ga) that Shut Down
       uses, while active: the one the runner adds, or the VM's own; empty
       without */
    QString guestAgentSocket() const;
    /*
     * A VM shown in vitrine's window starts paused (QEMU's -S) and waits
     * for its screen, the state Starting meanwhile: the view attaches to
     * displaySocket() and tells QEMU the size and refresh rate of the
     * screen, then displayReady() lets the guest run.  The guest has the
     * host's refresh rate from its first frame on: QEMU's EDID says 75 Hz
     * until a display tells it, and Plasma's animations run too fast for a
     * whole session when the rate changes after its shell started.  After
     * a few seconds without displayReady() the guest runs anyway.  Not for
     * a VM whose own arguments start it paused (-S), which stays paused.
     */
    bool waitsForDisplay() const;
    void displayReady();
    /* How long the guest waits for its screen at most, in ms (for tests) */
    static void setDisplayWait(int ms);
    /*
     * The arguments of the current run, or the last: those given to start(),
     * also for a QEMU found running (start() keeps them with the run's
     * runtime files).  vm.args may have changed since.
     */
    ArgsFile runArgs() const;

    /* The state goes Starting, then Running once QMP answers, or back to
       Stopped with failed() */
    void start(const ArgsFile &args);
    /*
     * Picks up a QEMU started by an earlier run of the manager, if any,
     * with the arguments it was started with; @args, those of the VM, only
     * for a QEMU started by a vitrine that did not keep them
     */
    void attach(const ArgsFile &args);
    void pause();
    void resume();
    /*
     * Asks the guest to shut down, the gentlest way it takes
     * (GuestShutdown): the shutdown handler, else QEMU's guest agent if the
     * guest runs it, else the ACPI power button - which a KDE guest answers
     * with its logout screen, waiting for someone at the guest's screen to
     * confirm.  shutdownWayChanged() tells each way tried.
     */
    void powerdown();
    /* The way the last powerdown() of the run took, or is trying; None
       once the guest shuts down */
    GuestShutdown::Way shutdownWay() const;
    /* The ACPI power button only */
    void pressPowerButton();
    /*
     * The guest's own way to shut down, tried first by powerdown(): the
     * guest tools' agent, which powers the guest off at once.  It returns
     * false if it cannot take the request now, else it answers later
     * whether it did (GuestShutdown::Asker).
     */
    void setShutdownHandler(const GuestShutdown::Asker &handler);
    void reset();
    /*
     * Quits QEMU at once: QMP quit, then SIGTERM if it is still there
     * after a few seconds, and never more - a SIGKILL loses what QEMU had
     * not written to the disks yet, a qcow2 image's cached metadata among
     * it.  A QEMU still running a few seconds after SIGTERM gets
     * notResponding(), for the user to decide (killQemu()).
     */
    void forceOff();
    /*
     * SIGKILL for a QEMU that does not respond (notResponding()), only when
     * the user asked for it: its disks may lose their last writes
     */
    void killQemu();
    /* Tests: how long QEMU has to quit before SIGTERM, and to end after it
       (twice that once its monitor closed) */
    void setQuitTimeout(int ms);
    /*
     * A guest asked to shut down (powerdown()) that panics on its way, as a
     * nested desktop's guest can at the very end of its poweroff (fbcon,
     * with a DRM session on two cards): QMP's GUEST_PANICKED, from the
     * nested desktop's ISA pvpanic, quits QEMU.  And the last resort, for a
     * nested desktop's guest that stalls without panicking: once its agent
     * went away (the guest is shutting down) it has this long to end,
     * else QEMU quits.  Long, as a slow shutdown must not be cut: 5 min.
     * Tests: shorter.
     */
    void setShutdownStallTimeout(int ms);
    /* Null while stopped */
    QmpClient *qmp() const;
    /* QEMU's process, 0 while stopped */
    qint64 pid() const;

signals:
    void stateChanged(VmRunner::State state);
    /* isSuspended() changed, which a pause of a suspended guest does
       without a state change */
    void suspendedChanged(bool suspended);
    /* waitsForDisplay() changed */
    void waitsForDisplayChanged(bool waiting);
    /* The start failed, QEMU stopped unexpectedly, or refused a command */
    void failed(const QString &error);
    /*
     * QEMU did not end after forceOff(): neither QMP quit nor SIGTERM ended
     * it.  It runs on (Stopping) until it ends or killQemu() ends it.  A
     * QEMU on its way out by itself (the guest shut down) is waited for
     * without it, however long it takes, until forceOff().
     */
    void notResponding();
    /*
     * The guest agent came up, at each boot of the guest, and the shared
     * folders with a mount point were mounted there, or not
     */
    void sharesMounted(const QStringList &mounted, const QStringList &problems);
    /*
     * QEMU's guest agent said what system the guest runs (its answer to
     * guest-get-osinfo: id, version-id, pretty-name...): at each boot of
     * the guest, as the agent comes up, and for a VM found running
     */
    void guestOsInfo(const QJsonObject &info);
    /*
     * QEMU's guest agent opened its port, or closed it: it comes up at each
     * boot of the guest, and goes as the guest shuts down or restarts (or
     * as qemu-ga itself restarts).  The runner's port, or the VM's own if
     * that one has an id; none without.
     */
    void guestAgentChanged(bool open);
    /* powerdown() tries @way; None: the request is over (the guest shuts
       down, or the run ended) */
    void shutdownWayChanged(GuestShutdown::Way way);
    /* The guest reboots, by itself or by Reset: QMP's RESET */
    void guestReset();

private:
    struct Private;
    Private *d;
};
