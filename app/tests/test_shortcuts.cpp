// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QGroupBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <memory>

#include "core/paths.h"
#include "core/vmstore.h"
#include "ui/argseditor.h"
#include "ui/mainwindow.h"
#include "ui/vmpane.h"

/*
 * The window's shortcuts as a user meets them, with the menus' actions
 * there too: a key two shortcuts take does nothing at all in Qt.  And the
 * menus' keys: Alt and a menu's letter, F10.
 */
class TestShortcuts : public QObject
{
    Q_OBJECT

    /* A window with a VM, shown and active */
    struct Window {
        QTemporaryDir dir;
        std::unique_ptr<VmStore> store;
        std::unique_ptr<MainWindow> window;
    };

    static bool make(Window *w)
    {
        QFile args(w->dir.filePath("vm/vm.args"));
        if (!QDir().mkpath(w->dir.filePath("vm")) || !args.open(QIODevice::WriteOnly) ||
            args.write("-name One\n-m 1G\n") <= 0) {
            return false;
        }
        args.close();
        w->store = std::make_unique<VmStore>(w->dir.path());
        w->window = std::make_unique<MainWindow>(w->store.get());
        w->window->resize(1280, 800);
        w->window->show();
        w->window->activateWindow();
        return QTest::qWaitForWindowActive(w->window.get());
    }

    /* The menus of the window's bar by the key that opens each: Alt and its letter */
    static QHash<QKeySequence, QMenu *> menus(const QWidget *window)
    {
        QHash<QKeySequence, QMenu *> menus;

        if (const auto *bar = window->findChild<QMenuBar *>()) {
            for (QAction *a : bar->actions()) {
                if (a->menu()) {
                    menus.insert(QKeySequence::mnemonic(a->text()), a->menu());
                }
            }
        }
        return menus;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
    }

