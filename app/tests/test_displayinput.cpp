// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The GTK view's keyboard and pointer (src/gtkview/displayinput): what the
 * Qt view's DisplayWindow does with keys, buttons, the wheel and the grab,
 * fed as the GTK widget feeds it, against stand-ins for QEMU's input calls
 * and for the compositor.  No display: the widget's GTK part is checked on
 * the nested KWin.
 */
#include <QTest>

#include <utility>

#include "gtkview/displayinput.h"
#include "gtkview/shortcutsinhibit.h"

namespace {

/* QEMU's and the compositor's calls, in one list: their order matters */
struct Log {
    QStringList calls;
    QStringList take() { return std::exchange(calls, {}); }
};

class FakeSink : public DisplayInput::Sink
{
public:
    explicit FakeSink(Log *log) : m_log(log) {}
    void keyPress(uint32_t qnum) override { m_log->calls << QString("press 0x%1").arg(qnum, 0, 16); }
    void keyRelease(uint32_t qnum) override
    {
        m_log->calls << QString("release 0x%1").arg(qnum, 0, 16);
    }
    void mouseAbs(uint32_t x, uint32_t y) override { m_log->calls << QString("abs %1 %2").arg(x).arg(y); }
    void mouseRel(int dx, int dy) override { m_log->calls << QString("rel %1 %2").arg(dx).arg(dy); }
    void mouseButton(uint32_t button, bool down) override
    {
        m_log->calls << QString("%1 %2").arg(down ? "down" : "up").arg(button);
    }
    bool mouseIsAbsolute() const override { return absolute; }

    bool absolute = true;

private:
    Log *m_log;
};

class FakeCompositor : public DisplayInput::Compositor
{
public:
    explicit FakeCompositor(Log *log) : m_log(log) {}
    void setShortcutsInhibited(bool on) override { m_log->calls << QString("inhibit %1").arg(on); }
    void setPointerLocked(bool on) override { m_log->calls << QString("lock %1").arg(on); }
    void setPointerConfined(bool on) override { m_log->calls << QString("confine %1").arg(on); }
    bool shortcutsInhibitPending() const override { return pending; }

    /* the compositor still asks the user (GNOME's dialog) */
    bool pending = false;

private:
    Log *m_log;
};

/* evdev codes (linux/input-event-codes.h) and their qnum codes */
constexpr uint32_t KEY_A = 30, KEY_Q = 16, KEY_LEFTMETA = 125, KEY_LEFTALT = 56;

struct Fixture {
    Log log;
    FakeSink sink{&log};
    FakeCompositor compositor{&log};
    DisplayInput input{&sink, &compositor};

    Fixture()
    {
        input.setGuestSize(QSize(1000, 1000));
        input.setView(QSize(1000, 1000), 1);
    }
};

} // namespace

