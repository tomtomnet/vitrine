// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"
#include "gtk/guarded.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "core/settingsupdates.h"

class Vm;

/*
 * The settings vitrine gives the VMs it makes now that the VM shown lacks
 * (SettingsUpdatesMonitor), where the user looks for them, never in a
 * dialog of their own: a row on the Console's home page while the VM is
 * off, which Not Now hides until vitrine quits, and the Details' group,
 * the main place, with each change and why, Review…, the latest update and
 * its Undo…, why vm.args cannot be checked, and the changes not asked
 * about again, each with Ask Again.  Hidden when there is nothing to say.
 */
class SettingsUpdatesStatus : public Gtk4::Guarded
{
public:
    SettingsUpdatesStatus();
    ~SettingsUpdatesStatus();
    SettingsUpdatesStatus(const SettingsUpdatesStatus &) = delete;
    SettingsUpdatesStatus &operator=(const SettingsUpdatesStatus &) = delete;

    /* For the home page: a boxed list of one row */
    GtkWidget *homeWidget() const { return m_home; }
    /* For the Details */
    AdwPreferencesGroup *detailsGroup() const { return m_details; }

    void setVm(Vm *vm);
    Vm *vm() const;

    /* Review… for @vm: the dialog */
    std::function<void(Vm *vm)> review;
    /* Undo… of @vm's latest update, asked first */
    std::function<void(Vm *vm)> undo;

    /* For tests: the home row's title, empty while it is hidden; the Details' rows' titles */
    QString homeText() const { return m_homeText; }
    QStringList detailsRows() const { return m_rowTitles; }

private:
    void update();

    QObject m_qt;
    QPointer<Vm> m_vm;
    GtkWidget *m_home;
    GtkWidget *m_homeRow;
    AdwPreferencesGroup *m_details;
    GtkWidget *m_review;
    /* the Details' rows, made again at each change */
    QList<GtkWidget *> m_rows;
    QString m_homeText;
    QStringList m_rowTitles;
};

/*
 * Review: the changes found, each checked, with why and the lines of
 * vm.args it changes, and Don't Ask Again; then those not asked about
 * again, with Ask Again.  Apply saves a copy of vm.args beside it first,
 * changes only those lines, and only while the VM is off; vm.args edited
 * meanwhile shows the list again rather than apply what was not seen.
 * Not Now closes it, and hides the home page's row until vitrine quits.
 */
class SettingsUpdatesDialog : public Gtk4::Dialog
{
public:
    /* What the window does before Apply: the settings' changes not applied yet first */
    using Confirm = std::function<void(Vm *vm, std::function<void(bool go)> done)>;

    SettingsUpdatesDialog(Vm *vm, Presenter presenter, Confirm confirm = {});

    /* For tests: the ids offered and those not asked about again, in their order */
    QStringList offered() const;
    QStringList declined() const;
    bool isChecked(const QString &id) const;
    void setChecked(const QString &id, bool checked);
    bool canApply() const;
    /* What the banner says, empty while it is hidden */
    QString note() const { return m_note; }
    void apply();
    void decline(const QString &id);
    void askAgain(const QString &id);
    void notNow();

private:
    /* The changes as the monitor has them now, the checks kept */
    void rebuild();
    void update();
    /* Apply, once the settings' changes were dealt with */
    void applyNow();

    QPointer<Vm> m_vm;
    Confirm m_confirm;
    QList<SettingsUpdates::Change> m_changes;
    QList<SettingsUpdates::Change> m_declined;
    /* the user's choice, kept through a rebuild */
    QHash<QString, bool> m_checked;
    QHash<QString, GtkWidget *> m_checks;
    QString m_note;
    /* vm.args changed while the list showed: it says so */
    bool m_stale = false;
    /* the list changes for a choice made here, which says nothing */
    bool m_ownChange = false;

    AdwPreferencesPage *m_page;
    AdwPreferencesGroup *m_group = nullptr;
    AdwPreferencesGroup *m_declinedGroup = nullptr;
    AdwBanner *m_banner;
    GtkWidget *m_apply;
};
