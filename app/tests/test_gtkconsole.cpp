// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <vector>

#include <unistd.h>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/gtkloop.h"
#include "gtk/mainwindow.h"
#include "gtk/vmconsole.h"
#include "gtk/widgets.h"
#include "gtkview/nesteddisplay.h"

/* The guest's desktop in the console, as NestedDisplay shows it, without a guest or Wayland */
class StandInDesktop : public NestedDisplay
{
public:
    StandInDesktop(const QString &socket, int *alive)
        : m_socket(socket), m_alive(alive), m_widget(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0))
    {
        g_object_ref_sink(m_widget);
        ++*m_alive;
    }
    ~StandInDesktop() override
    {
        g_object_unref(m_widget);
        --*m_alive;
    }

    QString socket() const override { return m_socket; }
    GtkWidget *widget() const override { return m_widget; }
    bool isFullScreen() const override { return full; }
    void setFullScreen(bool on) override
    {
        if (on != full) {
            full = on;
            fullScreenChanged(on);
        }
    }
    void setInputEnabled(bool on) override { input = on; }
    bool hasKeyboard() const override { return false; }
    void focus() override {}
    int connections() const override { return m_connections; }
    bool isMapped() const override { return m_mapped; }
    void setCovered(bool on) override { covered = on; }

    /* The guest's proxy connects, its nested KWin's window comes */
    void guestConnects(int count)
    {
        m_connections = count;
        connectionsChanged(count);
    }
    void guestMaps(bool on)
    {
        m_mapped = on;
        mappedChanged(on);
    }
    /* Its library's thread showed, as the guest connected */
    void libraryThread(pid_t tid)
    {
        m_threads = {{Thread::Role::Main, getpid()}, {Thread::Role::Library, tid}};
        threadsChanged();
    }
    std::vector<Thread> threads() const override { return m_threads; }

    bool full = false;
    bool input = true;
    bool covered = false;

private:
    QString m_socket;
    int *m_alive;
    GtkWidget *m_widget;
    int m_connections = 0;
    bool m_mapped = false;
    std::vector<Thread> m_threads;
};
#endif

/*
 * The GTK app's consoles, on a display no one sees: a VM's nested desktop
 * served once a run, its views as the guest's window comes and goes and as
 * the user picks them, Ctrl+Alt+1/2 by the key's place, full screen with
 * the view shown, a new view at the guest's reboot; in the window, the
 * actions that follow the views, the views' keys only while the console is
 * in sight, a dialog over the console, and what closing says of the
 * guest's session.
 */
class TestGtkConsole : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;
    QTemporaryDir m_tmp;
    std::unique_ptr<VmStore> m_store;
    /* the stand-ins made and alive, and the last one made */
    static inline int s_made = 0;
    static inline int s_alive = 0;
    static inline StandInDesktop *s_last = nullptr;

    bool addVm(const QString &id, const QString &name)
    {
        QFile args(m_tmp.filePath("vms/" + id + "/vm.args"));
        return QDir().mkpath(m_tmp.filePath("vms/" + id)) && args.open(QIODevice::WriteOnly) &&
               args.write(("-name " + name + "\n-m 1G\n").toUtf8()) > 0;
    }

    static bool enabled(MainWindow &window, const char *action)
    {
        return g_action_group_get_action_enabled(G_ACTION_GROUP(window.window()), action);
    }

    /* The view action's state: "screen" or "desktop" */
    static QString viewState(MainWindow &window)
    {
        GVariant *state = g_action_group_get_action_state(G_ACTION_GROUP(window.window()), "view");
        const QString view = state ? QString::fromUtf8(g_variant_get_string(state, nullptr)) : QString();
        if (state) {
            g_variant_unref(state);
        }
        return view;
    }

    static GdkModifierType ctrlAlt(GdkModifierType more = GdkModifierType(0))
    {
        return GdkModifierType(GDK_CONTROL_MASK | GDK_ALT_MASK | more);
    }

