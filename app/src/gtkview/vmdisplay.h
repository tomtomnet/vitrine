// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* GTK's first: Qt's keywords are macros, which sigc++'s names would meet
   (src/gtk/gtkbase.h) */
#include <sigc++/connection.h>
#include <sigc++/signal.h>

#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>

#include "fullscreenorder.h"
#include "vmview/common.h"
#include "vmview/framemailbox.h"
#include "vmview/stats.h"

namespace Gtk {
class Box;
class Widget;
class Window;
}
typedef struct _GtkWidget GtkWidget;
class DBusDisplay;
class DisplaySurface;
class DisplayWidget;
class GtkVmClipboard;
class Listener;
class QImage;
class Renderer;

/*
 * The screen of a running VM in the GTK app, through QEMU's D-Bus display:
 * the GTK counterpart of src/vmview's VmView, with the same calls.
 *
 * attach() connects to the VM's display monitor (VmRunner::displaySocket):
 * getfd + add_client hand QEMU one end of a socket for a peer-to-peer D-Bus
 * connection, on which the view registers its listener (src/vmview, as it
 * is) and shares the clipboard.  widget() shows the screen in a layout: the
 * frames on a Wayland surface of their own over the widget (DisplayWidget),
 * drawn by the render thread (src/vmview's Renderer, with src/gtkview/qtgui
 * for the Qt GUI classes it uses).  Full screen moves it to a window of its
 * own, the only way the compositor can put the guest's buffers on the
 * screen as they are: the switch makes the widget and its render thread
 * again, and keeps the D-Bus connection and the listener.
 *
 * Everything runs on the GUI thread, GTK's main loop on the default GLib
 * context, which also completes GDBus's calls; nothing needs Qt's event
 * loop.  One view per run of the VM: QEMU serves one display client at a
 * time.
 */
class VmDisplay
{
public:
    VmDisplay();
    /* Detaches */
    ~VmDisplay();
    VmDisplay(const VmDisplay &) = delete;
    VmDisplay &operator=(const VmDisplay &) = delete;

    /*
     * @guestWaits: the guest waits, paused, for its screen (VmRunner::
     * waitsForDisplay): QEMU gets a size and the refresh rate at once, the
     * size of the window around the view until it has its own.  Else only
     * once the view has a size.  @console: QEMU's console by its index, -1
     * for the first graphic one.
     */
    bool attach(const QString &monitorSocket, QString *error, bool guestWaits = false,
                int console = -1);
    bool isAttached() const;
    int console() const;

    /* Where the screen shows, but in full screen; owned by the view */
    Gtk::Widget *widget() const;
    bool isFullScreen() const { return m_fullScreen; }
    /* On: once the window the view is in is shown and active, if it is
       shown and not yet active (FullScreenOrder) */
    void setFullScreen(bool on);
    /*
     * Whether the guest's flips wait for the view to give back the buffer
     * shown before (GuestTools::flipsWaitForRelease()); true until set.
     * In full screen the view then copies the guest's frames: its own
     * buffer would go to the screen as it is and come back only after the
     * compositor's next flip, too late for a guest drawing at every refresh
     * (half rate at 240 Hz on an Arc Pro B50, the research's window drags
     * of 2026-11-08).  VITRINE_VIEW_FULL_SCREEN_ZERO_COPY=1 or 0 decides
     * instead, for A/B runs.
     */
    void setFlipsWaitForRelease(bool on);
    /* In full screen, the guest's buffers go to the screen as they are */
    bool fullScreenZeroCopy() const;

    /* Whether the guest gets the keys the desktop would take, as Alt+Tab */
    bool grabbed() const;
    QString grabState() const;
    void setGrab(bool on);
    /* Whether the keys go to the guest: the screen has the focus in the
       active window, and input is on */
    bool hasKeyboard() const { return m_hasKeyboard; }
    /* Off while QEMU drops input (the VM paused): no keys taken but the
       Ctrl+Alt+F hotkey, the others go to the window's shortcuts */
    void setInputEnabled(bool on);
    bool inputEnabled() const { return m_inputEnabled; }
    /*
     * Something GTK draws in the window covers the screen, as an in-window
     * dialog (AdwDialog) or a toast over the VM page: the frames' surface,
     * over anything GTK draws, leaves the window, and the widget shows a
     * still of the guest's last frame until uncovered.  The guest runs on.
     * Full screen, the screen is in a window of its own: nothing to do.
     */
    void setCovered(bool on);
    bool isCovered() const { return m_covered; }
    /* Gives the keyboard to the guest */
    void focus();
    /* Frame statistics since the last call */
    Stats::Summary takeStats();
    /* The guest's screen; empty before its first frame */
    QSize guestSize() const { return m_guestSize; }
    QString vmName() const;
    /* The widget the frames show over (tests, the test program) */
    DisplayWidget *displayWidget() const { return m_widget; }

    /* grabbed(), grabState(), hasKeyboard() or inputEnabled() changed */
    sigc::signal<void()> grabChanged;
    sigc::signal<void(bool)> fullScreenChanged;
    /* QEMU answered the first size and refresh rate given for the guest */
    sigc::signal<void()> screenInfoApplied;

private:
    void detach();
    void createWindow(bool fullScreen);
    void destroyWindow();
    void updateHostActive();
    void releaseUndrawn();
    void checkGrab();
    void sendFirstUiInfo(bool guestWaits);
    void screenChanged();
    void startUndrawn();
    void sayFullScreenPath();

    Options m_opts;
    /* on the heap: it outlives the view while D-Bus calls complete (detach) */
    std::unique_ptr<Stats> m_stats;
    FrameMailbox m_mailbox;
    DBusDisplay *m_dbus = nullptr;
    std::unique_ptr<Listener> m_listener;
    GtkVmClipboard *m_clipboard = nullptr;
    Renderer *m_renderer = nullptr;
    std::unique_ptr<DisplaySurface> m_surface;
    DisplayWidget *m_widget = nullptr;
    Gtk::Window *m_fullScreenWindow = nullptr;
    FullScreenOrder m_fullScreenOrder;
    Gtk::Box *m_host = nullptr;          // what widget() returns
    GtkWidget *m_placeholder = nullptr;  // in m_host while full screen
    std::shared_ptr<bool> m_alive;       // for what the other threads queue for this one
    bool m_fullScreen = false;
    bool m_flipsWait = true;
    /* VITRINE_VIEW_FULL_SCREEN_ZERO_COPY: 1 or 0, -1 when not set */
    int m_fullScreenZeroCopyEnv = -1;
    /* fullScreenZeroCopy() as last said on stderr, -1 before */
    int m_saidFullScreenZeroCopy = -1;
    bool m_covered = false;
    bool m_hasKeyboard = false;
    bool m_inputEnabled = true;
    sigc::connection m_undrawn;
    bool m_exposedOnce = false;
    bool m_renderFailed = false;
    sigc::connection m_uiInfo;
    bool m_uiInfoApplied = false;
    QString m_grabState;
    /* what the guest said last, for a new window */
    QSize m_guestSize;
    std::unique_ptr<QImage> m_cursor;
    int m_cursorHotX = 0;
    int m_cursorHotY = 0;
    bool m_cursorVisible = true;
    int m_console = -1;
};
