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
class QTimer;
class Vm;
class VmSnapshots;

/*
 * Install Guest Tools: what they install, how (the VM restarts and the guest
 * installs them before its desktop starts, then restarts once more), a
 * snapshot of the disks first.  It shuts the VM down, takes the snapshot,
 * and starts the VM again with the tools medium and the bootstrap.
 */
class GuestToolsDialog : public QDialog
{
    Q_OBJECT

public:
    /* Opens the dialog for @vm (none: nothing) */
    static void run(QWidget *parent, Vm *vm);
    /* How the main window starts a VM (its checks, USB access...); the
       runner's start() without */
    static void setStarter(const std::function<void(Vm *)> &start);

private:
    GuestToolsDialog(Vm *vm, QWidget *parent);
    void go(bool medium);
    void next();
    void fail(const QString &error);

    enum class Step { Idle, ShuttingDown, Snapshot, Starting };
    QPointer<Vm> m_vm;
    Step m_step = Step::Idle;
    bool m_mediumOnly = false;
    QLabel *m_progress;
    QCheckBox *m_snapshot;
    QDialogButtonBox *m_buttons;
    QPushButton *m_install;
    QPushButton *m_mediumButton;
    VmSnapshots *m_snapshots = nullptr;
    QTimer *m_timeout;
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
