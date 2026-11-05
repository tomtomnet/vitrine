// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>

class AppBanners;
class BuildsWindow;
class GuestToolsDialog;
class GuestToolsInstall;
class GuestToolsStatus;
class MainWindow;
class Task;
class UpdateNotifier;
class Vm;

/*
 * The main window's part for what Vitrine builds and for the guest tools
 * (its other parts: mainwindow.cpp, mainwindowconsole.cpp,
 * mainwindowdialogs.cpp): the Builds window; Install Guest Tools… and the
 * installs it keeps for the VMs, which go on with the dialog closed; the
 * guest tools' state of the VM shown, on its home page and its Details;
 * the banners of Vitrine's QEMU and of the host's KDE packages; the check
 * for updates; the tasks of the host packages' installs and reverts, and
 * the details of the builds' (the Builds window).  Vitrine back in front:
 * what builds run by hand made meanwhile.
 *
 * Made with the window's actions, before its VMs are listed; it does
 * nothing once the window is gone.
 */
class MainWindowBuilds
{
public:
    explicit MainWindowBuilds(MainWindow *window);
    ~MainWindowBuilds();
    MainWindowBuilds(const MainWindowBuilds &) = delete;
    MainWindowBuilds &operator=(const MainWindowBuilds &) = delete;

    /* MainWindow::showBuilds() */
    void showBuilds(const QString &component, const QString &guestRelease);
    /* Install Guest Tools… for @vm: the dialog, showing its install under way if any */
    void installGuestTools(Vm *vm);
    /* The VM shown: its guest tools' state, and the actions */
    void setVm(Vm *vm);
    /* @vm stays off once its guest shut down: the window closes once its VMs are off */
    void stayOff(Vm *vm);
    /* The window is gone */
    void closed();

    /* For tests */
    AppBanners *banners() const { return m_banners.get(); }
    GuestToolsStatus *toolsStatus() const { return m_toolsStatus.get(); }
    GuestToolsDialog *toolsDialog() const { return m_toolsDialog; }
    std::shared_ptr<GuestToolsInstall> toolsInstall(const QString &vmId) const;
    BuildsWindow *buildsWindow() const { return m_buildsWindow.get(); }

private:
    /* The install the window keeps for @vm, started as the dialog chose */
    std::shared_ptr<GuestToolsInstall> startGuestTools(Vm *vm, bool mediumOnly, bool snapshot);
    void installChanged(const QString &id);
    /* An install over: it goes, unless a dialog shows how it ended */
    void dropInstall(const QString &id);
    void cancelPending(Vm *vm);
    /* The host packages' install or revert as a task */
    void hostStarted(bool install);
    void hostFinished(const QString &error, bool cancelled);
    /* Vitrine in front again, after another program */
    void activated();
    void watchActive(GtkWidget *window);
    void presentOverBuilds(AdwDialog *dialog);
    /* Vitrine's QEMU built: if the VMs' QEMU is another now, its documentation */
    void qemuBuilt();

    MainWindow *m_window;
    /* Qt's connections, which go with the window */
    std::unique_ptr<QObject> m_qt;
    std::unique_ptr<AppBanners> m_banners;
    std::unique_ptr<GuestToolsStatus> m_toolsStatus;
    std::unique_ptr<BuildsWindow> m_buildsWindow;
    std::unique_ptr<UpdateNotifier> m_updates;
    /* the guest tools' installs under way, or over and shown, by VM id */
    QHash<QString, std::shared_ptr<GuestToolsInstall>> m_installs;
    GuestToolsDialog *m_toolsDialog = nullptr;
    /* the task of the host packages' install or revert running, and what it is about */
    QPointer<Task> m_hostTask;
    QStringList m_hostIds;
    /* the QEMU the VMs got before the last build of Vitrine's */
    QString m_qemuBefore;
    /* one of the app's windows is in front */
    bool m_active = false;
    guint m_activeLater = 0;
};
