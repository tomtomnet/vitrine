// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "core/paths.h"
#include "core/vmstore.h"
#include "ui/argseditor.h"
#include "ui/mainwindow.h"
#include "ui/vmpane.h"

/*
 * The window's shortcuts as a user meets them, with the menus' actions
 * there too: a key two shortcuts take does nothing at all in Qt
 */
class TestShortcuts : public QObject
{
    Q_OBJECT

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
};

QTEST_MAIN(TestShortcuts)
#include "test_shortcuts.moc"
