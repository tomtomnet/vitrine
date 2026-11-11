// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The GTK view's keyboard and pointer (src/gtkview/displayinput): what the
 * Qt view's DisplayWindow does with keys, buttons, the wheel and the grab,
 * fed as the GTK widget feeds it, against stand-ins for QEMU's input calls
 * and for the compositor, and where the keys go (the guest only while the
 * pointer is on the view or the grab holds; KeyOwners).  No display: the
 * widget's GTK part is checked on the nested GNOME and KWin.
 */
#include <QTest>

#include <utility>

#include "gtkview/displayinput.h"
#include "gtkview/keyowners.h"
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
constexpr uint32_t KEY_A = 30, KEY_Q = 16, KEY_LEFTMETA = 125, KEY_LEFTALT = 56, KEY_LEFTCTRL = 29;

using Key = DisplayInput::Key;

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
    /* the window has the keyboard and the pointer is on the view: the keys
       are the guest's, the automatic grab on */
    void withKeys()
    {
        input.setHostActive(true);
        input.pointerEnter(1, 1);
        log.take();
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
        f.withKeys();
        QCOMPARE(f.input.key(KEY_A, true), Key::Sent);
        QCOMPARE(f.input.key(KEY_A, true), Key::Kept);      // a repeat
        QVERIFY(f.input.keyDown(KEY_A));
        QCOMPARE(f.input.key(KEY_Q, true), Key::Sent);
        QCOMPARE(f.input.key(KEY_A, false), Key::Sent);
        QCOMPARE(f.input.key(KEY_A, false), Key::Kept);     // up already
        QCOMPARE(f.input.key(KEY_LEFTALT, false), Key::Kept);   // down before the view had the keys
        QCOMPARE(f.input.key(0, true), Key::Kept);          // KEY_RESERVED: no qnum
        QCOMPARE(f.input.key(100000, true), Key::Kept);
        QCOMPARE(f.log.take(), QStringList({"press 0x1e", "press 0x10", "release 0x1e"}));
        f.input.releaseAllKeys();
        QCOMPARE(f.log.take(), QStringList({"release 0x10"}));
        QVERIFY(!f.input.keyDown(KEY_Q));
        QCOMPARE(f.input.key(KEY_Q, false), Key::Kept);     // released in the guest already
        QCOMPARE(f.log.take(), QStringList());
    }

    /* Each key keeps the owner of its press until let go */
    void keyOwners()
    {
        KeyOwners owners;
        bool repeat = true;
        QCOMPARE(owners.press(1, true, &repeat), KeyOwners::Owner::Guest);
        QVERIFY(!repeat);
        QCOMPARE(owners.press(1, false, &repeat), KeyOwners::Owner::Guest);  // its repeat
        QVERIFY(repeat);
        QCOMPARE(owners.press(2, false), KeyOwners::Owner::Window);
        QCOMPARE(owners.press(2, true), KeyOwners::Owner::Window);
        QVERIFY(owners.isDown(2));
        QCOMPARE(owners.release(2), KeyOwners::Owner::Window);
        QCOMPARE(owners.release(2), KeyOwners::Owner::None);
        QVERIFY(!owners.isDown(2));
        owners.clear();
        QCOMPARE(owners.release(1), KeyOwners::Owner::None);
    }

    /* The keys go to the guest only while the pointer is on the view (the
       user, 2026-11-11: with the pointer elsewhere, Super opened both the
       desktop's overview and the guest's): elsewhere they are the window's,
       and the keys the guest had go up in it at once */
    void keysFollowThePointer()
    {
        Fixture f;
        int changes = 0;
        f.input.keysChanged = [&]() { changes++; };
        f.input.setHostActive(true);
        QVERIFY(!f.input.keysToGuest());
        QCOMPARE(f.input.key(KEY_A, true), Key::Window);
        QCOMPARE(f.input.key(KEY_A, true), Key::Window);    // its repeat
        f.input.pointerEnter(1, 1);
        QVERIFY(f.input.keysToGuest());
        QCOMPARE(changes, 1);
        /* a key pressed for the window stays the window's */
        QCOMPARE(f.input.key(KEY_A, true), Key::Window);
        QCOMPARE(f.input.key(KEY_A, false), Key::Window);
        f.log.take();
        QCOMPARE(f.input.key(KEY_Q, true), Key::Sent);
        QCOMPARE(f.log.take(), QStringList({"press 0x10"}));
        /* the pointer leaves with Q down: up in the guest at once, and its
           repeats and release go nowhere */
        f.input.pointerLeave();
        QVERIFY(!f.input.keysToGuest());
        QCOMPARE(changes, 2);
        QCOMPARE(f.log.take(), QStringList({"release 0x10", "inhibit 0", "lock 0", "confine 0"}));
        QCOMPARE(f.input.key(KEY_Q, true), Key::Kept);
        f.input.pointerEnter(2, 2);                         // back before Q is let go
        QCOMPARE(f.input.key(KEY_Q, true), Key::Kept);
        QCOMPARE(f.input.key(KEY_Q, false), Key::Kept);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0", "abs 2 2"}));
        /* the keyboard gone (another window), a key down: its release, if the
           view gets it, is not the guest's */
        QCOMPARE(f.input.key(KEY_A, true), Key::Sent);
        f.input.setHostActive(false);
        QVERIFY(!f.input.keysToGuest());
        QCOMPARE(f.log.take(), QStringList({"press 0x1e", "release 0x1e", "inhibit 0", "lock 0", "confine 0"}));
        QCOMPARE(f.input.key(KEY_A, false), Key::Window);
        QCOMPARE(changes, 4);
    }

    /* Ctrl+Alt+G's grab gives the guest the keys wherever the pointer is;
       its release with the pointer elsewhere takes them back */
    void keysWithTheGrab()
    {
        Fixture f;
        f.input.setHostActive(true);
        /* Ctrl+Alt+G's Ctrl and Alt, pressed for the window (the widget takes the G) */
        QCOMPARE(f.input.key(KEY_LEFTCTRL, true), Key::Window);
        QCOMPARE(f.input.key(KEY_LEFTALT, true), Key::Window);
        f.input.setGrab(true);
        QVERIFY(f.input.keysToGuest());
        QCOMPARE(f.input.key(KEY_LEFTALT, false), Key::Window);
        QCOMPARE(f.input.key(KEY_LEFTCTRL, false), Key::Window);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0"}));
        QCOMPARE(f.input.key(KEY_A, true), Key::Sent);
        /* released with the pointer elsewhere: A goes up in the guest */
        f.input.setGrab(false);
        QVERIFY(!f.input.keysToGuest());
        QCOMPARE(f.log.take(), QStringList({"press 0x1e", "release 0x1e", "inhibit 0", "lock 0", "confine 0"}));
        QCOMPARE(f.input.key(KEY_A, false), Key::Kept);
        QCOMPARE(f.input.key(KEY_Q, true), Key::Window);
    }

    /* Ctrl+Alt+G released with the pointer on the view holds off the grab and
       the keys until the pointer leaves, or a click on the view */
    void releaseHoldsTheKeysOff()
    {
        Fixture f;
        f.withKeys();
        QVERIFY(f.input.grabbed());
        QCOMPARE(f.input.key(KEY_A, true), Key::Sent);
        f.input.setGrab(!f.input.grabbed());               // Ctrl+Alt+G: released
        QVERIFY(!f.input.grabbed());
        QVERIFY(!f.input.keysToGuest());
        QCOMPARE(f.log.take(), QStringList({"press 0x1e", "release 0x1e", "inhibit 0", "lock 0", "confine 0"}));
        f.input.motion(5, 5);
        QCOMPARE(f.input.key(KEY_Q, true), Key::Window);
        /* a click: the grab and the keys again, then the button */
        f.log.take();
        QVERIFY(f.input.press(DisplayInput::Left, 6, 6));
        QVERIFY(f.input.grabbed());
        QVERIFY(f.input.keysToGuest());
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0", "abs 6 6", "down 0"}));
        f.input.release(DisplayInput::Left);
        QCOMPARE(f.input.key(KEY_Q, false), Key::Window);   // pressed for the window
        /* released again; the pointer out and back */
        f.input.setGrab(false);
        QVERIFY(!f.input.keysToGuest());
        f.input.pointerLeave();
        f.input.pointerEnter(7, 7);
        QVERIFY(f.input.grabbed());
        QVERIFY(f.input.keysToGuest());
    }

    /* VITRINE_VIEW_KEYS=focus: the keys go to the guest whenever the view has
       the keyboard, as before 2026-11-11 (scripted runs) */
    void keysFollowFocusSwitch()
    {
        Fixture f;
        f.input.setKeysFollowFocus(true);
        QVERIFY(!f.input.keysToGuest());
        f.input.setHostActive(true);
        QVERIFY(f.input.keysToGuest());
        QVERIFY(!f.input.grabbed());                        // the grab still follows the pointer
        QCOMPARE(f.input.key(KEY_A, true), Key::Sent);
        f.input.pointerEnter(1, 1);
        f.input.pointerLeave();
        QVERIFY(f.input.keysToGuest());
        QCOMPARE(f.input.key(KEY_A, false), Key::Sent);
        QCOMPARE(f.log.take(), QStringList({"press 0x1e", "inhibit 1", "lock 0", "confine 0", "abs 1 1",
                                            "inhibit 0", "lock 0", "confine 0", "release 0x1e"}));
        f.input.setHostActive(false);
        QVERIFY(!f.input.keysToGuest());
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

    /* A guest without a tablet: the pointer over the view takes the
       desktop's shortcuts and gives the guest the keys (Super over GRUB is
       the guest's alone), the first click locks the pointer, and relative
       motion goes to the guest only while the compositor has it locked */
    void relativePointer()
    {
        Fixture f;
        f.sink.absolute = false;
        f.input.setHostActive(true);
        f.input.pointerEnter(10, 10);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0"}));  // no absolute moves
        QVERIFY(f.input.keysToGuest());
        QCOMPARE(f.input.grabState(),
                 QString("grabbed while the pointer is on the VM (a click locks the pointer, "
                         "Ctrl+Alt+G releases)"));

        int cursorChanges = 0;
        f.input.cursorChanged = [&]() { cursorChanges++; };
        QVERIFY(!f.input.press(DisplayInput::Left, 10, 10));   // grabs instead
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "confine 0", "lock 1"}));
        QVERIFY(f.input.grabbed());
        QVERIFY(f.input.cursorHidden());
        QCOMPARE(cursorChanges, 1);
        QCOMPARE(f.input.grabState(), QString("grabbed, the pointer locked (Ctrl+Alt+G releases)"));

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

        /* Ctrl+Alt+G: the pointer free, the keys the window's until the pointer leaves */
        f.input.setGrab(false);
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        QVERIFY(!f.input.cursorHidden());
        QCOMPARE(cursorChanges, 2);
        QVERIFY(!f.input.keysToGuest());
        QCOMPARE(f.input.grabState(), QString("not grabbed (Ctrl+Alt+G grabs)"));
        f.input.pointerLeave();
        f.input.pointerEnter(10, 10);
        QVERIFY(f.input.keysToGuest());
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0"}));
    }

    /* The grab's text follows the mouse's mode, and is told again when the
       guest's tablet driver makes the mouse absolute */
    void grabTextFollowsTheMode()
    {
        Fixture f;
        f.sink.absolute = false;
        QStringList states;
        f.input.grabChanged = [&]() { states << f.input.grabState(); };
        f.withKeys();
        f.sink.absolute = true;
        f.input.mouseModeChanged();
        QCOMPARE(states, QStringList({
            "grabbed while the pointer is on the VM (a click locks the pointer, Ctrl+Alt+G releases)",
            "grabbed while the pointer is on the VM (Ctrl+Alt+G releases)",
        }));
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

    /* GNOME's dialog takes the window's keyboard as it asks about the user's
       grab: the grab stays through that loss, for the answer; once Allow
       granted it, the keyboard's loss releases it as before */
    void grabWhileAsked()
    {
        Fixture f;
        int64_t now = 1000;
        f.input.setClock([&]() { return now; });
        f.input.setAutoGrab(false);
        f.input.setHostActive(true);
        f.input.pointerEnter(1, 1);
        f.input.setGrab(true);              // the request
        f.log.take();
        f.compositor.pending = true;        // the dialog up
        now += 100;
        f.input.setHostActive(false);       // it takes the keyboard: kept
        QVERIFY(f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList());
        now += 5000;                        // Allow
        f.compositor.pending = false;
        f.input.setHostActive(true);
        QVERIFY(f.input.grabbed());
        QCOMPARE(f.input.grabState(), QString("grabbed (Ctrl+Alt+G releases)"));
        now += 1000;
        f.input.setHostActive(false);
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        /* the automatic grab is not the user's: it goes with the keyboard */
        f.input.setAutoGrab(true);
        f.input.setHostActive(true);
        f.log.take();
        f.compositor.pending = true;
        now += 100;
        f.input.setHostActive(false);
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
    }

    /* Deny leaves the request ungranted for good: the grab stays through the
       dialog's loss of the keyboard only, and a later loss (Alt+Tab away)
       releases it; a new request starts over */
    void grabAfterDeny()
    {
        Fixture f;
        int64_t now = 1000;
        f.input.setClock([&]() { return now; });
        f.input.setAutoGrab(false);
        f.input.setHostActive(true);
        f.input.pointerEnter(1, 1);
        f.input.setGrab(true);
        f.compositor.pending = true;
        f.log.take();
        now += 100;
        f.input.setHostActive(false);       // the dialog: kept
        QVERIFY(f.input.grabbed());
        now += 5000;                        // Deny: the keyboard back, still not granted
        f.input.setHostActive(true);
        QVERIFY(f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList());
        now += 1000;
        f.input.setHostActive(false);       // away: released
        QVERIFY(!f.input.grabbed());
        QCOMPARE(f.log.take(), QStringList({"inhibit 0", "lock 0", "confine 0"}));
        /* grabbed again: a new request, whose dialog's loss is kept again */
        f.input.setHostActive(true);
        f.input.setGrab(true);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 0"}));
        now += 100;
        f.input.setHostActive(false);
        QVERIFY(f.input.grabbed());
    }

    /* No dialog (a Deny GNOME remembered for the window, a compositor that
       never answers): a loss long after the request releases the grab */
    void grabLossLongAfterRequest()
    {
        Fixture f;
        int64_t now = 1000;
        f.input.setClock([&]() { return now; });
        f.input.setAutoGrab(false);
        f.input.setHostActive(true);
        f.input.pointerEnter(1, 1);
        f.input.setGrab(true);
        f.compositor.pending = true;
        f.log.take();
        now += DisplayInput::kAskedLossMs + 1;
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
        QCOMPARE(f.input.key(KEY_LEFTMETA, true), Key::Sent);
        QCOMPARE(f.log.take(), QStringList({"inhibit 1", "lock 0", "confine 1", "press 0xdb"}));
        QVERIFY(f.input.press(DisplayInput::Left, 5, 5));
        f.input.release(DisplayInput::Left);
        QCOMPARE(f.input.key(KEY_LEFTMETA, false), Key::Sent);
        QCOMPARE(f.log.take(), QStringList({"abs 5 5", "down 0", "up 0", "inhibit 1", "lock 0",
                                            "confine 0", "release 0xdb"}));

        /* without the grab nothing is confined: GNOME's unknown app, no grab of
           its own, the keys the guest's while the pointer is on the view */
        f.input.setAutoGrab(false);
        f.log.take();
        QCOMPARE(f.input.key(KEY_LEFTMETA, true), Key::Sent);
        QCOMPARE(f.log.take(), QStringList({"press 0xdb"}));
        /* the pointer elsewhere: not the guest's at all */
        QCOMPARE(f.input.key(KEY_LEFTMETA, false), Key::Sent);
        f.input.pointerLeave();
        f.log.take();
        QCOMPARE(f.input.key(KEY_LEFTMETA, true), Key::Window);
        QCOMPARE(f.log.take(), QStringList());
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