class TestDisplayInput : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /* evdev to qnum; the toolkit's repeats and the keys it held before are not the guest's */
    void keys()
    {
        Fixture f;
        QVERIFY(f.input.key(KEY_A, true));
        QVERIFY(!f.input.key(KEY_A, true));     // a repeat
        QVERIFY(f.input.keyDown(KEY_A));
        QVERIFY(f.input.key(KEY_Q, true));
        QVERIFY(f.input.key(KEY_A, false));
        QVERIFY(!f.input.key(KEY_A, false));    // up already
        QVERIFY(!f.input.key(KEY_LEFTALT, false));  // down before the view had the keys
        QVERIFY(!f.input.key(0, true));         // KEY_RESERVED: no qnum
        QVERIFY(!f.input.key(100000, true));
        QCOMPARE(f.log.take(), QStringList({"press 0x1e", "press 0x10", "release 0x1e"}));
        f.input.releaseAllKeys();
        QCOMPARE(f.log.take(), QStringList({"release 0x10"}));
        QVERIFY(!f.input.keyDown(KEY_Q));
    }

    void hotkeys()
    {
        using H = DisplayInput::Hotkey;
        QCOMPARE(DisplayInput::hotkey('g', true, true, false, false), H::Grab);
        QCOMPARE(DisplayInput::hotkey('f', true, true, false, false), H::FullScreen);
        QCOMPARE(DisplayInput::hotkey('g', true, true, true, false), H::None);
        QCOMPARE(DisplayInput::hotkey('g', true, true, false, true), H::None);
        QCOMPARE(DisplayInput::hotkey('g', true, false, false, false), H::None);
        QCOMPARE(DisplayInput::hotkey('h', true, true, false, false), H::None);
    }

    /* The view's logical pixels to the guest's, through the renderer's letterbox */
    void guestPos_data()
    {
        QTest::addColumn<QSize>("view");
        QTest::addColumn<double>("scale");
        QTest::addColumn<QPointF>("at");
        QTest::addColumn<QPoint>("guest");
        /* a 1000x500 view: the square guest pillarboxed at x 250 - 750 */
        QTest::newRow("left edge") << QSize(1000, 500) << 1.0 << QPointF(250, 0) << QPoint(0, 0);
        QTest::newRow("middle") << QSize(1000, 500) << 1.0 << QPointF(500, 250) << QPoint(500, 500);
        QTest::newRow("left of it") << QSize(1000, 500) << 1.0 << QPointF(100, 100) << QPoint(0, 200);
        QTest::newRow("bottom right") << QSize(1000, 500) << 1.0 << QPointF(749.9, 499.9)
                                      << QPoint(999, 999);
        /* scale 2: 1000x500 logical, 2000x1000 in buffer pixels */
        QTest::newRow("scale 2") << QSize(2000, 1000) << 2.0 << QPointF(500, 250) << QPoint(500, 500);
        QTest::newRow("scale 1.5") << QSize(1500, 1500) << 1.5 << QPointF(500, 500)
                                   << QPoint(500, 500);
    }
    void guestPos()
    {
        QFETCH(QSize, view);
        QFETCH(double, scale);
        QFETCH(QPointF, at);
        QFETCH(QPoint, guest);
        Fixture f;
        f.input.setView(view, scale);
        uint32_t x = 0, y = 0;
        QVERIFY(f.input.guestPos(at.x(), at.y(), &x, &y));
        QCOMPARE(QPoint(int(x), int(y)), guest);

        /* none before the guest's first frame */
        f.input.setGuestSize(QSize());
        QVERIFY(!f.input.guestPos(at.x(), at.y(), &x, &y));
    }

    void absolutePointer()
    {
        Fixture f;
        f.input.pointerEnter(10, 20);
        f.input.motion(15, 25);
        QVERIFY(f.input.press(DisplayInput::Left, 30, 40));
        f.input.release(DisplayInput::Left);
        QCOMPARE(f.log.take(), QStringList({"abs 10 20", "abs 15 25", "abs 30 40", "down 0", "up 0"}));
        QVERIFY(!f.input.cursorHidden());
    }

    /* A guest without a tablet: the first click locks the pointer, and
       relative motion goes to the guest only while the compositor has it locked */
    void relativePointer()
    {
        Fixture f;
        f.sink.absolute = false;
        f.input.setHostActive(true);
        f.input.pointerEnter(10, 10);
        QCOMPARE(f.log.take(), QStringList());  // no automatic grab, no absolute moves

        int cursorChanges = 0;
        f.input.cursorChanged = [&]() { cursorChanges++; };
        QVERIFY(!f.input.press(DisplayInput::Left, 10, 10));   // grabs instead
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "confine 0", "lock 1"}));
        QVERIFY(f.input.grabbed());
        QVERIFY(f.input.cursorHidden());
        QCOMPARE(cursorChanges, 1);

        f.input.relativeMotion(1.5, 0);         // not locked yet
        f.input.setPointerLockActive(true);
        f.input.relativeMotion(1.5, 0.25);
        f.input.relativeMotion(0.6, 0.8);
        QCOMPARE(f.log.take(), QStringList({"rel 1 0", "rel 1 1"}));
        f.input.setPointerLockActive(false);    // the compositor let go (the window lost focus)
        f.input.relativeMotion(5, 5);
        QCOMPARE(f.log.take(), QStringList());

        /* clicks reach the guest while grabbed */
        f.input.setPointerLockActive(true);
        QVERIFY(f.input.press(DisplayInput::Right, 10, 10));
        f.input.release(DisplayInput::Right);
        QCOMPARE(f.log.take(), QStringList({"down 2", "up 2"}));

        f.input.setGrab(false);
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        QVERIFY(!f.input.cursorHidden());
        QCOMPARE(cursorChanges, 2);
    }

    /* As QEMU's SDL display: an absolute pointer over the focused view takes
       the desktop's shortcuts; Ctrl+Alt+G gives them back until the pointer leaves */
    void autoGrab()
    {
        Fixture f;
        QStringList states;
        f.input.grabChanged = [&]() { states << f.input.grabState(); };
        f.input.pointerEnter(1, 1);
        QCOMPARE(f.log.take(), QStringList({"abs 1 1"}));   // the window does not have the keys
        f.input.setHostActive(true);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0"}));
        QVERIFY(f.input.grabbed());
        f.input.pointerLeave();
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        f.input.pointerEnter(2, 2);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0", "abs 2 2"}));

        /* Ctrl+Alt+G with the automatic grab on: released, and not again
           while the pointer stays */
        f.input.setGrab(!f.input.grabbed());
        QVERIFY(!f.input.grabbed());
        f.input.motion(3, 3);
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0", "abs 3 3"}));
        f.input.pointerLeave();
        f.input.pointerEnter(4, 4);
        QVERIFY(f.input.grabbed());
        QCOMPARE(states, QStringList({
            "grabbed while the pointer is on the VM (Ctrl+Alt+G releases)",
            "not grabbed (Ctrl+Alt+G grabs)",
            "grabbed while the pointer is on the VM (Ctrl+Alt+G releases)",
            "not grabbed (Ctrl+Alt+G grabs)",
            "grabbed while the pointer is on the VM (Ctrl+Alt+G releases)",
        }));

        /* the keyboard gone: no grab */
        f.log.take();
        f.input.setHostActive(false);
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        f.input.setGrab(true);
        QCOMPARE(f.input.grabState(), QString("grabbed (Ctrl+Alt+G releases)"));
    }

    /* On GNOME for an app it does not know: no grab of its own (GNOME would ask
       at every new window), Ctrl+Alt+G asks */
    void noAutoGrab()
    {
        Fixture f;
        f.input.setAutoGrab(false);
        f.input.setHostActive(true);
        f.input.pointerEnter(1, 1);
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"abs 1 1"}));
        QCOMPARE(f.input.grabState(), QString("not grabbed (Ctrl+Alt+G grabs; GNOME asks first)"));
        f.input.setGrab(true);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0"}));
        QCOMPARE(f.input.grabState(), QString("grabbed (Ctrl+Alt+G releases)"));
        f.input.setGrab(false);
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        QVERIFY(!f.input.grabbed());
        /* the pointer back over it: still nothing of its own */
        f.input.pointerLeave();
        f.input.pointerEnter(2, 2);
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"abs 2 2"}));
        /* allowed again (the app installed meanwhile, in a test): as before */
        f.input.setAutoGrab(true);
        QVERIFY(f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0"}));
    }

    /* GNOME's dialog takes the window's keyboard while it asks about the
       user's grab: the grab stays for its answer; once answered, the
       keyboard's loss releases the grab as before */
    void grabWhileAsked()
    {
        Fixture f;
        f.input.setAutoGrab(false);
        f.input.setHostActive(true);
        f.input.pointerEnter(1, 1);
        f.input.setGrab(true);
        f.log.take();
        f.compositor.pending = true;
        f.input.setHostActive(false);
        QVERIFY(f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList());
        f.input.setHostActive(true);
        QVERIFY(f.input.grabbed());
        QCOMPARE(f.input.grabState(), QString("grabbed (Ctrl+Alt+G releases)"));
        /* granted: the keyboard gone, the grab goes */
        f.compositor.pending = false;
        f.input.setHostActive(false);
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        /* the automatic grab is not the user's: it goes with the keyboard */
        f.input.setAutoGrab(true);
        f.input.setHostActive(true);
        f.log.take();
        f.compositor.pending = true;
        f.input.setHostActive(false);
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
    }

    /* Who asks on their own: everywhere but on GNOME for an app it does not know
       by its desktop file (XDG_CURRENT_DESKTOP is a list split by ':') */
    void automaticRequests_data()
    {
        QTest::addColumn<QString>("desktop");
        QTest::addColumn<bool>("known");
        QTest::addColumn<bool>("automatic");
        QTest::newRow("GNOME, window-backed") << "GNOME" << false << false;
        QTest::newRow("GNOME, known") << "GNOME" << true << true;
        QTest::newRow("Ubuntu's GNOME") << "ubuntu:GNOME" << false << false;
        QTest::newRow("GNOME Classic") << "GNOME-Classic:GNOME" << false << false;
        QTest::newRow("GNOME Classic, known") << "GNOME-Classic:GNOME" << true << true;
        QTest::newRow("KDE") << "KDE" << false << true;
        QTest::newRow("not GNOME by its name alone") << "GNOME-Flashback" << false << true;
        QTest::newRow("Cinnamon") << "X-Cinnamon" << false << true;
        QTest::newRow("empty") << "" << false << true;
        QTest::newRow("unset") << QString() << false << true;
    }

    void automaticRequests()
    {
        QFETCH(QString, desktop);
        QFETCH(bool, known);
        QFETCH(bool, automatic);
        const QByteArray d = desktop.toUtf8();
        QCOMPARE(ShortcutsInhibit::automaticFor(desktop.isNull() ? nullptr : d.constData(), known),
                 automatic);
    }

    /* KWin's Meta + drag: with the grab, the pointer is confined before the
       key reaches the guest (the press may follow at once), and freed after */
    void windowActionModifier()
    {
        Fixture f;
        f.input.setHostActive(true);
        f.input.pointerEnter(1, 1);
        f.log.take();
        QVERIFY(f.input.key(KEY_LEFTMETA, true));
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 1", "press 0xdb"}));
        QVERIFY(f.input.press(DisplayInput::Left, 5, 5));
        f.input.release(DisplayInput::Left);
        QVERIFY(f.input.key(KEY_LEFTMETA, false));
        QCOMPARE(f.log.take(), QStringList({"abs 5 5", "down 0", "up 0", "inhibit 1", "lock 0",
                                            "confine 0", "release 0xdb"}));

        /* without the grab nothing is confined */
        f.input.pointerLeave();
        f.log.take();
        QVERIFY(f.input.key(KEY_LEFTMETA, true));
        QCOMPARE(f.log.take(), QStringList({"press 0xdb"}));
    }

    /* The guest's buttons go up when the pointer is taken away, the
       keyboard goes, the grab goes or the mouse switches mode */
    void buttonsReleased()
    {
        Fixture f;
        f.input.setHostActive(true);
        f.input.pointerEnter(1, 1);
        f.log.take();
        auto pressTwo = [&]() {
            f.input.press(DisplayInput::Left, 1, 1);
            f.input.press(DisplayInput::Right, 1, 1);
            QCOMPARE(f.input.buttonsDown(), (1u << 0) | (1u << 2));
            f.log.take();
        };
        pressTwo();
        f.input.pointerLeave();
        QCOMPARE(f.log.take().mid(0, 2), QStringList({"up 0", "up 2"}));
        f.input.pointerEnter(1, 1);
        pressTwo();
        f.input.mouseModeChanged();
        QCOMPARE(f.log.take().mid(0, 2), QStringList({"up 0", "up 2"}));
        pressTwo();
        f.input.setGrab(true);
        f.log.take();
        f.input.press(DisplayInput::Middle, 1, 1);
        f.log.take();
        f.input.setGrab(false);
        QCOMPARE(f.log.take().mid(0, 3), QStringList({"up 0", "up 1", "up 2"}));
        pressTwo();
        f.input.setHostActive(false);
        QCOMPARE(f.log.take().mid(0, 2), QStringList({"up 0", "up 2"}));
        QCOMPARE(f.input.buttonsDown(), 0u);
    }

    /* 120ths of a notch, positive up and left: whole notches to QEMU */
    void wheel()
    {
        Fixture f;
        f.input.scroll(0, 120);
        QCOMPARE(f.log.take(), QStringList({"down 3", "up 3"}));
        f.input.scroll(0, 60);
        QCOMPARE(f.log.take(), QStringList());
        f.input.scroll(0, 60);
        QCOMPARE(f.log.take(), QStringList({"down 3", "up 3"}));
        f.input.scroll(0, -240);
        QCOMPARE(f.log.take(), QStringList({"down 4", "up 4", "down 4", "up 4"}));
        f.input.scroll(120, 0);
        f.input.scroll(-120, 0);
        QCOMPARE(f.log.take(), QStringList({"down 7", "up 7", "down 8", "up 8"}));
        /* a touchpad's small steps add up */
        for (int i = 0; i < 10; i++) {
            f.input.scroll(0, -12);
        }
        QCOMPARE(f.log.take(), QStringList({"down 4", "up 4"}));
    }

    /* MouseSet hides the guest's cursor */
    void cursorVisibility()
    {
        Fixture f;
        int changes = 0;
        f.input.cursorChanged = [&]() { changes++; };
        f.input.setGuestCursorVisible(false);
        QVERIFY(f.input.cursorHidden());
        f.input.setGuestCursorVisible(false);
        f.input.setGuestCursorVisible(true);
        QVERIFY(!f.input.cursorHidden());
        QCOMPARE(changes, 2);
    }
};

QTEST_GUILESS_MAIN(TestDisplayInput)
#include "test_displayinput.moc"
