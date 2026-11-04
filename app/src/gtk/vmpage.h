// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include "gtk/vmsummary.h"

class GpuContexts;
class QemuDocs;
class QTimer;
class Vm;

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
 * The screen is gtk-vmview's VmDisplay, which goes into screenArea(): a
 * placeholder until then.  Notices are a banner over the views, not
 * toasts: what GTK draws over the screen is hidden under its subsurface.
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
     * Where the VM's screen goes, gtk-vmview's VmDisplay: shown while the
     * VM runs in vitrine's window (VmRunner::displaySocket())
     */
    GtkWidget *screenArea() const { return m_screen; }
    /* An in-window dialog is over the screen, or no longer (VmDisplay::setCovered()) */
    void setCovered(bool covered);
    bool isCovered() const { return m_covered; }

private:
    void buildHeader();
    GtkWidget *buildConsole();
    void updateConsole();
    void updateDetails();
    void updateButtons();
    void showConsolePage(const char *name);

    QObject m_qt;
    QPointer<Vm> m_vm;
    QemuDocs *m_docs = nullptr;
    GpuContexts *m_contexts;
    QTimer *m_growing;
    QTimer *m_noticeTimer;
    QString m_error;
    bool m_covered = false;

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
    /* Console */
    GtkStack *m_console;
    AdwStatusPage *m_home;
    GtkWidget *m_homeError;
    AdwStatusPage *m_message;
    GtkWidget *m_screen;
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
