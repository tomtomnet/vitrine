// SPDX-License-Identifier: GPL-2.0-or-later
#include <QActionGroup>
#include <QApplication>
#include <QListView>
#include <QStandardItemModel>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/mainbar.h"

/*
 * The parts of the main window's bar on their own: the tabs as buttons,
 * which the keyboard goes through as through a QTabBar, and the drop-down
 * of VMs, whose list is as wide as what it draws (test_mainwindow has the
 * bar in the window)
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

    /* The list of the drop-down as wide as a name in bold, or the state under it */
    void chooserList()
    {
        const int stateRole = Qt::UserRole + 1;
        QStandardItemModel model;
        const QString longState = "Shut down, its QEMU still open: a long line under a short name";
        for (const QString &name : {"Arch", "Bravo"}) {
            auto *item = new QStandardItem(name);
            item->setData(name == "Arch" ? longState : QString("Powered off"), stateRole);
            model.appendRow(item);
        }
        QWidget window;
        auto *chooser = new VmChooser;
        chooser->setModel(&model);
        chooser->setStateRole(stateRole);
        (new QVBoxLayout(&window))->addWidget(chooser, 0, Qt::AlignLeft);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        chooser->showPopup();
        QTRY_VERIFY(chooser->view()->isVisible());
        const int state = chooser->view()->fontMetrics().horizontalAdvance(longState);
        QVERIFY2(chooser->view()->viewport()->width() >= state,
                 qPrintable(QString("the list %1 wide for a line %2 wide")
                                .arg(chooser->view()->viewport()->width())
                                .arg(state)));
        chooser->hidePopup();

        /* the wheel over the bar does not go through the VMs */
        chooser->setCurrentIndex(0);
        QWheelEvent wheel(QPointF(5, 5), chooser->mapToGlobal(QPointF(5, 5)), QPoint(),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(chooser, &wheel);
        QCOMPARE(chooser->currentIndex(), 0);
    }
};

QTEST_MAIN(TestMainBar)
#include "test_mainbar.moc"
