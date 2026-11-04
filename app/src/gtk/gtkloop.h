// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QString>

#include <functional>

/*
 * The GTK app's one main loop, for the core's Qt objects and for GTK
 * alike: Qt's own, QCoreApplication::exec(), which Fedora's Qt runs on
 * GLib's default main context (QEventDispatcherGlib), the context GTK has
 * its sources on.  Each iteration dispatches both: Qt's timers, socket
 * notifiers (QProcess, QLocalSocket, QMP) and posted events; GTK's input,
 * frame clock, timeouts and idles.
 *
 * Qt's loop rather than g_application_run(): Qt deletes the objects of
 * deleteLater() only from a loop of its own.  One deleteLater'd from a
 * GTK callback while no Qt loop runs (loop level 0, scope 0) would never
 * go: the core calls deleteLater() all over, on QProcesses and sockets,
 * from callbacks of both toolkits.  Under exec() every callback runs
 * inside Qt's loop: deferred deletes, quit(), exit() and aboutToQuit()
 * work as in the Qt app (test_gtkloop shows both).
 *
 * So the AdwApplication (a GtkApplication) does not run the loop, and what
 * g_application_run() would do for it is done here: start() registers it
 * on the session bus, which runs its startup (GTK and libadwaita set up)
 * in the first instance and makes a later one a remote of the first; a
 * remote activates the first, which shows its window, and is done.
 * exec() activates (the window comes), runs Qt's loop until the last
 * window is gone or quit(), as the windows' hold on the app would end
 * g_application_run(), then says shutdown and flushes the bus.
 */
class GtkLoop
{
public:
    enum class Start {
        Primary,    // this one runs: GTK and libadwaita are set up
        Remote,     // another one runs, and was asked to show its window
        Failed,     // the error says why
    };

    explicit GtkLoop(const char *appId);
    ~GtkLoop();
    GtkLoop(const GtkLoop &) = delete;
    GtkLoop &operator=(const GtkLoop &) = delete;

    /*
     * Whether Qt's loop runs on GLib's default main context, as GTK needs
     * it to: a Qt built with GLib, and QT_NO_GLIB not set.  @why: what to
     * tell the user if not.  Once QCoreApplication is made.
     */
    static bool supported(QString *why = nullptr);

    AdwApplication *adw() const { return m_app; }
    /* The same application, for gtkmm */
    Glib::RefPtr<Gtk::Application> app() const { return m_wrapped; }

    Start start(QString *error = nullptr);
    /* Called at each activation: exec()'s first, and each later instance's */
    void setActivated(std::function<void()> activated);
    /* Activates, then runs until the last window goes, or quit(): the exit status */
    int exec();
    /* Ends exec() with @status once it is back in the loop; also before it runs */
    void quit(int status = 0);

private:
    static void activateThunk(GApplication *app, gpointer self);
    static void windowRemovedThunk(GtkApplication *app, GtkWindow *window, gpointer self);

    AdwApplication *m_app;
    Glib::RefPtr<Gtk::Application> m_wrapped;
    std::function<void()> m_activated;
    bool m_quitting = false;
    int m_status = 0;
};
