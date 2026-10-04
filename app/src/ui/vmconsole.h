// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QPointer>
#include <QWidget>

#include "vmview/stats.h"

class Banner;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class Vm;
class VmView;

/*
 * A VM's console, VMware style: while the VM is off, a page to start it;
 * while it runs in vitrine's window, its screen (VmView); while it runs in
 * a window of its own (QEMU's SDL display), the way to that window.
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
    /* The screen of the running VM, once attached; else null */
    VmView *view() const;
    bool isFullScreen() const;
    void setFullScreen(bool on);
    /* Gives the keyboard to the guest, when the screen shows */
    void focusScreen();
    /* Why the last run ended with an error, until the next; empty for none */
    void setError(const QString &error);
    /* Of the last second, while the screen shows */
    const Stats::Summary &stats() const;

signals:
    void startRequested();
    void settingsRequested();
    void showWindowRequested();
    void showLogRequested();
    /* The page, the screen, its grab or keyboard focus, or full screen changed */
    void changed();
    /* The screen takes the guest's input now: it appeared, or the VM runs again */
    void screenReady();
    void statsChanged();

private:
    void update();
    void updateHome();
    void attach();
    void detach();
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
    QVBoxLayout *m_screenLayout;
    /* OwnWindow */
    QLabel *m_ownWindowText;

    /* attach() again, while QEMU opens the display's socket */
    QTimer *m_retry;
    int m_attempts = 0;
    QTimer *m_statsTimer;
    Stats::Summary m_stats;
};
