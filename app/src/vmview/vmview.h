// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>

#include "vmview/common.h"
#include "vmview/framemailbox.h"
#include "vmview/stats.h"

class QLabel;
class QTimer;
class QWidget;
class DBusDisplay;
class DisplayWindow;
class Listener;
class Renderer;
class VmClipboard;
class WaylandExtras;

/*
 * The screen of a running VM, through QEMU's D-Bus display: the client in
 * the files beside this one, behind one class.
 *
 * attach() connects to the VM's display monitor (VmRunner::displaySocket):
 * getfd + add_client hand QEMU one end of a socket for a peer-to-peer D-Bus
 * connection, on which the view registers its listener and shares the
 * clipboard (VmClipboard).  widget() shows the
 * screen in a layout, as a Wayland subsurface.  Full screen moves it to a
 * window of its own, the only way the compositor can put the guest's
 * buffers on the screen as they are: the switch re-creates the window and
 * its render thread, and keeps the D-Bus connection and the listener.
 *
 * One view per run of the VM: QEMU serves one display client at a time.
 */
class VmView : public QObject
{
    Q_OBJECT

public:
    explicit VmView(QObject *parent = nullptr);
    /* Detaches */
    ~VmView() override;

    bool attach(const QString &monitorSocket, QString *error);
    bool isAttached() const;

    /* Where the screen shows, but in full screen; owned by the view */
    QWidget *widget() const;
    bool isFullScreen() const;
    void setFullScreen(bool on);

    /* Whether the guest gets the keys the desktop would take, as Alt+Tab */
    bool grabbed() const;
    QString grabState() const;
    void setGrab(bool on);
    /* Whether the keys go to the guest: the screen has the focus in the
       active window, and input is on */
    bool hasKeyboard() const;
    /*
     * Off while QEMU drops input (the VM paused): the screen then takes no
     * keys, which go to the window's shortcuts instead (Resume among
     * them), and lets go of those held and of the grab.  On by default.
     * Full screen, the window still takes them: Ctrl+Alt+F brings it back.
     */
    void setInputEnabled(bool on);
    bool inputEnabled() const;
    /* Gives the keyboard to the guest */
    void focus();
    void sendCtrlAltDel();
    /* Frame statistics since the last call */
    Stats::Summary takeStats();
    /* The guest's screen, e.g. 3840x2160; empty before its first frame */
    QSize guestSize() const;
    QString vmName() const;

Q_SIGNALS:
    /* grabbed(), grabState(), hasKeyboard() or inputEnabled() changed */
    void grabChanged();
    void fullScreenChanged(bool on);
    /*
     * QEMU answered the first size and refresh rate of the screen the view
     * gave it for the guest (UIInfo.Apply), taken or refused: once per
     * attach().  A guest paused until then may run (VmRunner::displayReady).
     */
    void screenInfoApplied();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void detach();
    void createWindow(bool fullScreen);
    void destroyWindow();
    void updateHostActive();
    /* What the render thread does with the guest's buffers while it does not draw */
    void releaseUndrawn();
    /* grabChanged() if the grab changed: DisplayWindow does not signal the
       grab it takes or leaves itself (Ctrl+Alt+G, a click with a relative mouse) */
    void checkGrab();
    /* The first size and refresh rate for the guest, before the window has a size */
    void sendFirstUiInfo();
    /* The window's screen, its refresh rate or its pixel ratio changed */
    void screenChanged();
    void watchScreen();

    Options m_opts;
    /* on the heap: it outlives the view while D-Bus calls complete (detach) */
    std::unique_ptr<Stats> m_stats;
    FrameMailbox m_mailbox;
    WaylandExtras *m_wayland = nullptr;
    DBusDisplay *m_dbus = nullptr;
    std::unique_ptr<Listener> m_listener;
    VmClipboard *m_clipboard = nullptr;
    Renderer *m_renderer = nullptr;
    DisplayWindow *m_window = nullptr;
    QPointer<QWidget> m_host;           // what widget() returns
    QWidget *m_container = nullptr;     // embeds m_window, in m_host
    QLabel *m_placeholder = nullptr;    // in m_host while full screen
    bool m_fullScreen = false;
    bool m_hasKeyboard = false;
    bool m_inputEnabled = true;
    /* releaseUndrawn() until the window is first exposed, or for good if
       its render thread failed */
    QTimer *m_undrawn = nullptr;
    bool m_exposedOnce = false;
    bool m_renderFailed = false;
    /* UIInfo again once the screen settled (screenChanged()) */
    QTimer *m_uiInfo = nullptr;
    QMetaObject::Connection m_refreshWatch;  // the window's screen's refresh rate
    bool m_uiInfoApplied = false;   // screenInfoApplied() was signalled
    QString m_grabState;    // as last signalled
    /* what the guest said last, for a new window */
    QSize m_guestSize;
    QImage m_cursor;
    int m_cursorHotX = 0;
    int m_cursorHotY = 0;
    bool m_cursorVisible = true;
};
