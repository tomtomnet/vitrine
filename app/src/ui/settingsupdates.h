// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QWidget>

#include "core/settingsupdates.h"

class Banner;
class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class Vm;

/*
 * The settings vitrine gives the VMs it makes now that a VM lacks
 * (SettingsUpdatesMonitor), across the top of its page, never in a window
 * of their own: on the Console's home page while it is off, one banner
 * with Review… and Not Now, which hides it until vitrine quits; on its
 * Details, the main place, what it lacks (or no longer asks about) with
 * Review…, the latest update with Undo…, and why vm.args cannot be checked.
 */
class SettingsUpdatesBanner : public QWidget
{
    Q_OBJECT

public:
    enum class Place { Home, Details };

    explicit SettingsUpdatesBanner(Place place, QWidget *parent = nullptr);
    void setVm(Vm *vm);

    /* For tests: what each banner says, empty while hidden, and their buttons */
    QString offerText() const { return m_offerText; }
    QString updateText() const { return m_updateText; }
    QString problemText() const { return m_problemText; }
    QPushButton *reviewButton() const;
    QPushButton *notNowButton() const { return m_notNow; }
    QPushButton *undoButton() const;

private:
    void update();

    Place m_place;
    QPointer<Vm> m_vm;
    Banner *m_offer;
    Banner *m_updated;
    Banner *m_problem;
    QPushButton *m_notNow = nullptr;
    QString m_offerText;
    QString m_updateText;
    QString m_problemText;
};

/*
 * Review: the changes found, each checked, with why and the lines of
 * vm.args it changes, and Don't Ask Again; then those not asked about
 * again, with Ask Again.  Apply saves a copy of vm.args beside it first,
 * changes only those lines, and only while the VM is off; vm.args edited
 * meanwhile shows the list again rather than apply what was not seen.
 * Not Now hides the home page's banner until vitrine quits.
 */
class SettingsUpdatesDialog : public QDialog
{
    Q_OBJECT

public:
    /*
     * Opens it for @vm, from @from on a VmPane's page, whose changes not
     * applied yet would be lost or would undo the update: applied or
     * discarded first, as starting the VM does
     */
    static void run(QWidget *from, Vm *vm);
    /* Undoes @vm's latest update, from @from as run(), asked first */
    static void undo(QWidget *from, Vm *vm);

    explicit SettingsUpdatesDialog(Vm *vm, QWidget *parent = nullptr);

    /* Apply */
    void accept() override;
    /* Not Now */
    void reject() override;

    /* For tests */
    QStringList offered() const;
    QStringList declined() const;
    QCheckBox *checkBox(const QString &id) const { return m_checks.value(id); }
    QPushButton *applyButton() const;
    QString note() const { return m_noteText; }
    void decline(const QString &id);
    void askAgain(const QString &id);

private:
    /* The changes as the monitor has them now, the choices kept */
    void rebuild();
    void update();

    QPointer<Vm> m_vm;
    QList<SettingsUpdates::Change> m_changes;
    QList<SettingsUpdates::Change> m_declined;
    QHash<QString, bool> m_checked;
    QHash<QString, QCheckBox *> m_checks;
    bool m_built = false;
    bool m_stale = false;
    bool m_ownChange = false;
    QString m_noteText;

    Banner *m_note;
    QWidget *m_list;
    QVBoxLayout *m_rows;
    QDialogButtonBox *m_buttons;
};
