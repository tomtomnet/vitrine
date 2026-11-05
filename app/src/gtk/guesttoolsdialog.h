// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

class GuestToolsInstall;
class Vm;

/*
 * Install Guest Tools: the systems they are for and what the guest runs,
 * what they install, how (the VM restarts and the guest installs them
 * before its desktop starts, then restarts once more), a snapshot of the
 * disks first: the Qt app's GuestToolsDialog, with its texts
 * (ui/guesttoolstext.h).  Install, or Attach Medium Only, starts the
 * install the window keeps for the VM (GuestToolsInstall, through
 * @start): the dialog closes while the guest shuts down, the install goes
 * on, and the dialog opened again shows it, with Cancel Install.  Closing
 * the dialog never calls an install off.
 *
 * Over it, the medium of the VM's Fedora release: not built yet, being
 * built, failed to build, out of date, with Build…, Show or Update…, which
 * open the Builds window on it (@builds).  For any VM, but not into a guest
 * known to run another system.
 */
class GuestToolsDialog : public Gtk4::Dialog
{
public:
    /* The install the window keeps for @vm, started as chosen: Attach Medium Only, the snapshot */
    using Start = std::function<std::shared_ptr<GuestToolsInstall>(Vm *vm, bool mediumOnly,
                                                                   bool snapshot)>;
    /* The Builds window on @component, with the guest tools of Fedora @release */
    using ShowBuilds = std::function<void(const QString &component, const QString &release)>;

    /* @install: the VM's under way, if any */
    GuestToolsDialog(Vm *vm, std::shared_ptr<GuestToolsInstall> install, Start start,
                     ShowBuilds builds, Presenter presenter);

    Vm *vm() const;
    /* The install it shows, if any */
    std::shared_ptr<GuestToolsInstall> install() const { return m_install.lock(); }
    void setInstall(std::shared_ptr<GuestToolsInstall> install);
    /* What it shows, as the VM, the medium and the install are now: after a change of any */
    void refresh();

    /* Install, or Attach Medium Only: the buttons */
    void go(bool mediumOnly);
    /* For tests: what the buttons say and allow, the texts shown */
    QString installLabel() const;
    bool canInstall() const;
    bool canAttach() const;
    bool canCancel() const;
    /* The note on what the tools are and install, the progress, the banners: plain text */
    QString text() const;
    QString progressText() const;
    QString buildBannerText() const;
    QString warningText() const;
    void setSnapshot(bool snapshot);

private:
    void refreshBuild();

    QPointer<Vm> m_vm;
    std::weak_ptr<GuestToolsInstall> m_install;
    Start m_start;
    ShowBuilds m_builds;
    /* the Fedora release of the medium the build banner is about */
    QString m_release;
    /* the step of the install shown last: it steps out of the way as the guest shuts down */
    int m_seenStep = 0;
    int m_percent = -1;

    AdwBanner *m_build;
    AdwBanner *m_warning;
    GtkWidget *m_what;
    GtkWidget *m_snapshot;
    GtkWidget *m_attachRow;
    GtkWidget *m_attach;
    AdwPreferencesGroup *m_progressGroup;
    GtkWidget *m_spinner;
    GtkWidget *m_progress;
    GtkWidget *m_background;
    GtkWidget *m_cancel;
    GtkWidget *m_stop;
    GtkWidget *m_ok;
    GtkWidget *m_close;
};
