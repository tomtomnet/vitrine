// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QPointer>
#include <QWidget>

#include "core/consoleviews.h"
#include "nested/desktopview.h"
#include "vmview/stats.h"

class Banner;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;
class Vm;
class VmView;

/*
 * A VM's console, VMware style: while the VM is off, a page to start it;
 * while it runs in vitrine's window, its screen (VmView); while it runs in
 * a window of its own (QEMU's SDL or GTK display), the way to that window.
 *
 * A VM whose nested desktop shows in vitrine's window (NestedDesktop::
 * Mode::Integrated) has three views, which the Console tab's menu chooses,
 * as the View menu and Ctrl+Alt+1/2/3 do (ConsoleViews tells which, and
 * when they change by themselves): Boot and Screen, QEMU's consoles 0 and
 * 1 through the D-Bus display (the VGA card's screen, the virtio-gpu
 * card's), and Desktop, the guest's KDE session (NestedView).  The keys
 * and the pointer follow the view shown, full screen too.  The console
 * serves the nested desktop's socket for the whole run (serveNested()),
 * shown or not: the guest's session lives as long as it does.
 *
 * MainWindow keeps one console per VM, in a stack on the Console tab, for
 * as long as the VM exists: the screen is a native subsurface drawn by a
 * render thread, which must not be re-created under it, as moving the
 * widget to new pages each time the VM is selected would do.
 */
class VmConsole : public QWidget
{
    Q_OBJECT

public:
    enum class Page {
        Home,       // off, starting or stopping
        Message,    // starting, connecting, cannot connect, no screen
        Screen,     // the screen, in this window or full screen
        OwnWindow,  // in QEMU's window
    };

    explicit VmConsole(Vm *vm, QWidget *parent = nullptr);
    /* Detaches the screen first: the render thread draws into its window */
    ~VmConsole() override;

    Vm *vm() const;
    Page page() const;
    /* The screen of the running VM through the D-Bus display, once attached; else null */
    VmView *view() const;
    bool isFullScreen() const;
    void setFullScreen(bool on);
    /* Gives the keyboard to the guest, when the screen shows */
    void focusScreen();
    /* The view shown takes the keys: the screen's or the guest's desktop's */
    bool hasKeyboard() const;
    /* The view shown can take the keys now: there, and the VM takes input */
    bool takesInput() const;
    /* Where the view shown is, for the keyboard focus; null without */
    QWidget *screenWidget() const;
    /* Why the last run ended with an error, until the next; empty for none */
    void setError(const QString &error);
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
    DesktopView *nestedView() const;
    /* The guest's desktop session is up: it ends when vitrine quits */
    bool hasNestedSession() const;
    /*
     * Serves the nested desktop's @socket for the VM's run, before QEMU
     * starts (VmRunner::setNestedHost): the guest's desktop then shows in
     * the Desktop view.  False with @error without Wayland.
     */
    bool serveNested(const QString &socket, QString *error);
    /* Tests: their stand-in for the guest's desktop's view; empty for NestedView */
    static void setDesktopViewFactory(const DesktopView::Factory &factory);

signals:
    void startRequested();
    void settingsRequested();
    void showWindowRequested();
    void showLogRequested();
    /* The page, the screen, its grab or keyboard focus, full screen, or the view changed */
    void changed();
    /* The screen takes the guest's input now: it appeared, or the VM runs again */
    void screenReady();
    void statsChanged();

private:
    void update();
    void updateHome();
    void attach();
    void detach();
    /* The nested desktop's view goes, with the run */
    void dropNested();
    /* The view of m_views: its page, the D-Bus display's console, the keys */
    void applyView();
    void showMessage(const QString &title, const QString &text, const QString &button = {});
    void setPage(Page page);

    QPointer<Vm> m_vm;
    VmView *m_view = nullptr;
    QStackedWidget *m_pages;
    /* Home */
    QLabel *m_name;
    QLabel *m_state;
    Banner *m_error;
    QPushButton *m_start;
    QLabel *m_summary;
    /* Message */
    QLabel *m_messageTitle;
    QLabel *m_messageText;
    QPushButton *m_messageButton;
    /* Screen: nothing over it, which would resize the guest */
    QWidget *m_screen;
    QStackedWidget *m_viewPages;
    QWidget *m_dbusPage;
    QVBoxLayout *m_screenLayout;
    QWidget *m_desktopPage;
    QVBoxLayout *m_desktopLayout;
    /* OwnWindow */
    QLabel *m_ownWindowText;

    /* the nested desktop in this window */
    ConsoleViews *m_views;
    DesktopView *m_nested = nullptr;

    /* attach() again, while QEMU opens the display's socket */
    QTimer *m_retry;
    int m_attempts = 0;
    QTimer *m_statsTimer;
    Stats::Summary m_stats;
};
