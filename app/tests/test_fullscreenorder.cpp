// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FullScreenOrder: a view's full-screen window waits for the window the view
 * is in to be shown and active, so that it maps last (GNOME's Mutter gives
 * the focus and the top to the window it maps last).  On broadway, where no
 * one sees the windows.
 */
#include <gtkmm/init.h>
#include <gtkmm/window.h>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

#include "gtkdisplay.h"
#include "gtkview/fullscreenorder.h"

class TestFullScreenOrder : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;

private Q_SLOTS:
    void initTestCase()
    {
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        gtk_init();
        Gtk::init_gtkmm_internals();
        /* the longest wait, short for the tests */
        FullScreenOrder::setTimeout(300);
    }

    void cleanupTestCase()
    {
        GtkDisplay::down(m_broadway, m_bus);
    }

    /* Nothing to wait for: no window, or one not shown */
    void noWait()
    {
        FullScreenOrder order;
        int retries = 0;
        QVERIFY(order.ready(nullptr, [&]() { retries++; }));
        Gtk::Window hidden;
        QVERIFY(order.ready(&hidden, [&]() { retries++; }));
        QVERIFY(!order.waiting());
        QTest::qWait(400);
        QCOMPARE(retries, 0);
    }

    /* A window just shown: the full screen comes once it is active, or the
       wait is over, through the one retry, which is then let through */
    void waitsForTheWindow()
    {
        FullScreenOrder order;
        Gtk::Window window;
        window.present();
        QVERIFY(!window.is_active());

        int retries = 0, wentAhead = 0;
        std::function<void()> retry = [&]() {
            retries++;
            if (order.ready(&window, retry)) {
                wentAhead++;
            }
        };
        QVERIFY(!order.ready(&window, retry));
        QVERIFY(order.waiting());
        /* asked again meanwhile: the same wait, its own retry */
        int others = 0;
        QVERIFY(!order.ready(&window, [&]() { others++; }));
        QTRY_COMPARE(retries, 1);
        QCOMPARE(wentAhead, 1);
        QCOMPARE(others, 0);
        QVERIFY(!order.waiting());
        QTest::qWait(400);
        QCOMPARE(retries, 1);

        /* active by now (broadway focuses a window shown), or not: either way
           no wait once the window had its turn */
        if (window.is_active()) {
            QVERIFY(order.ready(&window, [&]() { others++; }));
        }
        window.set_visible(false);
    }

    /* Full screen asked off while it waits: it never comes */
    void cancelled()
    {
        FullScreenOrder order;
        Gtk::Window window;
        window.present();
        int retries = 0;
        QVERIFY(!order.ready(&window, [&]() { retries++; }));
        order.cancel();
        QVERIFY(!order.waiting());
        QTest::qWait(500);
        QCOMPARE(retries, 0);
        window.set_visible(false);
    }

    /* Gone while it waits: no retry after its end */
    void destroyedWhileWaiting()
    {
        int retries = 0;
        Gtk::Window window;
        window.present();
        {
            FullScreenOrder order;
            QVERIFY(!order.ready(&window, [&]() { retries++; }));
        }
        QTest::qWait(500);
        QCOMPARE(retries, 0);
        window.set_visible(false);
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-fullscreenorder-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestFullScreenOrder test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_fullscreenorder.moc"
