// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFutureWatcher>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtConcurrent>

#include <cstdio>
#include <cstring>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "gtk/gtkloop.h"
#endif

/* The app's own ID: the bus is the test's */
static constexpr char kAppId[] = "org.vitrine.Vitrine";
/* This binary again, as a later vitrine-gtk */
static constexpr char kSecond[] = "--second-instance";

/*
 * vitrine-gtk's one main loop (GtkLoop): Qt's, on GLib's default main
 * context, which GTK runs on too.  Under it, Qt's timers, processes,
 * threads and deferred deletes work as in the Qt app, deleteLater() from
 * GTK's callbacks included, and GTK's timeouts, idles and windows too; a
 * later instance hands over to the first, which shows its window again;
 * closing the last window ends the loop, with the app's shutdown.  Also
 * why the loop is Qt's: GLib's alone leaves deleteLater'd objects alive.
 *
 * GTK runs on a display of its own (broadway, which no one sees) and the
 * app on a session bus of its own, never the desktop's.
 */
class TestGtkLoop : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;

private slots:
    void initTestCase()
    {
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        QVERIFY(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS") ==
                QString::fromUtf8(g_test_dbus_get_bus_address(m_bus)));
    }

    void cleanupTestCase()
    {
        GtkDisplay::down(m_broadway, m_bus);
    }

    void supported()
    {
        QString why;

        QVERIFY2(GtkLoop::supported(&why), qPrintable(why));
    }

    /*
     * Why the loop is Qt's: with GLib's alone running, as under
     * g_application_run(), an object deleteLater'd in a GLib callback
     * stays (Qt's loop level and scope are 0 there), whatever the time
     */
    void premise()
    {
        QPointer<QObject> object = new QObject;
        GMainLoop *loop = g_main_loop_new(nullptr, false);

        g_idle_add_once([](gpointer object) { static_cast<QObject *>(object)->deleteLater(); },
                        object.data());
        g_timeout_add_once(300, [](gpointer loop) { g_main_loop_quit(static_cast<GMainLoop *>(loop)); },
                           loop);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
        if (object.isNull()) {
            QSKIP("This Qt deletes what GLib's loop alone deleteLater'd: GtkLoop's reason "
                  "to run Qt's loop is gone");
        }
        /* Qt's deletes it */
        QTRY_VERIFY(object.isNull());
    }

    void loop()
    {
        GtkLoop loop(kAppId);
        QString error;
        GtkWidget *window = nullptr;
        int activations = 0;
        bool shutdown = false;
        bool timedOut = false;
        QStringList ran;
        QPointer<QObject> fromGtk = new QObject;
        QPointer<QObject> fromQt = new QObject;
        QProcess process;
        QProcess second;
        QFutureWatcher<int> watcher;
        int processExit = -1;
        QByteArray processOutput;
        int secondExit = -1;
        int threadResult = 0;

        QCOMPARE(loop.start(&error), GtkLoop::Start::Primary);
        g_signal_connect(loop.adw(), "shutdown", G_CALLBACK(+[](GApplication *, gpointer done) {
            *static_cast<bool *>(done) = true;
        }), &shutdown);
        loop.setActivated([&]() {
            activations++;
            if (!window) {
                window = adw_application_window_new(GTK_APPLICATION(loop.adw()));
                g_object_add_weak_pointer(G_OBJECT(window), reinterpret_cast<gpointer *>(&window));
            }
            gtk_window_present(GTK_WINDOW(window));
            /* the first one: the rest from the loop */
            if (activations > 1) {
                return;
            }
            QTimer::singleShot(10, [&]() { ran << "qt-timer"; });
            Glib::signal_timeout().connect_once([&]() { ran << "gtk-timeout"; }, 10);
            Glib::signal_idle().connect_once([&]() {
                ran << "gtk-idle";
                /* in a GTK callback, Qt's loop around it */
                fromGtk->deleteLater();
            });
            QTimer::singleShot(0, fromQt.data(), [&]() { fromQt->deleteLater(); });
            QObject::connect(&process, &QProcess::finished, [&](int code) {
                processExit = code;
                processOutput = process.readAllStandardOutput();
            });
            process.start("/bin/sh", {"-c", "echo out; exit 3"});
            QObject::connect(&watcher, &QFutureWatcher<int>::finished,
                             [&]() { threadResult = watcher.result(); });
            watcher.setFuture(QtConcurrent::run([]() { return 42; }));
            /* a later vitrine-gtk, once this one runs: this window again */
            QObject::connect(&second, &QProcess::finished, [&](int code) { secondExit = code; });
            second.setProcessChannelMode(QProcess::ForwardedChannels);
            second.start(QCoreApplication::applicationFilePath(), {kSecond});
        });

        /* all done: the window closes, which ends the loop */
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (ran.size() == 3 && fromGtk.isNull() && fromQt.isNull() && processExit >= 0 &&
                threadResult && secondExit >= 0 && activations >= 2 && window) {
                poll.stop();
                gtk_window_destroy(GTK_WINDOW(window));
            }
        });
        poll.start(20);
        QTimer::singleShot(30000, [&]() {
            timedOut = true;
            loop.quit(99);
        });

        const int status = loop.exec();
        QVERIFY(!timedOut);
        QCOMPARE(status, 0);
        QVERIFY(shutdown);
        QCOMPARE(activations, 2);
        QVERIFY(!window);
        ran.sort();
        QCOMPARE(ran, QStringList({"gtk-idle", "gtk-timeout", "qt-timer"}));
        QVERIFY(fromGtk.isNull());
        QVERIFY(fromQt.isNull());
        QCOMPARE(processExit, 3);
        QCOMPARE(processOutput, QByteArray("out\n"));
        QCOMPARE(threadResult, 42);
        QCOMPARE(secondExit, 0);
    }
};

/* A later instance: a remote of the first, which it activates */
static int secondInstance(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    GtkLoop loop(kAppId);
    QString error;

    switch (loop.start(&error)) {
    case GtkLoop::Start::Remote:
        return 0;
    case GtkLoop::Start::Primary:
        fprintf(stderr, "the second instance is the first\n");
        return 2;
    case GtkLoop::Start::Failed:
        fprintf(stderr, "the second instance did not register: %s\n", qPrintable(error));
        return 3;
    }
    return 4;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], kSecond) == 0) {
        return secondInstance(argc, argv);
    }
    /* nothing on the desktop's screen or bus, whoever runs it (gtkdisplay.h) */
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkloop-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }

    QCoreApplication app(argc, argv);
    TestGtkLoop test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkloop.moc"
