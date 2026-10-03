// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QStringList>

#include <functional>

#include "core/argsfile.h"

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
    /* Why the last start failed or QEMU stopped abnormally */
    QString errorString() const;
    /* <dir>/qemu.log: the output of QEMU and virtiofsd during the last run */
    QString logPath() const;
    /* The full QEMU command line for @args, as start() would run it */
    QStringList commandLine(const ArgsFile &args) const;
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
     * The environment variables QEMU gets on top of the manager's, as
     * NAME=VALUE: those of the display, then the #env directives
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
     * main one.  Empty unless the running VM shows in vitrine's window.
     */
    QString displaySocket() const;
    /*
     * The arguments of the current run, or the last: those given to start(),
     * or to attach() for a QEMU found running.  vm.args may have changed since.
     */
    ArgsFile runArgs() const;

    /* The state goes Starting, then Running once QMP answers, or back to
       Stopped with failed() */
    void start(const ArgsFile &args);
    /* Picks up a QEMU started by an earlier run of the manager, if any;
       @args, those of the VM, tell which shared folders to mount */
    void attach(const ArgsFile &args = {});
    void pause();
    void resume();
    /* ACPI power button */
    void powerdown();
    void reset();
    /* Quits QEMU at once */
    void forceOff();
    /* Null while stopped */
    QmpClient *qmp() const;
    /* QEMU's process, 0 while stopped */
    qint64 pid() const;

signals:
    void stateChanged(VmRunner::State state);
    /* The start failed, QEMU stopped unexpectedly, or refused a command */
    void failed(const QString &error);
    /*
     * The guest agent came up, at each boot of the guest, and the shared
     * folders with a mount point were mounted there, or not
     */
    void sharesMounted(const QStringList &mounted, const QStringList &problems);

private:
    struct Private;
    Private *d;
};
