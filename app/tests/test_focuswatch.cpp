// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * FocusWatch: whether a widget has the keyboard (the focus of its window,
 * the window active), known once GTK made the change.  GTK 4 sends the
 * focus controllers' enter before it stores the new focus: a VM's screen
 * that decided there, given the focus by the list of VMs or by the Console
 * view coming back, took no grab.  On broadway, where no one sees the
 * windows, with plain focusable widgets for the views.
 */
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/init.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "gtkdisplay.h"
#include "gtkview/focuswatch.h"

namespace {

/* A watch on @widget and what it said: its value at each call */
struct Watched {
    explicit Watched(Gtk::Widget &widget)
        : watch(GTK_WIDGET(widget.gobj()), [this, &widget]() {
              said.push_back(watch.hasKeyboard());
              /* told after GTK stored the change: the widget is (or is not)
                 the window's focus by then */
              focusWhenTold.push_back(widget.is_focus());
          })
    {
    }
    /* before the watch, which may call at once */
    std::vector<bool> said;
    std::vector<bool> focusWhenTold;
    FocusWatch watch;
};

} // namespace

class TestFocusWatch : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;

    /* @window shown and active (broadway focuses a window shown), else a skip */
    static bool shownActive(Gtk::Window &window)
    {
        window.present();
        for (int i = 0; i < 100 && !window.is_active(); i++) {
            QTest::qWait(20);
        }
        return window.is_active();
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        gtk_init();
        Gtk::init_gtkmm_internals();
    }

    void cleanupTestCase()
    {
        GtkDisplay::down(m_broadway, m_bus);
    }

    /* The focus moved from one widget to another: each is told once GTK
       stored it, and not yet in the new focus's enter, where GTK has not */
    void focusMoved()
    {
        Gtk::Window window;
        Gtk::Box box(Gtk::Orientation::VERTICAL);
        Gtk::Button one("one"), two("two");
        box.append(one);
        box.append(two);
        window.set_child(box);
        Watched first(one), second(two);
        bool watchInEnter = true;
        auto enter = Gtk::EventControllerFocus::create();
        enter->signal_enter().connect([&]() { watchInEnter = second.watch.hasKeyboard(); });
        two.add_controller(enter);

        if (!shownActive(window)) {
            QSKIP("broadway did not make the window active");
        }
        one.grab_focus();
        QVERIFY(first.watch.hasKeyboard());
        QVERIFY(!second.watch.hasKeyboard());
        QCOMPARE(first.focusWhenTold.back(), true);

        two.grab_focus();
        QVERIFY(!first.watch.hasKeyboard());
        QVERIFY(second.watch.hasKeyboard());
        QCOMPARE(second.said, std::vector<bool>({true}));
        QCOMPARE(second.focusWhenTold, std::vector<bool>({true}));
        QCOMPARE(first.said.back(), false);
        /* GTK's enter came before the change: the watch said nothing yet */
        QVERIFY(!watchInEnter);

        /* the focus off any widget: the window's own keys */
        window.unset_focus();
        QVERIFY(!second.watch.hasKeyboard());
        window.set_visible(false);
    }

    /* A stack's page switch moves the focus to the page shown (the console
       of the VM chosen, the Console view back from Details), and back */
    void stackSwitch()
    {
        Gtk::Window window;
        Gtk::Stack stack;
        /* in boxes, as the screens are in the consoles' pages: GtkStack moves
           the focus only from inside the page it leaves */
        Gtk::Box pageOne(Gtk::Orientation::VERTICAL), pageTwo(Gtk::Orientation::VERTICAL);
        Gtk::Button screenOne("screen one"), screenTwo("screen two");
        pageOne.append(screenOne);
        pageTwo.append(screenTwo);
        stack.add(pageOne, "one");
        stack.add(pageTwo, "two");
        window.set_child(stack);
        Watched first(screenOne), second(screenTwo);

        if (!shownActive(window)) {
            QSKIP("broadway did not make the window active");
        }
        screenOne.grab_focus();
        QVERIFY(first.watch.hasKeyboard());

        stack.set_visible_child("two");
        QVERIFY(!first.watch.hasKeyboard());
        QVERIFY(second.watch.hasKeyboard());
        QCOMPARE(second.focusWhenTold.back(), true);

        stack.set_visible_child("one");
        QVERIFY(first.watch.hasKeyboard());
        QVERIFY(!second.watch.hasKeyboard());
        window.set_visible(false);
    }

    /* The window no longer active, then active again: the focus it kept
       has the keyboard only while it is active */
    void windowActivity()
    {
        Gtk::Window window, other;
        Gtk::Button screen("screen");
        window.set_child(screen);
        Watched watched(screen);

        if (!shownActive(window)) {
            QSKIP("broadway did not make the window active");
        }
        screen.grab_focus();
        QVERIFY(watched.watch.hasKeyboard());
        if (!shownActive(other)) {
            QSKIP("broadway did not make the second window active");
        }
        QTRY_VERIFY(!window.is_active());
        QVERIFY(!watched.watch.hasKeyboard());
        QVERIFY(screen.is_focus());   // kept by its window

        other.set_visible(false);
        QVERIFY(shownActive(window));
        QTRY_VERIFY(watched.watch.hasKeyboard());
        QCOMPARE(watched.said, std::vector<bool>({true, false, true}));
        window.set_visible(false);
    }

    /* Off its window: no keyboard, and the watch follows the window the
       widget is realized in next; gone, it says nothing more */
    void leavesItsWindow()
    {
        Gtk::Window window, next;
        Gtk::Box box(Gtk::Orientation::VERTICAL), nextBox(Gtk::Orientation::VERTICAL);
        auto screen = std::make_unique<Gtk::Button>("screen");
        box.append(*screen);
        window.set_child(box);
        next.set_child(nextBox);
        auto watched = std::make_unique<Watched>(*screen);

        if (!shownActive(window)) {
            QSKIP("broadway did not make the window active");
        }
        screen->grab_focus();
        QVERIFY(watched->watch.hasKeyboard());
        box.remove(*screen);
        QVERIFY(!watched->watch.hasKeyboard());

        nextBox.append(*screen);
        if (!shownActive(next)) {
            QSKIP("broadway did not make the second window active");
        }
        screen->grab_focus();
        QVERIFY(watched->watch.hasKeyboard());

        const size_t told = watched->said.size();
        watched.reset();
        nextBox.remove(*screen);
        screen.reset();
        QCOMPARE(told, size_t(3));
        next.set_visible(false);
        window.set_visible(false);
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-focuswatch-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestFocusWatch test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_focuswatch.moc"
