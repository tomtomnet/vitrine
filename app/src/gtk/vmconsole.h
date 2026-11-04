// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"
#include "gtkview/nesteddisplay.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "core/consoleviews.h"
#include "vmview/stats.h"

class QTimer;
class Vm;
class VmDisplay;

/*
 * A VM's console in the GTK app, as the Qt app's VmConsole: while the VM
 * runs in vitrine's window, its screen (gtkview's VmDisplay); while it runs
 * in a window of QEMU's own, a word of that; while it starts, connects to
 * its screen or cannot show it, a message.  VmPage shows its page() for the
 * selected VM, and its widget() where the screen goes.
 *
 * A VM whose nested desktop shows in vitrine's window (NestedDesktop::
 * Mode::Integrated) has two views, which ConsoleViews tells, as they come
 * and go and as the user picks them (the machine menu, Ctrl+Alt+1/2):
 * Screen, the VM's screen through the D-Bus display, and Desktop, the
 * guest's session (NestedDisplay).  The keys, the pointer and full screen
 * follow the view shown.  The console serves the nested desktop's socket
 * for the whole run (serveNested()), shown or not: the guest's session
 * lives as long as it does.
 *
 * MainWindow keeps one console per VM, for as long as the VM exists: the
 * screen is a surface a render thread draws, which stays with its console
 * as other VMs are selected (its widget, in VmPage's stack of consoles,
 * unmapped meanwhile), as in the Qt app.
 */
class VmConsole
{
public:
    enum class Page {
        Home,       // off, starting or stopping
        Message,    // starting, connecting, cannot connect, no screen
        Screen,     // the screen, in this window or full screen
        OwnWindow,  // in QEMU's window
    };

    explicit VmConsole(Vm *vm);
    /* Detaches the screen first: its render thread draws into the window */
    ~VmConsole();
    VmConsole(const VmConsole &) = delete;
    VmConsole &operator=(const VmConsole &) = delete;

    Vm *vm() const;
    Page page() const;
    /* The Message page's title and text, empty for none */
    QString messageTitle() const;
    QString messageText() const;
    /* A wait, under way (Starting, Connecting): a spinner with it */
    bool isWaiting() const;
    /* The screen could not show: Try Again (retry()) */
    bool canRetry() const;
    void retry();

    /* The views, in a stack: the screen's and the guest's desktop's; VmPage's to place */
    GtkWidget *widget() const;
    /* The screen of the running VM through the D-Bus display, once attached; else null */
    VmDisplay *display() const;
    /* The view shown is there: the screen attached, or the guest's desktop */
    bool hasScreen() const;
    bool isFullScreen() const;
    void setFullScreen(bool on);
    /* Gives the keyboard to the guest, when the screen shows */
    void focusScreen();
    /* The view shown takes the keys: the screen's or the guest's desktop's */
    bool hasKeyboard() const;
    /* The view shown can take the keys now: there, and the VM takes input */
    bool takesInput() const;
    /* The screen's grab (Ctrl+Alt+G): the desktop's shortcuts go to the guest */
    bool grabbed() const;
    QString grabState() const;
    void setGrab(bool on);
    /* An in-window dialog is over the console, or no longer (VmPage::setCovered()) */
    void setCovered(bool on);
    /* No full screen and no grab: before a dialog, which they would hide */
    void leaveScreen();
    /* Of the last second, while the screen shows */
    const Stats::Summary &stats() const;

    /* The nested desktop in this window: its views, while the VM runs so */
    bool hasViews() const;
    ConsoleViews::View currentView() const;
    /* The user's choice of view */
    void showView(ConsoleViews::View view);
    /* The guest's desktop is the view shown */
    bool desktopShown() const;
    /* The guest's desktop, while the VM runs with it in this window; else null */
    NestedDisplay *nestedView() const;
    /* The guest's desktop session is up: it ends when vitrine quits */
    bool hasNestedSession() const;
    /*
     * Serves the nested desktop's @socket for the VM's run, before QEMU
     * starts (VmRunner::setNestedHost): the guest's desktop then shows in
     * the Desktop view.  False with @error without Wayland.
     */
    bool serveNested(const QString &socket, QString *error);
    /* Tests: their stand-in for the guest's desktop; empty for NestedDisplay::create() */
    static void setNestedFactory(const NestedDisplay::Factory &factory);

    /* The page, the screen, its grab or keyboard focus, full screen, or the view changed */
    std::function<void()> changed;
    /* The screen takes the guest's input now: it appeared, or the VM runs again */
    std::function<void()> screenReady;

private:
    void update();
    void attach();
    void detach();
    /* The nested desktop's view goes, with the run */
    void dropNested();
    /* The guest restarted: the nested desktop's socket served anew */
    void renewNested();
    /* The view of m_views: its page, the keys */
    void applyView();
    void showMessage(const QString &title, const QString &text, bool waiting = false,
                     bool retry = false);
    void setPage(Page page);
    void notifyChanged();
    void notifyScreenReady();

    QObject m_qt;
    QPointer<Vm> m_vm;
    VmDisplay *m_display = nullptr;
    NestedDisplay *m_nested = nullptr;
    ConsoleViews *m_views;
    Page m_page = Page::Home;
    QString m_messageTitle;
    QString m_messageText;
    bool m_waiting = false;
    bool m_retry = false;
    bool m_covered = false;

    /* the views: "screen", the display's widget in m_screenBox; "desktop", the guest's */
    GtkWidget *m_stack;
    GtkWidget *m_screenBox;
    GtkWidget *m_desktopBox;

    /* attach() again, while QEMU opens the display's socket */
    QTimer *m_retryTimer;
    int m_attempts = 0;
    /* the display's numbers pile up until taken */
    QTimer *m_statsTimer;
    Stats::Summary m_stats;
};
