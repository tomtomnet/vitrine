// SPDX-License-Identifier: GPL-2.0-or-later
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalSpy>
#include <QSplitter>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QToolTip>

#include <QtMath>

#include <csignal>
#include <memory>

#include <sys/stat.h>

#include "core/tasks.h"
#include "core/vmcloner.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "scales.h"
#include "ui/clonedialog.h"
#include "ui/icons.h"
#include "ui/sidebar.h"
#include "ui/taskbutton.h"

/*
 * The side of the main window: the list of VMs collapsed to a strip of
 * their icons and expanded back, by the splitter's handle too, the icons
 * crisp at every scale; and the button of the app's tasks under it, in each
 * of its states: none, one running, done, failed, several, its menu, how
 * long the finished ones stay.
 */
namespace {

const char *const kNames[] = {"Alpha", "Bravo", "A Virtual Machine With a Rather Long Name"};

QIcon computer()
{
    return Icons::themed({"computer"}, QStyle::SP_ComputerIcon);
}

/* As the main window has it: the side, then the VM's pane, in a splitter */
struct Window {
    std::unique_ptr<QWidget> top;
    QSplitter *splitter;
    Sidebar *side;

    explicit Window(TaskList *tasks, int width = 900)
        : top(std::make_unique<QWidget>()), splitter(new QSplitter), side(new Sidebar(tasks))
    {
        auto *layout = new QHBoxLayout(top.get());
        auto *pane = new QWidget;

        layout->setContentsMargins(0, 0, 0, 0);
        pane->setMinimumWidth(300);
        splitter->addWidget(side);
        splitter->addWidget(pane);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setChildrenCollapsible(false);
        side->setSplitter(splitter);
        layout->addWidget(splitter);
        for (const char *name : kNames) {
            auto *item = new QListWidgetItem(computer(), name);
            item->setData(StateRole, QString("Stopped"));
            side->list()->addItem(item);
        }
        top->resize(width, 500);
    }

    QListWidget *list() const { return side->list(); }
    TaskButton *button() const { return side->taskButton(); }
};

/* Shown, laid out and active: the successes count their time */
void settle(QWidget *window)
{
    Scales::settle(window);
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));
}

void layOut()
{
    for (int i = 0; i < 4; i++) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    }
}

/* A mouse event on the splitter's handle, at @x in the window */
void drag(QSplitter *splitter, QEvent::Type type, int x)
{
    QSplitterHandle *handle = splitter->handle(1);
    const QPoint local = handle->mapFrom(splitter->window(),
                                         QPoint(x, splitter->height() / 2));
    QMouseEvent event(type, local, handle->mapToGlobal(local),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(handle, &event);
}

/* What a tooltip event on @widget at @at shows */
QString tipAt(QWidget *widget, const QPoint &at)
{
    QToolTip::hideText();
    QHelpEvent help(QEvent::ToolTip, at, widget->mapToGlobal(at));
    QApplication::sendEvent(widget, &help);
    return QToolTip::isVisible() ? QToolTip::text() : QString();
}

QStringList entries(const QMenu *menu)
{
    QStringList texts;
    for (const QAction *a : menu->actions()) {
        if (!a->isSeparator()) {
            texts << a->text();
        }
    }
    return texts;
}

}

