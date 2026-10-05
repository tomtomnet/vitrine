// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QPointer>

#include <functional>

class Banner;
class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QPushButton;
class Vm;
class VmSnapshots;

/*
 * Install Guest Tools: what they install, how (the VM restarts and the guest
 * installs them before its desktop starts, then restarts once more), a
 * snapshot of the disks first.  It shuts the VM down, takes the snapshot,
 * and starts the VM again with the tools medium and the bootstrap.
 *
 * Never in the way of the VM's screen, where the guest may ask what to do
 * when asked to shut down (Plasma's logout screen): not modal, and hidden
 * while the guest shuts down, until the VM starts again.
 */
class GuestToolsDialog : public QDialog
{
    Q_OBJECT

public:
    /* Opens the dialog for @vm (none: nothing), or brings back its open
       one, hidden while its guest shuts down */
    static void run(QWidget *parent, Vm *vm);
    /* How the main window starts a VM (its checks, USB access...); the
       runner's start() without */
    static void setStarter(const std::function<void(Vm *)> &start);

    /* Its open one for @vm, if any */
    static GuestToolsDialog *of(Vm *vm);

private:
    GuestToolsDialog(Vm *vm, QWidget *parent);
    /* What the buttons do as the VM is now, and what keeps them from it */
    void refresh();
    void go(bool medium);
    void next();
    void fail(const QString &error);
    /* Its content changed: as high as it is at its width */
    void fit();
    /* The progress line, under the notes */
    void setProgress(const QString &text);

    enum class Step { Idle, ShuttingDown, Snapshot, Starting };
    QPointer<Vm> m_vm;
    Step m_step = Step::Idle;
    bool m_mediumOnly = false;
    QLabel *m_restart;
    Banner *m_warning;
    QLabel *m_progress;
    QCheckBox *m_snapshot;
    QDialogButtonBox *m_buttons;
    QPushButton *m_install;
    QPushButton *m_mediumButton;
    VmSnapshots *m_snapshots = nullptr;
};

/*
 * How the guest tools of a VM are doing, across the top of its page, with
 * the button that goes with it: Install..., Restart Guest, Update...
 */
class GuestToolsBanner : public QWidget
{
    Q_OBJECT

public:
    explicit GuestToolsBanner(QWidget *parent = nullptr);
    void setVm(Vm *vm);

private:
    void update();

    QPointer<Vm> m_vm;
    Banner *m_info;
    Banner *m_warning;
};