private slots:
    void initTestCase()
    {
        QString error;

        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_tmp.isValid());
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        m_loop = new GtkLoop("org.vitrine.Vitrine");
        QCOMPARE(m_loop->start(&error), GtkLoop::Start::Primary);
        QVERIFY(addVm("alpha", "Alpha"));
        m_store = std::make_unique<VmStore>(m_tmp.filePath("vms"));
        QCOMPARE(m_store->vms().size(), 1);
        VmConsole::setNestedFactory([](const QString &socket, QString *) -> NestedDisplay * {
            s_made++;
            s_last = new StandInDesktop(socket, &s_alive);
            return s_last;
        });
    }

    void cleanupTestCase()
    {
        VmConsole::setNestedFactory({});
        m_store.reset();
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    void views()
    {
        Vm *vm = m_store->find("alpha");
        const QString socket = m_tmp.filePath("nested.sock");
        QString error;
        {
            VmConsole console(vm);

            /* off: its home, no views */
            QCOMPARE(console.page(), VmConsole::Page::Home);
            QVERIFY(!console.hasViews());
            QVERIFY(!console.hasScreen());
            QVERIFY(console.serveNested(socket, &error));
            StandInDesktop *desktop = s_last;
            QVERIFY(desktop);
            QCOMPARE(s_alive, 1);
            QCOMPARE(desktop->socket(), socket);
            QVERIFY(console.hasViews());
            QCOMPARE(console.currentView(), ConsoleViews::View::Screen);
            QVERIFY(!console.desktopShown());
            /* the guest's desktop in the console's stack, beside the screen's place */
            QCOMPARE(gtk_widget_get_parent(gtk_widget_get_parent(desktop->widget())),
                     console.widget());
            /* served once a run */
            QVERIFY(console.serveNested(socket, &error));
            QCOMPARE(console.nestedView(), static_cast<NestedDisplay *>(desktop));
            QCOMPARE(s_alive, 1);

            /* the nested KWin connected: its desktop, with no keys from a VM that does not run */
            desktop->guestConnects(1);
            desktop->guestMaps(true);
            QCOMPARE(console.currentView(), ConsoleViews::View::Desktop);
            QVERIFY(console.desktopShown());
            QVERIFY(console.hasScreen());
            QVERIFY(!desktop->input);
            QVERIFY(console.hasNestedSession());
            /* the user's choice holds until the window comes or goes */
            console.showView(ConsoleViews::View::Screen);
            QCOMPARE(console.currentView(), ConsoleViews::View::Screen);
            QVERIFY(!console.hasScreen());
            console.showView(ConsoleViews::View::Desktop);
            QCOMPARE(console.currentView(), ConsoleViews::View::Desktop);

            /* full screen goes with the view shown: the Screen view has none here */
            console.setFullScreen(true);
            QVERIFY(desktop->isFullScreen());
            QVERIFY(console.isFullScreen());
            console.showView(ConsoleViews::View::Screen);
            QVERIFY(!desktop->isFullScreen());
            QVERIFY(!console.isFullScreen());
            console.showView(ConsoleViews::View::Desktop);

            /* under a dialog: the guest's window leaves vitrine's */
            console.setCovered(true);
            QVERIFY(desktop->covered);
            console.setCovered(false);
            QVERIFY(!desktop->covered);
            /* before a dialog: out of full screen */
            console.setFullScreen(true);
            console.leaveScreen();
            QVERIFY(!desktop->isFullScreen());

            /* the session ended: the VM's screen, its login screen or its shutdown */
            desktop->guestMaps(false);
            QCOMPARE(console.currentView(), ConsoleViews::View::Screen);
            /*
             * The host's Reset under the session: the screen, and the nested
             * desktop served anew, its connections with the old view; the
             * next session's window shows
             */
            desktop->guestMaps(true);
            QCOMPARE(console.currentView(), ConsoleViews::View::Desktop);
            const int made = s_made;
            Q_EMIT vm->runner()->guestReset();
            QCOMPARE(console.currentView(), ConsoleViews::View::Screen);
            /* the old one gone, a new one (its memory may be the old one's) */
            QCOMPARE(s_made, made + 1);
            QCOMPARE(s_alive, 1);
            desktop = s_last;
            QCOMPARE(console.nestedView(), static_cast<NestedDisplay *>(desktop));
            QCOMPARE(desktop->socket(), socket);
            QVERIFY(!console.hasNestedSession());
            desktop->guestMaps(true);
            QCOMPARE(console.currentView(), ConsoleViews::View::Desktop);
            /* a new view goes under a dialog as the old one was */
            console.setCovered(true);
            QVERIFY(desktop->covered);
        }
        /* the console gone, its view with it (the guest's session ends) */
        QCOMPARE(s_alive, 0);
    }

    /*
     * The guest's desktop's threads with the screen's, for real time while
     * the VM is in front: none until its library's thread shows, none with
     * VITRINE_VIEW_RT=0, the new view's after a reboot; their line
     */
    void threads()
    {
        Vm *vm = m_store->find("alpha");
        QString error;
        VmConsole console(vm);

        QVERIFY(console.realtimeThreads().empty());
        QVERIFY(console.serveNested(m_tmp.filePath("threads.sock"), &error));
        QVERIFY(console.realtimeThreads().empty());
        s_last->libraryThread(4242);
        QVERIFY(console.realtimeThreads() == (std::vector<pid_t>{getpid(), 4242}));
        qputenv("VITRINE_VIEW_RT", "0");
        QVERIFY(console.realtimeThreads().empty());
        qunsetenv("VITRINE_VIEW_RT");
        QVERIFY(console.realtimeThreads() == (std::vector<pid_t>{getpid(), 4242}));

        /* a reboot: a new view, its own library's thread */
        Q_EMIT vm->runner()->guestReset();
        QVERIFY(console.realtimeThreads().empty());
        s_last->libraryThread(4343);
        QVERIFY(console.realtimeThreads() == (std::vector<pid_t>{getpid(), 4343}));

        QCOMPARE(QString::fromStdString(NestedDisplay::threadLine(s_last->threads())),
                 QString("vitrine: desktop: threads: main=%1 library=4343").arg(getpid()));
        QCOMPARE(QString::fromStdString(NestedDisplay::threadLine({})),
                 QString("vitrine: desktop: threads:"));
    }

    /* Ctrl+Alt+1/2 by the key's place, whatever character the layout gives it */
    void keys()
    {
        using View = ConsoleViews::View;
        using Maybe = std::optional<View>;

        /* AZERTY: &, é and " on the keys of 1, 2 and 3 (XKB's keycodes 10, 11, 12) */
        QVERIFY(VmConsole::viewForKey(GDK_KEY_ampersand, 10, ctrlAlt()) == Maybe(View::Screen));
        QVERIFY(VmConsole::viewForKey(GDK_KEY_eacute, 11, ctrlAlt()) == Maybe(View::Desktop));
        QVERIFY(VmConsole::viewForKey(GDK_KEY_quotedbl, 12, ctrlAlt()) == std::nullopt);
        /* EurKEY (QWERTY) gives the digits */
        QVERIFY(VmConsole::viewForKey(GDK_KEY_1, 10, ctrlAlt()) == Maybe(View::Screen));
        QVERIFY(VmConsole::viewForKey(GDK_KEY_2, 11, ctrlAlt()) == Maybe(View::Desktop));
        /* the keypad's 1 and 2 (KEY_KP1, KEY_KP2: 79 and 80, + 8) */
        QVERIFY(VmConsole::viewForKey(GDK_KEY_KP_End, 87, ctrlAlt()) == Maybe(View::Screen));
        QVERIFY(VmConsole::viewForKey(GDK_KEY_KP_Down, 88, ctrlAlt()) == Maybe(View::Desktop));
        /* Ctrl+Alt alone */
        QVERIFY(VmConsole::viewForKey(GDK_KEY_exclam, 10, ctrlAlt(GDK_SHIFT_MASK)) == std::nullopt);
        QVERIFY(VmConsole::viewForKey(GDK_KEY_1, 10, ctrlAlt(GDK_SUPER_MASK)) == std::nullopt);
        QVERIFY(VmConsole::viewForKey(GDK_KEY_1, 10, GDK_CONTROL_MASK) == std::nullopt);
        /* no keycode: by the character */
        QVERIFY(VmConsole::viewForKey(GDK_KEY_2, 0, ctrlAlt()) == Maybe(View::Desktop));
        QVERIFY(VmConsole::viewForKey(GDK_KEY_ampersand, 0, ctrlAlt()) == std::nullopt);
    }

    void window()
    {
        MainWindow window(m_loop, m_store.get());
        QString error;

        window.present();
        QTest::qWait(100);
        window.select("alpha");
        VmConsole *console = window.console("alpha");
        QVERIFY(console);
        QCOMPARE(window.console("alpha"), console);
        /* a VM that is off, without its nested desktop: none of the console's actions */
        for (const char *off : {"view", "full-screen", "send-keys", "release-input"}) {
            QVERIFY2(!enabled(window, off), off);
        }

        QVERIFY(console->serveNested(m_tmp.filePath("window.sock"), &error));
        StandInDesktop *desktop = s_last;
        QVERIFY(enabled(window, "view"));
        QCOMPARE(viewState(window), QString("screen"));
        desktop->guestConnects(1);
        desktop->guestMaps(true);
        QCOMPARE(viewState(window), QString("desktop"));
        QVERIFY(enabled(window, "full-screen"));
        QVERIFY(!enabled(window, "send-keys"));
        /* the menu's */
        g_action_group_activate_action(G_ACTION_GROUP(window.window()), "view",
                                       g_variant_new_string("screen"));
        QCOMPARE(console->currentView(), ConsoleViews::View::Screen);
        QCOMPARE(viewState(window), QString("screen"));
        g_action_group_activate_action(G_ACTION_GROUP(window.window()), "view",
                                       g_variant_new_string("desktop"));
        QCOMPARE(console->currentView(), ConsoleViews::View::Desktop);

        /* Ctrl+Alt+1/2 for the console in sight only: off, the page shows the VM's home */
        QVERIFY(!window.switchViewByKey(GDK_KEY_ampersand, 10, ctrlAlt()));
        QCOMPARE(console->currentView(), ConsoleViews::View::Desktop);
        /* full screen, it is in sight */
        g_action_group_activate_action(G_ACTION_GROUP(window.window()), "full-screen", nullptr);
        QVERIFY(desktop->isFullScreen());
        QVERIFY(window.switchViewByKey(GDK_KEY_ampersand, 10, ctrlAlt()));
        QCOMPARE(console->currentView(), ConsoleViews::View::Screen);
        QVERIFY(!desktop->isFullScreen());
        /* a key that is not one of them goes on to the guest */
        QVERIFY(!window.switchViewByKey(GDK_KEY_quotedbl, 12, ctrlAlt()));

        /* a dialog: no screen in full screen first, the console in sight covered meanwhile */
        console->showView(ConsoleViews::View::Desktop);
        desktop->setFullScreen(true);
        AdwDialog *dialog = Gtk4::alert("A Question", "Over the console");
        window.present(dialog);
        QVERIFY(!desktop->isFullScreen());
        QVERIFY(desktop->covered);
        adw_dialog_force_close(dialog);
        QTRY_VERIFY(!desktop->covered);

        /* closing asks, and says the guest's session ends with it */
        gtk_window_close(window.window());
        AdwDialog *asked =
            adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window.window()));
        QVERIFY(asked && ADW_IS_ALERT_DIALOG(asked));
        const QString body = QString::fromUtf8(adw_alert_dialog_get_body(ADW_ALERT_DIALOG(asked)));
        QVERIFY2(body.contains("Closing Vitrine logs out the desktop session of Alpha"),
                 qPrintable(body));
        adw_dialog_force_close(asked);
        QVERIFY(window.window());
        /* its session over: nothing to say of it */
        desktop->guestMaps(false);
        desktop->guestConnects(0);
        QVERIFY(!console->hasNestedSession());
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkconsole-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkConsole test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkconsole.moc"
