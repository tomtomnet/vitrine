// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * The GTK app's tests: GTK on a display of its own that no one sees
 * (broadway), the app on a session bus of its own, never the desktop's,
 * whoever runs them.  Include gtk/gtkbase.h first.
 */

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <cstdio>

#include <sys/prctl.h>
#include <sys/stat.h>

namespace GtkDisplay {

/*
 * In main(), before anything reads the environment: a runtime folder of
 * its own for broadway's socket (GLib keeps the first value it reads), GTK
 * on broadway alone, off the portals and the desktop's settings.  False
 * if the folder could not be made.
 */
inline bool prepare(QTemporaryDir &runtime)
{
    if (!runtime.isValid() || chmod(QFile::encodeName(runtime.path()).constData(), 0700) != 0) {
        fprintf(stderr, "no runtime folder\n");
        return false;
    }
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    qputenv("GDK_BACKEND", "broadway");
    qputenv("BROADWAY_DISPLAY", ":7");
    qputenv("GTK_A11Y", "none");
    qputenv("GSETTINGS_BACKEND", "memory");
    qputenv("GIO_USE_VFS", "local");
    qputenv("ADW_DISABLE_PORTAL", "1");
    qputenv("GDK_DEBUG", "no-portals");
    return true;
}

/*
 * The display and the bus, for initTestCase(): broadwayd in @broadway,
 * which ends with the test however the test ends (GTK ends a process
 * whose display does not open, and a daemon left would hold ctest's
 * output open), and a bus with no service to start on it (portals,
 * dconf).  Null and a skip when there is no broadwayd.
 */
inline GTestDBus *up(QProcess &broadway)
{
    const QString broadwayd = QStandardPaths::findExecutable("gtk4-broadwayd");
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");

    if (broadwayd.isEmpty()) {
        return nullptr;
    }
    /* its web side on a socket, no port; GTK's side in the runtime folder */
    broadway.setChildProcessModifier([]() { prctl(PR_SET_PDEATHSIG, SIGTERM); });
    broadway.start(broadwayd, {"--unixsocket", runtime + "/broadway-web", ":7"});
    if (!broadway.waitForStarted() ||
        !QTest::qWaitFor([&]() {
            return !QDir(runtime).entryList({"broadway*.socket"}, QDir::System).isEmpty();
        })) {
        return nullptr;
    }
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    /* which it unsets, against XDG_RUNTIME_DIR/bus: GTK finds broadway's socket there */
    qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime));
    return bus;
}

/*
 * For cleanupTestCase(): the daemons end.  The bus's object stays, as its
 * dispose would wait for the session connection to go, which the app
 * keeps (it stays registered until the process ends: GtkLoop).
 */
inline void down(QProcess &broadway, GTestDBus *bus)
{
    if (bus) {
        g_test_dbus_stop(bus);
    }
    broadway.kill();
    broadway.waitForFinished();
}

}
