// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

class HostPrefs;
class QProcess;
class QTimer;
class VmStore;

/*
 * Preferences, as GNOME's apps have them: an AdwPreferencesDialog of two
 * pages.  General: the QEMU the VMs run (Vitrine's, built from here, or
 * another one), the guest tools' medium, virtiofsd, the folder of the VMs
 * and the update check; Host: host tuning while VMs run (HostPrefs).
 *
 * The Qt app's PreferencesDialog, through the same calls, but each change
 * applies at once, as GNOME's preferences do, with no OK: a path typed
 * applies with Enter or its row's apply button, the folder of the VMs as
 * it is chosen (the window lists that folder's VMs then).  The build rows'
 * buttons open the Builds window on what they show.
 */
class PreferencesDialog : public Gtk4::Dialog
{
public:
    /* The Builds window, on "qemu", or on "medium" with the guest tools' release */
    using ShowBuilds = std::function<void(const QString &component, const QString &guestRelease)>;

    enum Page { General, Host };

    PreferencesDialog(VmStore *store, Presenter presenter, ShowBuilds showBuilds);
    ~PreferencesDialog() override;

    void setPage(Page page);
    HostPrefs *host() const { return m_host.get(); }
    /* Its widget named @name (tests): rows by what they set */
    GtkWidget *find(const char *name) const;
    /* The folder of the VMs, as Change… and Default set it */
    void setVmsDir(const QString &dir);

private:
    void buildGeneral(AdwPreferencesPage *page);
    void updateStack();
    void updateTools();
    /* Another QEMU, switched on or off, its path applied: saved, then checked */
    void applyQemu();
    void checkQemu();
    void applyVirtiofsd();
    void checkVirtiofsd();
    void updateVmsDir();
    void closed() override;

    QPointer<VmStore> m_store;
    ShowBuilds m_showBuilds;
    std::unique_ptr<HostPrefs> m_host;
    /* the other QEMU's check, a moment after its path changes, as the Qt dialog has it */
    QTimer *m_check;
    QProcess *m_version = nullptr;
    /* the folder of the VMs was changed here: what that changed shows */
    bool m_dirChanged = false;

    AdwPreferencesPage *m_general;
    GtkWidget *m_stack;
    GtkWidget *m_stackButton;
    GtkWidget *m_custom;
    GtkWidget *m_qemu;
    GtkWidget *m_qemuStatus;
    GtkWidget *m_tools;
    GtkWidget *m_toolsButton;
    /* the group says what shared folders use */
    AdwPreferencesGroup *m_shares;
    GtkWidget *m_virtiofsd;
    GtkWidget *m_vmsDir;
    GtkWidget *m_vmsDirDefault;
    GtkWidget *m_updates;
};
