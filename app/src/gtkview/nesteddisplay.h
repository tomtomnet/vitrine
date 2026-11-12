// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* GTK's first: Qt's keywords are macros, which sigc++'s names would meet
   (src/gtk/gtkbase.h) */
#include <sigc++/signal.h>

#include <QString>

#include <functional>
#include <string>
#include <vector>

#include <sys/types.h>

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
    /* The view has the keyboard: its focus in the active window, or its
       full-screen window active, and input on */
    virtual bool hasKeyboard() const = 0;
    /* And a key typed now goes to the guest: the pointer is on the guest's
       window, or the guest captured the desktop's shortcuts */
    virtual bool keysToGuest() const { return hasKeyboard(); }
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

    /* A thread of this process the guest's desktop goes through */
    struct Thread {
        enum class Role { Main, Library };
        Role role;
        pid_t tid;
        bool operator==(const Thread &) const = default;
    };
    /*
     * Those threads, for real time while the VM is in front
     * (HostSettings::setViewThreads): the library's own (src/nested/vmview.h:
     * the guest's requests to the compositor, its events back), known from
     * its first callback, as the guest connects; and the app's main thread,
     * GDK's reader of the display - libwayland reads a display only once
     * every thread that prepared to read it has, so the library's reads wait
     * for GDK's.  Empty until the library's thread is known.  Each change
     * goes to stderr as threadLine() and to threadsChanged.
     */
    virtual std::vector<Thread> threads() const { return {}; }
    /* Their ids, for real time: none with VITRINE_VIEW_RT=0 */
    std::vector<pid_t> realtimeThreads() const;
    /* "vitrine: desktop: threads:", then " main=TID" and " library=TID":
       a stable line for benches (src/gtkview/README.md) */
    static std::string threadLine(const std::vector<Thread> &threads);

    sigc::signal<void(int)> connectionsChanged;
    sigc::signal<void(bool)> mappedChanged;
    /* The guest asks for full screen, or a window (its Ctrl+Alt+F) */
    sigc::signal<void(bool)> fullScreenRequested;
    /* Ctrl+Alt+F while no guest window is there to ask */
    sigc::signal<void()> fullScreenToggled;
    sigc::signal<void(bool)> fullScreenChanged;
    /* hasKeyboard() or keysToGuest() changed */
    sigc::signal<void()> keyboardChanged;
    /* threads() changed */
    sigc::signal<void()> threadsChanged;

protected:
    NestedDisplay() = default;
};
