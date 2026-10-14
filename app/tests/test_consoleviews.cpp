// SPDX-License-Identifier: GPL-2.0-or-later
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>

#include "core/consoleviews.h"

using View = ConsoleViews::View;
Q_DECLARE_METATYPE(std::optional<ConsoleViews::View>)

/* XKB's keycodes of the keys 1, 2, 3 above the letters, and of A, F, KP_1 */
static constexpr quint32 kKey1 = 10, kKey2 = 11, kKey3 = 12, kKeyA = 38, kKeyF = 41, kKp1 = 87;

static std::optional<View> viewOf(int key, Qt::KeyboardModifiers mods, quint32 scanCode,
                                  const QString &text = {})
{
    const QKeyEvent event(QEvent::KeyPress, key, mods, scanCode, 0, 0, text);
    return ConsoleViews::viewForKey(&event);
}

class TestConsoleViews : public QObject
{
    Q_OBJECT

private slots:
    /* From power-on: the boot, the guest's screen, its desktop, and back */
    void automatic()
    {
        ConsoleViews views;
        QSignalSpy changed(&views, &ConsoleViews::viewChanged);

        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        QCOMPARE(changed.size(), 0);
        /* the virtio-gpu card's screen shows the guest's: its console, its login screen */
        views.screenUp();
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 1);
        /* once a run */
        views.screenUp();
        QCOMPARE(changed.size(), 1);
        /* the nested KWin connected: its window */
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        QVERIFY(views.hasDesktop());
        views.setDesktop(true);
        QCOMPARE(changed.size(), 2);
        /* the session ended: the login screen is on the guest's screen */
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Screen);
        QVERIFY(!views.hasDesktop());
        /* a reboot: the boot again, then its screen when it comes back */
        views.reset();
        QCOMPARE(views.view(), View::Boot);
        views.screenUp();
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 5);
        QCOMPARE(changed.last().first().value<View>(), View::Screen);
        /* the next run starts at the boot, its desktop not there */
        views.setDesktop(true);
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        QVERIFY(!views.hasDesktop());
    }

    /* The user's choice holds until the next event that moves the view */
    void chosen()
    {
        ConsoleViews views;

        views.powerOn();
        views.choose(View::Desktop);
        QCOMPARE(views.view(), View::Desktop);
        /* the screen comes up: only a view still on the boot moves */
        views.screenUp();
        QCOMPARE(views.view(), View::Desktop);
        views.choose(View::Boot);
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        /* the boot, to look at it while the session runs: until the session ends */
        views.choose(View::Boot);
        QCOMPARE(views.view(), View::Boot);
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Screen);
        views.choose(View::Desktop);
        QCOMPARE(views.view(), View::Desktop);
        views.reset();
        QCOMPARE(views.view(), View::Boot);
    }

    void qemuConsoles()
    {
        QCOMPARE(ConsoleViews::qemuConsole(View::Boot), 0);
        QCOMPARE(ConsoleViews::qemuConsole(View::Screen), 1);
        /* the D-Bus display stays on the guest's screen behind the desktop */
        QCOMPARE(ConsoleViews::qemuConsole(View::Desktop), 1);
    }

    /* Ctrl+Alt+1/2/3 by the key's place: AZERTY's &, é and " */
    void keys_data()
    {
        const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;

        QTest::addColumn<int>("key");
        QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
        QTest::addColumn<quint32>("scanCode");
        QTest::addColumn<QString>("text");
        QTest::addColumn<std::optional<View>>("view");
        QTest::newRow("AZERTY &") << int(Qt::Key_Ampersand) << ctrlAlt << kKey1 << "&"
                                  << std::optional(View::Boot);
        QTest::newRow("AZERTY é") << int(Qt::Key_Eacute) << ctrlAlt << kKey2 << "é"
                                  << std::optional(View::Screen);
        QTest::newRow("AZERTY \"") << int(Qt::Key_QuoteDbl) << ctrlAlt << kKey3 << "\""
                                   << std::optional(View::Desktop);
        QTest::newRow("QWERTY 2") << int(Qt::Key_2) << ctrlAlt << kKey2 << "2"
                                  << std::optional(View::Screen);
        QTest::newRow("keypad 1") << int(Qt::Key_1) << (ctrlAlt | Qt::KeypadModifier) << kKp1
                                  << "1" << std::optional(View::Boot);
        /* a 1 elsewhere than the 1's place: not it */
        QTest::newRow("1 on A's place") << int(Qt::Key_1) << ctrlAlt << kKeyA << "1"
                                        << std::optional<View>();
        /* no place given (some platforms): Qt's key */
        QTest::newRow("no scan code") << int(Qt::Key_3) << ctrlAlt << quint32(0) << "3"
                                      << std::optional(View::Desktop);
        QTest::newRow("Ctrl alone") << int(Qt::Key_Ampersand) << Qt::KeyboardModifiers(
                                           Qt::ControlModifier) << kKey1 << "&"
                                    << std::optional<View>();
        QTest::newRow("Ctrl+Alt+Shift") << int(Qt::Key_1) << (ctrlAlt | Qt::ShiftModifier)
                                        << kKey1 << "1" << std::optional<View>();
        QTest::newRow("Ctrl+Alt+F") << int(Qt::Key_F) << ctrlAlt << kKeyF << "f"
                                    << std::optional<View>();
    }

    void keys()
    {
        QFETCH(int, key);
        QFETCH(Qt::KeyboardModifiers, modifiers);
        QFETCH(quint32, scanCode);
        QFETCH(QString, text);
        QFETCH(std::optional<View>, view);

        QCOMPARE(viewOf(key, modifiers, scanCode, text), view);
    }
};

QTEST_MAIN(TestConsoleViews)
#include "test_consoleviews.moc"