    /* Ctrl+S: Apply on the Settings tab, Settings (that tab) on the others */
    void ctrlS()
    {
        QTemporaryDir dir;
        QFile args(dir.filePath("vm/vm.args"));
        QVERIFY(QDir().mkpath(dir.filePath("vm")));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-name One\n-m 1G\n") > 0);
        args.close();
        VmStore store(dir.path());
        MainWindow window(&store);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *pane = window.findChild<VmPane *>();
        QVERIFY(pane);
        QTRY_VERIFY(pane->vm());
        QTest::failOnWarning(QRegularExpression("Ambiguous shortcut"));

        /* on the Console tab: Settings */
        pane->setTab(VmPane::Console);
        QTest::keyClick(&window, Qt::Key_S, Qt::ControlModifier);
        QTRY_COMPARE(pane->tab(), VmPane::Settings);

        /* there: Apply */
        pane->setPage(VmPane::Arguments);
        auto *editor = pane->findChild<ArgsEditor *>();
        QVERIFY(editor);
        QTRY_VERIFY(editor->isVisible());
        editor->setFocus();
        editor->moveCursor(QTextCursor::End);
        QTest::keyClicks(editor, "-smp 2");
        QTRY_VERIFY(pane->isModified());
        /* at once, as a user's Ctrl+S after a key */
        QTest::keyClick(editor, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(!pane->isModified());
        QVERIFY(args.open(QIODevice::ReadOnly));
        QVERIFY(args.readAll().contains("-smp 2"));
    }

    /*
     * Alt and a menu's letter open it on every tab and settings page, each
     * time: Qt gives a key two widgets take to each in turn, so nothing in
     * the window may have a menu's letter.  F10 opens the first menu.
     */
    void menuKeys()
    {
        Window w;
        QVERIFY(make(&w));
        MainWindow *window = w.window.get();
        auto *pane = window->findChild<VmPane *>();
        QVERIFY(pane);
        QTRY_VERIFY(pane->vm());
        const QHash<QKeySequence, QMenu *> byKey = menus(window);
        QCOMPARE(byKey.size(), 4);
        QStringList wrong;

        /* every tab, then every settings page */
        QList<std::pair<QString, std::function<void()>>> places;
        for (const VmPane::Tab tab : {VmPane::Console, VmPane::Details, VmPane::Snapshots,
                                      VmPane::Logs}) {
            places << std::pair(QString("tab %1").arg(int(tab)),
                                [pane, tab]() { pane->setTab(tab); });
        }
        for (int page = VmPane::General; page <= VmPane::Arguments; page++) {
            places << std::pair(QString("page %1").arg(page), [pane, page]() {
                pane->setTab(VmPane::Settings);
                pane->setPage(VmPane::Page(page));
            });
        }
        for (const auto &[where, go] : std::as_const(places)) {
            go();
            /* the window active, as the desktop keeps it once a menu closes:
               offscreen, a closed menu's window takes the focus as soon as
               events are processed, hence none between the presses below
               (and qWaitForWindowActive() counts that menu as the window) */
            QCoreApplication::processEvents();
            window->activateWindow();
            QTRY_VERIFY(window->isActiveWindow());
            for (auto it = byKey.cbegin(); it != byKey.cend(); ++it) {
                /* twice: a key two widgets take goes to the second the second time */
                for (int i = 0; i < 2; i++) {
                    /* the window's own: the application's may be a closed menu */
                    QWidget *focus = window->focusWidget() ? window->focusWidget() : window;
                    QMenu *menu = it.value();
                    QTest::keySequence(focus, it.key());
                    if (!QTest::qWaitFor(
                            [menu]() { return QApplication::activePopupWidget() == menu; }, 2000)) {
                        QWidget *open = QApplication::activePopupWidget();
                        wrong << QString("%1: %2 did not open %3 (%4 open)")
                                     .arg(where, it.key().toString(), menu->title(),
                                          open ? open->metaObject()->className() : "nothing");
                    }
                    while (QWidget *popup = QApplication::activePopupWidget()) {
                        popup->close();
                    }
                }
            }
        }
        /* F10: the first menu, its first entry chosen */
        pane->setTab(VmPane::Console);
        QCoreApplication::processEvents();
        window->activateWindow();
        QTRY_VERIFY(window->isActiveWindow());
        QWidget *focus = window->focusWidget() ? window->focusWidget() : window;
        QTest::keyClick(focus, Qt::Key_F10);
        QMenu *file = byKey.value(QKeySequence(Qt::ALT | Qt::Key_F));
        QVERIFY(file);
        QTRY_COMPARE(QApplication::activePopupWidget(), file);
        QCOMPARE(file->activeAction(), file->actions().value(0));
        file->close();
        for (const QString &line : wrong.mid(0, 20)) {
            qWarning("%s", qPrintable(line));
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(QString("%1 problems").arg(wrong.size())));
    }

    /*
     * No button, label with a buddy, group box or tab of the window, shown
     * or not, enabled or not, has a menu's letter: one shown later would
     * take Alt and that letter in turn with the menu
     */
    void menuLettersFree()
    {
        Window w;
        QVERIFY(make(&w));
        MainWindow *window = w.window.get();
        auto *pane = window->findChild<VmPane *>();
        QTRY_VERIFY(pane->vm());
        /* every page made */
        for (int page = VmPane::General; page <= VmPane::Arguments; page++) {
            pane->setTab(VmPane::Settings);
            pane->setPage(VmPane::Page(page));
        }
        const QHash<QKeySequence, QMenu *> byKey = menus(window);
        QCOMPARE(byKey.size(), 4);
        QStringList wrong;

        auto check = [&](const QWidget *widget, const QKeySequence &key, const QString &text) {
            if (byKey.contains(key)) {
                wrong << QString("%1 '%2' takes %3").arg(widget->metaObject()->className(), text,
                                                       key.toString());
            }
        };
        for (const QWidget *widget : window->findChildren<QWidget *>()) {
            if (widget->window() != window || qobject_cast<const QMenuBar *>(widget)) {
                continue;
            }
            if (const auto *button = qobject_cast<const QAbstractButton *>(widget)) {
                check(widget, button->shortcut(), button->text());
            } else if (const auto *label = qobject_cast<const QLabel *>(widget);
                       label && label->buddy()) {
                check(widget, QKeySequence::mnemonic(label->text()), label->text());
            } else if (const auto *box = qobject_cast<const QGroupBox *>(widget)) {
                check(widget, QKeySequence::mnemonic(box->title()), box->title());
            } else if (const auto *tabs = qobject_cast<const QTabBar *>(widget)) {
                for (int i = 0; i < tabs->count(); i++) {
                    check(widget, QKeySequence::mnemonic(tabs->tabText(i)), tabs->tabText(i));
                }
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join("; ")));
    }
};

QTEST_MAIN(TestShortcuts)
#include "test_shortcuts.moc"
