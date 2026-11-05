// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "gtk/vmsummary.h"

class GpuContexts;
class QemuDocs;
class QTimer;
class Vm;
class VmConsole;

/*
 * The page of the VM selected in the sidebar, the content of the window:
 * its header bar with the power actions (the window's actions: win.start,
 * win.pause, win.shut-down, win.reset, win.force-off) and a menu of the
 * others, a banner for notices, and two views:
 *
 *  - Console: what the VM is and Start while it is off (the Qt app's
 *    Console home), its screen while it runs in vitrine's window, else
 *    what shows it (its own window, none);
 *  - Details: what it is, in groups of property rows.
 *
 * The screen is the VM's console's (VmConsole: its VmDisplay, or the
 * guest's nested desktop), which MainWindow keeps for the VM's life: each
 * console's widget is added once (addConsole()), and the selected VM's
 * shows (setConsole()).  Notices are a banner over the views, not toasts:
 * what GTK draws over the screen is hidden under its surface.  While the
 * screen shows they float over it instead (a popover, a window of its own
 * above the screen), as a banner would resize the guest's screen twice.
 */
class VmPage
{
public:
    enum class View { Console, Details };

    VmPage();
    ~VmPage();
    VmPage(const VmPage &) = delete;
    VmPage &operator=(const VmPage &) = delete;

    /* The pane: an AdwToolbarView */
    GtkWidget *widget() const { return m_root; }
    /* Its header bar, which the window adds the sidebar's button to */
    AdwHeaderBar *headerBar() const { return m_header; }
    /*
     * Narrow, as @narrow applies: the VM's name over the page, its views'
     * switcher at the bottom, where the header bar has no room for it
     */
    void addNarrowSetters(AdwBreakpoint *narrow);

    Vm *vm() const;
    /* @error: why its last run ended with an error, until the next start; empty for none */
    void setVm(Vm *vm, const QString &error = {});
    /* The state, the actions' look, the views: after a change of the VM */
    void refresh();
    void setError(const QString &error);

    View view() const;
    void setView(View view);

    /* A notice over the views for @timeoutMs, as the Qt app's status bar shows it */
    void notify(const QString &text, int timeoutMs);
    /* The notice goes, if it is @text */
    void clearNotice(const QString &text);
    /* What the closing window waits for, over the notices until it closes; empty: none */
    void setClosing(const QString &text);

    /*
     * Where the VM's screen goes: the consoles, shown while the VM runs in
     * vitrine's window (VmRunner::displaySocket())
     */
    GtkWidget *screenArea() const { return m_screen; }
    /* A console's widget, in the place of the screen, for as long as its VM exists */
    void addConsole(VmConsole *console);
    void removeConsole(VmConsole *console);
    /* The selected VM's console, null for none: its page and its screen */
    void setConsole(VmConsole *console);
    VmConsole *console() const { return m_consoleShown; }
    /* The VM's screen is in sight: the Console view, on its screen */
    bool screenShown() const;
    /* The console's page, its screen, its keys or its views changed */
    void refreshConsole() { updateConsole(); }
    /* The Console view came in sight, chosen or set */
    std::function<void()> consoleShown;
    /* An in-window dialog is over the screen, or no longer (VmConsole::setCovered()) */
    void setCovered(bool covered);
    bool isCovered() const { return m_covered; }
    /* The screen came in sight, or went out of it (screenShown()) */
    std::function<void(bool shown)> screenShownChanged;

    /* More on the Console's home page, under the error, over Start: the guest tools' state */
    void addHomeWidget(GtkWidget *widget);
    /* A group of the Details, before the VM's own: the guest tools' */
    void addDetailsGroup(AdwPreferencesGroup *group);

private:
    void buildHeader();
    GtkWidget *buildConsole();
    void updateConsole();
    void updateDetails();
    void updateButtons();
    void showConsolePage(const char *name);
    /* The notice in the banner, or over the screen while it shows */
    void placeNotice();

    QObject m_qt;
    QPointer<Vm> m_vm;
    QemuDocs *m_docs = nullptr;
    GpuContexts *m_contexts;
    QTimer *m_growing;
    QTimer *m_noticeTimer;
    QString m_error;
    bool m_covered = false;
    /* the selected VM's console */
    VmConsole *m_consoleShown = nullptr;
    /* the notice shown, if any: in the banner, or over the screen (m_toast) */
    QString m_noticeText;
    bool m_noticeShown = false;
    /* the screen in sight, as screenShownChanged last said */
    bool m_screenWasShown = false;

    GtkWidget *m_root;
    AdwHeaderBar *m_header;
    AdwViewStack *m_views;
    /* narrow: the title, and the views' switcher at the bottom */
    AdwWindowTitle *m_title;
    AdwViewSwitcherBar *m_switcherBar;
    AdwBanner *m_notice;
    AdwBanner *m_closing;
    /* Start while off, Pause or Resume and Shut Down while on */
    GtkWidget *m_start;
    GtkWidget *m_pause;
    GtkWidget *m_shutDown;
    /* while the screen shows: Full Screen, and the Send Key menu */
    GtkWidget *m_fullScreen;
    GtkWidget *m_sendKey;
    /* Console */
    GtkStack *m_console;
    AdwStatusPage *m_home;
    GtkWidget *m_homeError;
    AdwStatusPage *m_message;
    GtkWidget *m_retry;
    GtkWidget *m_screen;
    /* the consoles' widgets, the selected VM's shown; m_noConsole without */
    GtkStack *m_consoles;
    GtkWidget *m_noConsole;
    /* the notice over the screen */
    GtkWidget *m_toast;
    GtkWidget *m_toastLabel;
    /* Details: rebuilt when its rows change, else their values updated */
    AdwPreferencesPage *m_details;
    /* its header: the VM's name and state beside its icon */
    GtkWidget *m_detailsName;
    GtkWidget *m_detailsState;
    AdwBanner *m_detailsNote;
    AdwPreferencesGroup *m_errorGroup;
    AdwActionRow *m_errorRow;
    QList<AdwPreferencesGroup *> m_groups;
    QList<QList<AdwActionRow *>> m_rows;
    QList<VmSummary::Section> m_shown;
};
