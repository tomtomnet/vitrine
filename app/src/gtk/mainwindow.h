// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

#include "core/firmwarefiles.h"
#include "core/vmrunner.h"

class CloneDialog;
class GtkLoop;
class SettingsDialog;
class Vm;
class VmCloner;
class VmPage;
class VmStore;

/*
 * vitrine-gtk's window, after the GNOME HIG: the VMs in a sidebar, each
 * with its state, the selected one's page beside it (VmPage), an empty
 * state while there are none; the primary menu in the sidebar's header
 * bar, the VM's power actions in the page's.  The sidebar hides (F9), and
 * overlays the page in a narrow window.
 *
 * It does what the Qt app's MainWindow does for the same, through the same
 * core calls: Start and its checks (a VM busy with a clone, the nested
 * desktop's one card, Vitrine's QEMU to build, damaged firmware, USB
 * access), Shut Down and how it reached the guest, Pause and Resume, Reset,
 * Force Off and a QEMU that does not end, why a run failed, Remove; and
 * what closing asks while VMs show in the window or tasks run.  The
 * actions whose dialogs are not in the GTK app yet are there, inactive:
 * New VM, Import VM, Settings, Clone, Builds, Preferences.
 *
 * It outlives its GtkWindow (main() keeps it until the loop ends): once
 * the window is gone it does nothing.
 */
class MainWindow
{
public:
    MainWindow(GtkLoop *loop, VmStore *store);
    ~MainWindow();
    MainWindow(const MainWindow &) = delete;
    MainWindow &operator=(const MainWindow &) = delete;

    /* Null once closed */
    GtkWindow *window() const;
    /* In front, with @token from another instance to take the focus, if any */
    void present(const QByteArray &token = {});
    /* The selected VM, if any */
    Vm *current() const;
    void select(const QString &id);
    /*
     * Quits as Close does when its questions are answered Keep Running in
     * the Background and Close Anyway, asking nothing: for SIGTERM, SIGINT
     * and SIGHUP (QuitSignals).  The VMs run on, the tasks under way end
     * with the app, the window's state is saved.
     */
    void quitWithoutQuestions();
    /* A notice over the VM's page, as the Qt app's status bar has it */
    void notify(const QString &text, int timeoutMs = 8000);
    /*
     * @dialog in the window.  The VM's screen is covered meanwhile
     * (VmPage::setCovered()): what GTK draws over the screen is under it.
     */
    void present(AdwDialog *dialog);

private:
    struct Item {
        QString id;
        AdwSidebarItem *item;
    };

    void buildWindow();
    void createActions();
    GSimpleAction *addAction(GActionMap *map, const char *name,
                             std::function<void()> activated, const char *accel = nullptr);
    void setEnabled(const char *name, bool enabled);

    void addVm(Vm *vm);
    /* Its item in the sidebar, in its place by name */
    void insertItem(Vm *vm);
    void removeItem(const QString &id);
    int indexOf(const QString &id) const;
    /* The id of the VM selected in the sidebar, which the store may have removed already */
    QString selectedId() const;
    void updateItem(Vm *vm);
    void stateChanged(Vm *vm, VmRunner::State state);
    void failed(Vm *vm, const QString &error);
    void currentChanged();
    void updateActions();
    void updateWaiting();

    /* Start, from the running state of @snapshot if given (Start From It) */
    void start(const QString &snapshot = {});
    /* Start, past the questions that may call it off */
    void startChecked(const QString &id, const QString &snapshot = {});
    void togglePause();
    void shutDown();
    void reset();
    void forceOff();
    void remove();
    void showLog();
    void openFolder();
    void about();

    /* The dialogs, in mainwindowdialogs.cpp: what they present goes through present() */
    std::function<void(AdwDialog *)> presenter();
    void newVm();
    void importVm();
    void showCommandLine();
    /* The settings of @vm, on @page (SettingsDialog::Page), else the one shown last */
    void openSettings(Vm *vm, int page = -1);
    /* Ctrl+S: Apply, while the settings show; else they show */
    void settingsShortcut();
    void cloneVm();
    void showSnapshots();
    /* The log of @vm, as QEMU writes it */
    void showLogOf(Vm *vm);
    /* The copy of @source as @name, the window's: it goes on with the dialog closed */
    VmCloner *startClone(Vm *source, const QString &name);

    /* Before a start: damaged firmware copies of @vm, new ones if the user wants; then @go */
    void checkFirmware(Vm *vm, std::function<void()> go);
    /* New copies of @files in their place, if the user wants; @done(true) when done */
    void resetFirmware(Vm *vm, const QList<FirmwareFiles::File> &files, const QString &accept,
                       std::function<void(bool)> done);
    /* A QEMU that does not end after Force Off: killed if the user says so */
    void askKill(Vm *vm);

    /* GtkWindow::close-request: true to keep the window, while asking */
    bool closeRequested();
    /*
     * The tasks under way, if any: true while it asks whether to stop
     * them, which closes once answered; else the window may go now (its
     * state saved)
     */
    bool askTasks();
    /* After an answer to the close request: the tasks, then closing */
    void closeUnlessAsked();
    void finishClosing();
    void closed();

    GtkLoop *m_loop;
    VmStore *m_store;
    /* Qt's connections, which go with the window */
    std::unique_ptr<QObject> m_qt;
    std::unique_ptr<VmPage> m_page;

    GtkWidget *m_window = nullptr;
    GtkStack *m_main = nullptr;
    AdwOverlaySplitView *m_split = nullptr;
    AdwSidebar *m_sidebar = nullptr;
    AdwSidebarSection *m_section = nullptr;
    QList<Item> m_items;
    QHash<QString, GSimpleAction *> m_actions;
    /* In-window dialogs open, which cover the screen */
    int m_covering = 0;
    /* The window may go: its questions are answered */
    bool m_closing = false;
    /* While the list changes: no current change to follow */
    bool m_syncing = false;

    /* Why the last run of a VM ended with an error, by id */
    QHash<QString, QString> m_errors;
    /* The state of each VM before its latest change */
    QHash<QString, VmRunner::State> m_states;
    QHash<QString, VmRunner::State> m_endedFrom;
    /* Started from here, not yet running */
    QSet<QString> m_starting;
    /* Waiting for access to their USB devices, to start */
    QSet<QString> m_askingUsb;
    /* VMs asked to shut down as the window closed: it closes once they are off */
    QSet<QString> m_closeAfter;
    /* Of those, the VMs still starting when asked: asked once they run */
    QSet<QString> m_shutDownOnceUp;
    /* The questions whether to kill a QEMU that does not end, by VM */
    QHash<QString, AdwDialog *> m_killPrompts;
    /* The settings, while they show */
    SettingsDialog *m_settings = nullptr;
    /* The copies of clones going on, by the VM copied; the dialog showing one */
    QHash<QString, QPointer<VmCloner>> m_clones;
    CloneDialog *m_cloneDialog = nullptr;
};