class TestSidebar : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        Scales::quieter();
        Scales::bigScreen();
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << "/usr/share/icons");
        if (QDir("/usr/share/icons/breeze").exists()) {
            QIcon::setThemeName("breeze");
        }
        QApplication::setStyle("Fusion");
    }

    void cleanup()
    {
        QToolTip::hideText();
        while (QWidget *popup = QApplication::activePopupWidget()) {
            popup->close();
        }
    }

    /* The strip of icons and back, the list as it was; the window never takes the list's room */
    void collapse()
    {
        TaskList tasks;
        Window w(&tasks);
        settle(w.top.get());
        QListWidget *list = w.list();
        QSignalSpy changed(w.side, &Sidebar::collapsedChanged);

        w.side->setExpandedWidth(300);
        layOut();
        QCOMPARE(w.side->width(), 300);
        list->setCurrentRow(1);

        const int strip = w.side->stripWidth();
        QCOMPARE(strip, 2 * VmItemDelegate::iconInset(list) + list->iconSize().width());
        w.side->setCollapsed(true);
        layOut();
        QCOMPARE(changed.size(), 1);
        QVERIFY(w.side->isCollapsed());
        QVERIFY(w.side->property("collapsed").toBool());
        QCOMPARE(w.side->width(), strip);
        QVERIFY(list->isVisible());
        QCOMPARE(list->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        /* each item the strip's width, as high as expanded */
        QCOMPARE(list->visualItemRect(list->item(0)).width(), strip);
        QCOMPARE(list->currentRow(), 1);
        QVERIFY(w.button()->isCompact());
        /* the window wider: not the strip */
        w.top->resize(1100, 500);
        layOut();
        QCOMPARE(w.side->width(), strip);

        w.side->setCollapsed(false);
        layOut();
        QCOMPARE(changed.size(), 2);
        QCOMPARE(w.side->width(), 300);
        QVERIFY(!w.button()->isCompact());
        QCOMPARE(list->currentRow(), 1);
        /* the window narrower: the VM's side gives its room first, then not under the least */
        w.top->resize(560, 500);
        layOut();
        QVERIFY2(w.side->width() >= w.side->minimumExpandedWidth(),
                 qPrintable(QString::number(w.side->width())));
    }

    /* Dragged under half way to the strip, the list snaps to it; dragged out, it expands */
    void handle()
    {
        TaskList tasks;
        Window w(&tasks);
        settle(w.top.get());
        w.splitter->setOpaqueResize(true);
        w.side->setExpandedWidth(350);
        layOut();
        QCOMPARE(w.side->width(), 350);
        const int strip = w.side->stripWidth();
        const int least = w.side->minimumExpandedWidth();
        const int half = (strip + least) / 2;
        /* where the handle is grabbed, from its left edge */
        const int grab = w.splitter->handle(1)->width() / 2;
        const auto at = [&](int width) { return width + grab; };

        drag(w.splitter, QEvent::MouseButtonPress, at(350));
        /* narrower than the list can be, not yet half way: the least */
        drag(w.splitter, QEvent::MouseMove, at(half + 10));
        QVERIFY(!w.side->isCollapsed());
        QCOMPARE(w.side->width(), least);
        /* past it: the strip */
        drag(w.splitter, QEvent::MouseMove, at(half - 10));
        QVERIFY(w.side->isCollapsed());
        QCOMPARE(w.side->width(), strip);
        drag(w.splitter, QEvent::MouseMove, at(5));
        QCOMPARE(w.side->width(), strip);
        /* out again: expanded, the least at first */
        drag(w.splitter, QEvent::MouseMove, at(half + 2));
        QVERIFY(!w.side->isCollapsed());
        QCOMPARE(w.side->width(), least);
        drag(w.splitter, QEvent::MouseMove, at(280));
        QCOMPARE(w.side->width(), 280);
        drag(w.splitter, QEvent::MouseButtonRelease, at(280));
        layOut();
        QCOMPARE(w.side->width(), 280);
        QCOMPARE(w.side->expandedWidth(), 280);

        /* collapsed by a drag: F9 gives back the width it had before it */
        drag(w.splitter, QEvent::MouseButtonPress, at(280));
        drag(w.splitter, QEvent::MouseMove, at(strip + 3));
        drag(w.splitter, QEvent::MouseButtonRelease, at(strip + 3));
        layOut();
        QVERIFY(w.side->isCollapsed());
        QCOMPARE(w.side->width(), strip);
        /* fixed again: the window's layouts do not widen it */
        QCOMPARE(w.side->maximumWidth(), strip);
        w.side->setCollapsed(false);
        layOut();
        QCOMPARE(w.side->width(), 280);
    }

    /* Collapsed, each VM tells its name and state; expanded, only what does not fit */
    void tooltips()
    {
        TaskList tasks;
        Window w(&tasks);
        settle(w.top.get());
        QListWidget *list = w.list();
        const auto tipOf = [list](int row) {
            return tipAt(list->viewport(), list->visualItemRect(list->item(row)).center());
        };

        w.side->setExpandedWidth(220);
        layOut();
        QCOMPARE(tipOf(0), QString());
        const QString long2 = tipOf(2);
        QVERIFY2(long2.contains(kNames[2]), qPrintable(long2));

        w.side->setCollapsed(true);
        layOut();
        const QString alpha = tipOf(0);
        QVERIFY2(alpha.contains("<b>Alpha</b>") && alpha.contains("Stopped"), qPrintable(alpha));
    }

    /* One task: its title and how far, its details on a click; hidden without any */
    void oneTask()
    {
        TaskList tasks;
        Window w(&tasks);
        settle(w.top.get());
        TaskButton *button = w.button();
        int shown = 0;

        QVERIFY(!button->isVisible());
        tasks.setDetails(Task::Kind::Clone, [&shown](Task *) { shown++; });
        Task *task = tasks.add(Task::Kind::Clone, "Clone of Alpha");
        layOut();
        QVERIFY(button->isVisible());
        QCOMPARE(button->title(), QString("Clone of Alpha"));
        /* busy: nothing to tell but the turning ring */
        QCOMPARE(button->status(), QString());
        /* under the list, as wide, less tall than a VM */
        const QRect item = w.list()->visualItemRect(w.list()->item(0));
        QCOMPARE(button->width(), w.side->width());
        QVERIFY2(button->height() < item.height(),
                 qPrintable(QString("%1 high, a VM %2").arg(button->height()).arg(item.height())));
        QCOMPARE(button->geometry().bottom(), w.side->height() - 1);
        QVERIFY(button->toolTip().contains("Clone of Alpha"));

        /* turning while busy; still with a measure */
        const QImage busy = button->grab().toImage();
        QTest::qWait(250);
        QVERIFY(button->grab().toImage() != busy);
        task->setText("Copying disk.qcow2");
        task->setProgress(45, 100);
        QCOMPARE(button->status(), QString("45%"));
        QVERIFY(button->toolTip().contains("Copying disk.qcow2"));
        const QImage still = button->grab().toImage();
        QTest::qWait(250);
        QCOMPARE(button->grab().toImage(), still);

        /* running: its details, and it stays */
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(shown, 1);
        QCOMPARE(tasks.tasks().size(), 1);

        /* failed: it says so, and stays however long, until seen */
        button->setLinger(30);
        task->finish(Task::State::Failed, "No space left on device");
        QCOMPARE(button->status(), QString("Failed"));
        QVERIFY(button->toolTip().contains("No space left on device"));
        QTest::qWait(150);
        QCOMPARE(tasks.tasks().size(), 1);
        QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(shown, 2);
        QVERIFY(tasks.tasks().isEmpty());
        QVERIFY(!button->isVisible());
    }

    /* From the keyboard: Tab reaches it, Space shows the details, the menu key the menu */
    void keyboard()
    {
        TaskList tasks;
        Window w(&tasks);
        settle(w.top.get());
        TaskButton *button = w.button();
        int shown = 0;

        tasks.setDetails(Task::Kind::QemuBuild, [&shown](Task *) { shown++; });
        tasks.add(Task::Kind::QemuBuild, "QEMU build");
        layOut();
        QCOMPARE(button->focusPolicy() & Qt::TabFocus, Qt::TabFocus);
        button->setFocus(Qt::TabFocusReason);
        QVERIFY(button->hasFocus());
        QTest::keyClick(button, Qt::Key_Space);
        QCOMPARE(shown, 1);
        /* the menu key's, as Qt makes it of the key from the platform */
        QContextMenuEvent menuKey(QContextMenuEvent::Keyboard, button->rect().center(),
                                  button->mapToGlobal(button->rect().center()));
        QApplication::sendEvent(button, &menuKey);
        QTRY_VERIFY(button->menu() && button->menu()->isVisible());
        QCOMPARE(entries(button->menu()), QStringList({"QEMU build"}));
        button->menu()->close();
    }

    /* Several: their number, a menu of them with their progress, Clear Finished */
    void severalTasks()
    {
        TaskList tasks;
        Window w(&tasks);
        settle(w.top.get());
        TaskButton *button = w.button();
        QList<Task *> shown;

        button->setLinger(60000);
        tasks.setDetails(Task::Kind::QemuBuild, [&shown](Task *task) { shown << task; });
        tasks.setDetails(Task::Kind::Clone, [&shown](Task *task) { shown << task; });
        Task *build = tasks.add(Task::Kind::QemuBuild, "QEMU build");
        Task *clone = tasks.add(Task::Kind::Clone, "Clone of R&D");
        build->setProgress(20, 100);
        clone->setProgress(60, 100);
        layOut();
        QCOMPARE(button->title(), QString("2 tasks"));
        /* those running, together */
        QCOMPARE(button->status(), QString("40%"));

        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(button->menu() && button->menu()->isVisible());
        QStringList texts = entries(button->menu());
        QCOMPARE(texts.size(), 2);
        QCOMPARE(texts[0], QString("QEMU build · 20%"));
        /* an & of a name is no mnemonic */
        QCOMPARE(texts[1], QString("Clone of R&&D · 60%"));
        /* as it goes on */
        build->setText("Step 3 of 5: compiling QEMU");
        QCOMPARE(entries(button->menu())[0], QString("QEMU build · 20% — Step 3 of 5: compiling QEMU"));
        button->menu()->actions()[1]->trigger();
        QCOMPARE(shown, QList<Task *>({clone}));
        button->menu()->close();

        clone->finish(Task::State::Done, "New VM: R&D clone");
        QCOMPARE(button->status(), QString("20%"));
        build->finish(Task::State::Failed, "Compiling QEMU failed (status 2)");
        QCOMPARE(button->status(), QString("1 failed"));
        /* the context menu: the same, and Clear Finished */
        QContextMenuEvent context(QContextMenuEvent::Mouse, button->rect().center(),
                                  button->mapToGlobal(button->rect().center()));
        QApplication::sendEvent(button, &context);
        QTRY_VERIFY(button->menu() && button->menu()->isVisible());
        texts = entries(button->menu());
        QCOMPARE(texts.size(), 3);
        QCOMPARE(texts[1], QString("Clone of R&&D · Done — New VM: R&&D clone"));
        QCOMPARE(texts[2], QString("Clear Finished"));
        button->menu()->actions().last()->trigger();
        QVERIFY(tasks.tasks().isEmpty());
        QVERIFY(!button->isVisible());
    }

    /* A success goes after a while with the window in front; seen in its window, at once */
    void linger()
    {
        TaskList tasks;
        Window w(&tasks);
        settle(w.top.get());
        TaskButton *button = w.button();

        button->setLinger(100);
        Task *done = tasks.add(Task::Kind::QemuBuild, "QEMU build");
        done->finish(Task::State::Done);
        QCOMPARE(button->status(), QString("Done"));
        QTRY_VERIFY(tasks.tasks().isEmpty());

        /* not in front: it waits for the window */
        QWidget other;
        other.resize(200, 100);
        settle(&other);
        QVERIFY(!w.top->isActiveWindow());
        Task *later = tasks.add(Task::Kind::Clone, "Clone of Alpha");
        later->finish(Task::State::Done);
        QTest::qWait(300);
        QCOMPARE(tasks.tasks().size(), 1);
        w.top->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w.top.get()));
        QTRY_VERIFY(tasks.tasks().isEmpty());
    }

    /* A task's own window, in front when it ends or later: seen */
    void followSeen()
    {
        TaskList *tasks = TaskList::instance();
        QDialog dialog;
        QWidget other;
        QPointer<Task> task = tasks->add(Task::Kind::QemuBuild, "QEMU build");
        TaskButton::followSeen(&dialog, [&task]() { return task.data(); });

        dialog.resize(300, 200);
        other.resize(200, 100);
        settle(&dialog);
        task->finish(Task::State::Failed, "Stopped");
        QTRY_VERIFY(!task);

        task = tasks->add(Task::Kind::QemuBuild, "QEMU build");
        settle(&other);
        task->finish(Task::State::Failed, "Build dependencies are missing");
        QTest::qWait(100);
        QVERIFY(task);
        dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QTRY_VERIFY(!task);
    }

    /*
     * A clone goes on with its dialog closed, a task whose details show
     * the dialog again; done unseen, its details are the new VM; the
     * dialog goes once there is nothing left to show
     */
    void cloneInBackground()
    {
        QTemporaryDir tmp;
        VmStore store(tmp.filePath("vms"));
        QString error;
        Vm *source = store.create("Slow", &error);
        /* its disk a pipe: the copy waits for what the test writes */
        const QString disk = source->dir() + "/disk.img";
        QVERIFY(::mkfifo(QFile::encodeName(disk).constData(), 0600) == 0);
        QVERIFY(source->save(ArgsFile::parse("-name Slow\n-hda disk.img\n")));
        const auto feed = [&disk]() {
            QFile f(disk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("disk");
        };
        /* a copy that gives up closes the pipe under the writer */
        const auto pipe = std::signal(SIGPIPE, SIG_IGN);
        QWidget window;
        window.resize(600, 400);
        settle(&window);

        QPointer<CloneDialog> dialog = new CloneDialog(&store, source, &window);
        QSignalSpy cloned(dialog.data(), &CloneDialog::cloned);
        dialog->open();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        dialog->accept();
        QVERIFY(dialog->isCloning());
        QPointer<Task> task = dialog->findChild<VmCloner *>()->task();
        QVERIFY(task);
        QCOMPARE(task->title(), QString("Clone of Slow"));
        QVERIFY(task->hasDetails());
        /* closed: the copy goes on, the source still busy */
        dialog->reject();
        QVERIFY(!dialog->isVisible());
        QTest::qWait(50);
        QVERIFY(dialog && dialog->isCloning());
        QVERIFY(!source->runner()->busy().isEmpty());
        /* its details: the dialog again, the copy going on */
        task->showDetails();
        QVERIFY(dialog->isVisible());
        QVERIFY(task);
        dialog->reject();
        /* done unseen: kept for its task, whose details are the new VM */
        feed();
        QTRY_VERIFY(!dialog->isCloning());
        QCOMPARE(task->state(), Task::State::Done);
        QVERIFY(cloned.isEmpty());
        QTest::qWait(50);
        QVERIFY(dialog);
        task->showDetails();
        QCOMPARE(cloned.size(), 1);
        QCOMPARE(cloned[0][0].value<Vm *>()->name(), QString("Slow clone"));
        QTRY_VERIFY(!dialog);
        QTRY_VERIFY(!task);
        QVERIFY(source->runner()->busy().isEmpty());

        /* failed unseen: its details show why */
        dialog = new CloneDialog(&store, source, &window);
        dialog->open();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        dialog->accept();
        task = dialog->findChild<VmCloner *>()->task();
        dialog->reject();
        /* the copy's folder gone under it */
        QVERIFY(QDir(tmp.filePath("vms/Slow-clone-2")).removeRecursively());
        feed();
        QTRY_VERIFY(!dialog->isCloning());
        QCOMPARE(task->state(), Task::State::Failed);
        QVERIFY(!dialog->isVisible());
        task->showDetails();
        QVERIFY(dialog->isVisible());
        QTRY_VERIFY(!task);
        bool told = false;
        for (const QLabel *label : dialog->findChildren<QLabel *>()) {
            told = told || (label->isVisible() && label->text().contains("could not be cloned"));
        }
        QVERIFY(told);
        dialog->reject();
        QTRY_VERIFY(!dialog);

        /* Stop: the copy and the dialog go */
        dialog = new CloneDialog(&store, source, &window);
        dialog->open();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        dialog->accept();
        task = dialog->findChild<VmCloner *>()->task();
        auto *stop = dialog->findChild<QPushButton *>();
        for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
            if (b->text().remove('&') == "Stop") {
                stop = b;
            }
        }
        QVERIFY(stop && stop->isVisible());
        stop->click();
        QCOMPARE(task->state(), Task::State::Stopped);
        QTRY_VERIFY(!dialog);
        QVERIFY(!QFileInfo::exists(tmp.filePath("vms/Slow-clone-2")));
        QVERIFY(source->runner()->busy().isEmpty());
        QCOMPARE(store.vms().size(), 2);
        std::signal(SIGPIPE, pipe);
    }

    /* Collapsed, the VMs' icons and the task's are their pixmaps, on whole device pixels */
    void crisp()
    {
        Scales::sweep([](int) {
            using namespace Scales;
            QStringList wrong;
            TaskList tasks;
            Window w(&tasks);
            Task *task = tasks.add(Task::Kind::Clone, "Clone of Alpha");

            task->setProgress(30, 100);
            w.side->setCollapsed(true);
            w.list()->setCurrentRow(-1);
            ::settle(w.top.get());
            const qreal ratio = w.top->devicePixelRatio();
            const QImage shot = grab(w.top.get());
            const QRgb base = w.list()->palette().color(QPalette::Base).rgb();

            /* not the first: under the cursor, or current once the list has the focus */
            for (int row = 1; row < 3; row++) {
                const QRect item = w.list()->visualItemRect(w.list()->item(row));
                const QRect r(w.list()->viewport()->mapTo(w.top.get(), item.topLeft()), item.size());
                const QImage icon = computer()
                                        .pixmap(w.list()->iconSize(), ratio)
                                        .toImage()
                                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                const QPointF middle = QRectF(r).center() * ratio;
                /* the item's background, as its edge has it */
                const QRgb background = shot.pixel(qCeil(r.left() * ratio) + 1, qRound(middle.y()));
                const Found found = find(shot, icon, background,
                                         QPoint(qRound(middle.x() - icon.width() / 2.0),
                                                qRound(middle.y() - icon.height() / 2.0)),
                                         2, 2);
                if (found.off > kCrisp) {
                    wrong << QString("VM %1: not its pixmap (off by %2)").arg(row)
                                 .arg(found.off, 0, 'f', 1);
                }
            }

            /* the task's icon in the middle of its ring: the middle of it, the ring around */
            TaskButton *button = w.button();
            const int small = button->style()->pixelMetric(QStyle::PM_SmallIconSize);
            const QImage whole = Icons::themed({"edit-copy"}, QStyle::SP_FileDialogNewFolder)
                                     .pixmap(QSize(small, small), ratio)
                                     .toImage()
                                     .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            const QRect inner(whole.width() / 4, whole.height() / 4, whole.width() / 2,
                              whole.height() / 2);
            const QRect b = geometryIn(button, w.top.get());
            const QPointF middle = QRectF(b).center() * ratio;
            const Found found =
                find(shot, whole.copy(inner), base,
                     QPoint(qRound(middle.x() - whole.width() / 2.0) + inner.left(),
                            qRound(middle.y() - whole.height() / 2.0) + inner.top()),
                     2, 2);
            if (found.off > kCrisp) {
                wrong << QString("the task's icon: not its pixmap (off by %1)")
                             .arg(found.off, 0, 'f', 1);
            }
            return wrong;
        });
    }
};

QTEST_MAIN(TestSidebar)
#include "test_sidebar.moc"
