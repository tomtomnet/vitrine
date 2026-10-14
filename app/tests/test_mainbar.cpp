// SPDX-License-Identifier: GPL-2.0-or-later
#include <QActionGroup>
#include <QApplication>
#include <QHelpEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QTest>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "ui/mainbar.h"

/*
 * The parts of the main window's bar on their own: the tabs as buttons,
 * which the keyboard goes through as through a QTabBar; the menus, all of
 * them shown in the window however narrow; the buttons with icons alone,
 * whose tooltips name them (test_mainwindow has the bar in the window)
 */
class TestMainBar : public QObject
{
    Q_OBJECT

    /* Five tabs as VmPane makes them: checkable, one checked */
    static QList<QAction *> tabs(QObject *parent)
    {
        auto *group = new QActionGroup(parent);
        QList<QAction *> actions;

        for (const char *text : {"&Console", "&Details", "&Settings", "S&napshots", "L&ogs"}) {
            auto *action = new QAction(text, group);
            action->setCheckable(true);
            actions << action;
        }
        actions[0]->setChecked(true);
        return actions;
    }

    static QList<QToolButton *> buttons(const TabSwitcher *switcher)
    {
        QList<QToolButton *> buttons;

        for (QToolButton *button : switcher->findChildren<QToolButton *>()) {
            if (button->objectName() != "tabMenu") {
                buttons << button;
            }
        }
        return buttons;
    }

private slots:
    /* One stop of Tab, the current tab; the arrows to the next ones, not round */
    void tabsKeyboard()
    {
        QWidget window;
        const QList<QAction *> actions = tabs(&window);
        auto *switcher = new TabSwitcher(actions);
        (new QVBoxLayout(&window))->addWidget(switcher);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        const QList<QToolButton *> all = buttons(switcher);
        QCOMPARE(all.size(), 5);

        auto stops = [&all]() {
            QList<int> stops;
            for (int i = 0; i < all.size(); i++) {
                if (all[i]->focusPolicy() & Qt::TabFocus) {
                    stops << i;
                }
            }
            return stops;
        };
        QCOMPARE(stops(), QList<int>{0});

        all[0]->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(all[0]->hasFocus());
        QTest::keyClick(all[0], Qt::Key_Right);
        QVERIFY(actions[1]->isChecked());
        QVERIFY(all[1]->hasFocus());
        QCOMPARE(stops(), QList<int>{1});
        /* a disabled tab is skipped */
        actions[2]->setEnabled(false);
        QTest::keyClick(all[1], Qt::Key_Right);
        QVERIFY(actions[3]->isChecked());
        QVERIFY(all[3]->hasFocus());
        QTest::keyClick(all[3], Qt::Key_Right);
        QVERIFY(actions[4]->isChecked());
        /* the last one: no further, as on a QTabBar */
        QTest::keyClick(all[4], Qt::Key_Right);
        QVERIFY(actions[4]->isChecked());
        QTest::keyClick(all[4], Qt::Key_Left);
        QVERIFY(actions[3]->isChecked());
        QCOMPARE(stops(), QList<int>{3});

        /* folded: one button, Tab reaches it, the current tab's name */
        switcher->setCompact(true);
        auto *menu = switcher->findChild<QToolButton *>("tabMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QVERIFY(menu->focusPolicy() & Qt::TabFocus);
        QCOMPARE(menu->text(), QString("Snapshots"));
        for (const QToolButton *button : all) {
            QVERIFY(!button->isVisible());
        }
        QVERIFY(switcher->widthFor(true) < switcher->widthFor(false));
    }

    /*
     * The menus: in the window, never in a global menu; at the least width
     * the bar takes, each of them whole and none in the menu bar's own
     * overflow menu; the tabs then one button
     */
    void menus()
    {
        QMainWindow window;
        auto *bar = new MainBar("Bar");
        window.addToolBar(bar);
        QMenuBar *menus = bar->addMenuBar();
        for (const char *title : {"&File", "&View", "&Machine", "&Help"}) {
            menus->addMenu(title)->addAction("Entry");
        }
        bar->addStretch();
        auto *switcher = bar->addTabs(tabs(&window));
        window.setCentralWidget(new QWidget);
        window.resize(1200, 400);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(!menus->isNativeMenuBar());
        QVERIFY(menus->isVisible());
        QVERIFY(!switcher->isCompact());

        /* as narrow as the window gets: the bar as wide as it needs then */
        window.resize(100, 400);
        QTRY_VERIFY(switcher->isCompact());
        QCoreApplication::processEvents();
        QVERIFY2(bar->width() >= bar->neededWidth(true, true),
                 qPrintable(QString("the bar %1 wide, needs %2").arg(bar->width())
                                .arg(bar->neededWidth(true, true))));
        const auto *more = menus->findChild<QToolButton *>("qt_menubar_ext_button");
        QVERIFY2(!more || !more->isVisible(), "a menu in the menu bar's overflow menu");
        QCOMPARE(menus->width(), menus->sizeHint().width());
        for (QAction *menu : menus->actions()) {
            const QRect r = menus->actionGeometry(menu);
            QVERIFY2(r.isValid() && menus->rect().contains(r),
                     qPrintable(menu->text() + " cut or out of the menu bar"));
        }
        const auto *overflow = bar->findChild<QToolButton *>("qt_toolbar_ext_button");
        QVERIFY2(!overflow || !overflow->isVisible(), "the bar's overflow button shows");
    }

    /* Buttons with an icon alone: their tooltips name them, with their shortcuts */
    void iconButtons()
    {
        QMainWindow window;
        auto *bar = new MainBar("Bar");
        window.addToolBar(bar);
        auto *start = new QAction("S&tart", &window);
        start->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
        auto *off = new QAction("&Force Off", &window);
        off->setToolTip("Stop the VM at once");
        bar->addIconButtons({start, off});
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const QString startTip =
            "Start (" + QKeySequence(Qt::CTRL | Qt::Key_Return).toString(QKeySequence::NativeText) +
            ")";
        QCOMPARE(MainBar::toolTipOf(start), startTip);
        QCOMPARE(MainBar::toolTipOf(off), QString("Force Off\nStop the VM at once"));
        for (QAction *a : {start, off}) {
            auto *button = qobject_cast<QToolButton *>(bar->widgetForAction(a));
            QVERIFY(button);
            QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
        }
        /* as a hover shows it, and its new name after a change */
        start->setText("&Resume");
        auto *button = bar->widgetForAction(start);
        const QPoint at = button->rect().center();
        QHelpEvent help(QEvent::ToolTip, at, button->mapToGlobal(at));
        QApplication::sendEvent(button, &help);
        QTRY_VERIFY(QToolTip::isVisible());
        QCOMPARE(QToolTip::text(), QString(startTip).replace("Start", "Resume"));
        QToolTip::hideText();
    }
};

QTEST_MAIN(TestMainBar)
#include "test_mainbar.moc"
