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
class QWidget;
class DBusDisplay;
class DisplayWindow;
class Listener;
class Renderer;
class WaylandExtras;

/*
 * The screen of a running VM, through QEMU's D-Bus display: the client in
 * the files beside this one, behind one class.
 *
 * attach() connects to the VM's display monitor (VmRunner::displaySocket):
 * getfd + add_client hand QEMU one end of a socket for a peer-to-peer D-Bus
 * connection, on which the view registers its listener.  widget() shows the
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
    /* Gives the keyboard to the guest */
    void focus();
    void sendCtrlAltDel();
    /* Frame statistics since the last call */
    Stats::Summary takeStats();
    /* The guest's screen, e.g. 3840x2160; empty before its first frame */
    QSize guestSize() const;
    QString vmName() const;

Q_SIGNALS:
    void grabChanged();
    void fullScreenChanged(bool on);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void detach();
    void createWindow(bool fullScreen);
    void destroyWindow();
    void updateHostActive();

    Options m_opts;
    Stats m_stats;
    FrameMailbox m_mailbox;
    WaylandExtras *m_wayland = nullptr;
    DBusDisplay *m_dbus = nullptr;
    std::unique_ptr<Listener> m_listener;
    Renderer *m_renderer = nullptr;
    DisplayWindow *m_window = nullptr;
    QPointer<QWidget> m_host;           // what widget() returns
    QWidget *m_container = nullptr;     // embeds m_window, in m_host
    QLabel *m_placeholder = nullptr;    // in m_host while full screen
    bool m_fullScreen = false;
    /* what the guest said last, for a new window */
    QSize m_guestSize;
    QImage m_cursor;
    int m_cursorHotX = 0;
    int m_cursorHotY = 0;
    bool m_cursorVisible = true;
};
