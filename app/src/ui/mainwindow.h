// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSet>

#include "core/vmrunner.h"

class MainBar;
class QAction;
class QKeyEvent;
class QMenu;
class QMenuBar;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class UpdateNotifier;
class UsbMenu;
class QStackedWidget;
class Sidebar;
class StatusStats;
class Vm;
class VmConsole;
class VmDetails;
class VmPane;
class VmStore;

/*
 * The VMs on the left, the selected one on the right, in tabs: its console
 * first, where its screen shows while it runs, VMware style
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(VmStore *store, QWidget *parent = nullptr);

    /* The selected VM, if any */
    Vm *current() const;
    void select(const QString &id);
    void newVm();
    void importVm();
    /* The Builds window, on @component ("qemu") if given */
    void showBuilds(const QString &component = {});
    void openSettings(Vm *vm, int page = -1);
    /* Shows the window over the others, for a second instance */
    void bringToFront();

    /* The bar's own menu: what shows and hides the list, not the bar */
    QMenu *createPopupMenu() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    /* Dialogs, from anywhere: they come out of full screen first */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void createActions();
    /* The list of VMs collapsed to its icons, or expanded: what F9 and the
       bar's button say they do */
    void libraryCollapsed(bool collapsed);
    /* F10: the first menu, as Alt and its letter open it */
    void openMenu();
    void addVm(Vm *vm);
    void removeItem(const QString &id);
    QListWidgetItem *itemOf(const QString &id) const;
    void updateItem(Vm *vm);
    void stateChanged(Vm *vm, VmRunner::State state);
    void failed(Vm *vm, const QString &error);
    void updateActions();
    void updateStatus();
    void currentChanged();
    void showCurrent();
    void leaveVm();
    /* The console of @vm, made the first time */
    VmConsole *consoleOf(Vm *vm);
    /* That of the selected VM, if made */
    VmConsole *currentConsole() const;
    void consoleChanged(VmConsole *console);
    /* Who has the keyboard, in the status bar */
    void updateInput();
    /* Why the screen of the VM shown may take the keys (focusScreen()) */
    enum class FocusCause {
        Activated,  // the window came to the front
        Shown,      // the screen appeared, its VM runs again, its tab was chosen
        Switched,   // another VM was selected
    };
    /* The keys to the screen of the VM shown, without a click on it */
    void focusScreen(FocusCause cause);
    /* No screen in full screen, none grabbing: before a dialog */
    void leaveScreens();

    void start();
    void togglePause();
    void shutDown();
    void reset();
    void forceOff();
    void cloneVm();
    void remove();
    /* Start, with -loadvm @snapshot */
    void startFrom(const QString &snapshot);
    void showLog();
    void showWindow();
    void showCommandLine();
    void toggleFullScreen();
    void sendCtrlAltDel();
    void releaseInput();
    /*
     * Ctrl+Alt+1/2/3 from anywhere in the app, the VM's screens included:
     * the view of the console shown (a VM with the nested desktop in this
     * window), whatever character the layout gives the key.  True when the
     * key was one of them and went there.
     */
    bool switchViewByKey(const QKeyEvent *event);

    VmStore *m_store;
    /* The list of VMs and the tasks' button under it */
    Sidebar *m_sidebar;
    QListWidget *m_list;
    QStackedWidget *m_right;
    VmPane *m_pane;
    VmDetails *m_details;
    QSplitter *m_splitter;
    QLabel *m_qemuStatus;
    /* How many VMs run, or which the closing window waits for: a widget of
       its own, so that the status bar's messages keep their time */
    QLabel *m_running;
    /* The consoles, by VM id, in the stack the Console tab shows */
    QStackedWidget *m_consoles;
    QWidget *m_noConsole;
    QHash<QString, VmConsole *> m_consoleOf;
    /* Whether the keys go to the VM */
    QLabel *m_input;
    /* What the selected VM takes of the host, how smoothly it runs */
    StatusStats *m_stats;
    /* Why the last run of a VM ended with an error, by id */
    QHash<QString, QString> m_errors;
    /* The state of each VM before its latest change */
    QHash<QString, VmRunner::State> m_states;
    QHash<QString, VmRunner::State> m_endedFrom;
    /* Started from here, not yet running */
    QSet<QString> m_starting;
    /* Waiting for access to their USB devices, to start */
    QSet<QString> m_askingUsb;
    /* The snapshot the next start of a VM starts from, by id */
    QHash<QString, QString> m_loadvm;
    /* The VMs' QEMU when a build of Vitrine's began, to tell whether it changed */
    QString m_qemuBefore;
    UpdateNotifier *m_updates;
    /* The selection left a VM with changes, to ask about */
    bool m_leaving = false;
    /*
     * VMs asked to shut down as the window closed: it closes once they are
     * off, their screens shown meanwhile, as a guest may ask before it
     * shuts down (KDE's logout screen)
     */
    QSet<QString> m_closeAfter;
    /* Of those, the VMs still starting when asked: asked once they run */
    QSet<QString> m_shutDownOnceUp;
    /* Since the last mouse press in the window */
    QElapsedTimer m_lastPress;
    /* Since the last key press in the list of VMs */
    QElapsedTimer m_lastListKey;

    QAction *m_new;
    QAction *m_import;
    QAction *m_build;
    QAction *m_settings;
    QAction *m_start;
    QAction *m_showWindow;
    QAction *m_pause;
    QAction *m_shutDown;
    QAction *m_reset;
    QAction *m_forceOff;
    QAction *m_clone;
    QAction *m_remove;
    QAction *m_log;
    QAction *m_folder;
    QAction *m_command;
    QAction *m_fullScreen;
    QAction *m_library;
    QAction *m_ctrlAltDel;
    QAction *m_releaseInput;
    /* The nested desktop's views of the console: Boot, Screen, Desktop */
    QList<QAction *> m_views;
    QAction *m_preferences;
    QAction *m_reference;
    QAction *m_quit;
    /* The one bar under the title: menus, the VM's power buttons, its tabs */
    MainBar *m_bar;
    /* In the bar: collapses or expands the list (m_library) */
    QAction *m_listButton;
    /* The window's menus, in the bar */
    QMenuBar *m_menus;
    QAction *m_openMenu;
    /* Host USB devices for the selected VM while it runs */
    UsbMenu *m_usb;
};
