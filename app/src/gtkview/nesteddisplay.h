// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* GTK's first: Qt's keywords are macros, which sigc++'s names would meet
   (src/gtk/gtkbase.h) */
#include <sigc++/signal.h>

#include <QString>

#include <functional>

typedef struct _GtkWidget GtkWidget;

/*
 * The nested desktop in the GTK app (NestedDesktop::Mode::Integrated): the
 * guest's KDE session - its nested KWin's window - in the console's
 * Desktop view, or alone on the screen.  The GTK counterpart of
 * src/nested's NestedView, on the same library (src/nested/vmview.h): it
 * serves the socket QEMU's cross-domain connects to, and passes the guest's
 * Wayland protocol on to the compositor over GDK's own connection, so that
 * the guest's window is a part of vitrine's.
 *
 * This is what the console needs of it (VmConsole), which tests replace
 * with a stand-in: the real one (create()) needs GDK's Wayland backend and
 * a guest.
 */
class NestedDisplay
{
public:
    /* Makes the consoles' views: create() but in tests */
    using Factory = std::function<NestedDisplay *(const QString &socket, QString *error)>;

    /*
     * Serves @socket (an old socket there is replaced) on GDK's default
     * display; null with @error, as on a display that is not Wayland's
     */
    static NestedDisplay *create(const QString &socket, QString *error);
    /* Stops serving: the guest's window is closed first, which ends its
       session, as its nested KWin quits */
    virtual ~NestedDisplay() = default;
    NestedDisplay(const NestedDisplay &) = delete;
    NestedDisplay &operator=(const NestedDisplay &) = delete;

    /* The socket QEMU's cross-domain connects to */
    virtual QString socket() const = 0;
    /* The view in the console; owned by the display, out of any parent when it goes */
    virtual GtkWidget *widget() const = 0;
    /* The guest's desktop alone on the screen, in a window of its own */
    virtual bool isFullScreen() const = 0;
    virtual void setFullScreen(bool on) = 0;
    /* Off: the guest gets no keys, and vitrine's shortcuts work */
    virtual void setInputEnabled(bool on) = 0;
    /* The guest gets the keys now */
    virtual bool hasKeyboard() const = 0;
    /* GTK's focus to the view: the keys to the guest */
    virtual void focus() = 0;
    /* The guest's connections to the socket, and its window */
    virtual int connections() const = 0;
    virtual bool isMapped() const = 0;
    /*
     * Something GTK draws in the window covers the view (an in-window
     * dialog): the guest's window, over anything GTK draws, leaves vitrine's
     * window until uncovered, the view black meanwhile.  Its session runs
     * on.  Full screen, it is in a window of its own: nothing to do.
     */
    virtual void setCovered(bool on) = 0;

    sigc::signal<void(int)> connectionsChanged;
    sigc::signal<void(bool)> mappedChanged;
    /* The guest asks for full screen, or a window (its Ctrl+Alt+F) */
    sigc::signal<void(bool)> fullScreenRequested;
    /* Ctrl+Alt+F while no guest window is there to ask */
    sigc::signal<void()> fullScreenToggled;
    sigc::signal<void(bool)> fullScreenChanged;
    /* hasKeyboard() changed */
    sigc::signal<void()> keyboardChanged;

protected:
    NestedDisplay() = default;
};
