// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QPointer>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class Vm;
class VmCloner;
class VmStore;

/*
 * Clones a stopped VM under a new name, showing what it copies.  Closing
 * it during the copy does not stop it: the copy goes on, a task of the
 * app's, whose details show the dialog again, no longer modal; Stop stops
 * it.  The dialog goes once it is closed with nothing left to show: the
 * copy over and its task gone.
 */
class CloneDialog : public QDialog
{
    Q_OBJECT
    /* for the metatype of Vm *, in cloned() */
    Q_MOC_INCLUDE("core/vmstore.h")

public:
    CloneDialog(VmStore *store, Vm *source, QWidget *parent = nullptr);
    ~CloneDialog() override;

    /* Once accepted */
    Vm *clone() const;
    /* The copy goes on */
    bool isCloning() const;

    /* Clone: starts the copy */
    void accept() override;
    /* Cancel, or the copy going on unseen */
    void reject() override;

signals:
    /* The new VM, for the window to select: as the dialog shows the end
       of the copy, or once the task's details are asked for after it */
    void cloned(Vm *clone);

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void finished(Vm *clone, const QString &error);
    /* The task's details: the dialog, or the clone it made unseen */
    void present();
    /* Before the copy, during it, after it */
    void updateButtons();
    /* Deleted once hidden with nothing left to show: no copy, no task */
    void goIfDone();

    QPointer<Vm> m_source;
    QPointer<Vm> m_clone;
    VmCloner *m_cloner;
    QLineEdit *m_name;
    QLabel *m_error;
    QProgressBar *m_progress;
    QLabel *m_background;
    QPushButton *m_ok;
    QPushButton *m_stop;
    QPushButton *m_cancel;
    QPushButton *m_close;
    /* The copy ended well: the details are the new VM's */
    bool m_made = false;
};
