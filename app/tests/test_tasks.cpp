// SPDX-License-Identifier: GPL-2.0-or-later
#include <QPointer>
#include <QSignalSpy>
#include <QTest>

#include "core/tasks.h"

/*
 * The app's tasks: several at once in the order they started, their
 * progress, how they end, their details, and when they leave the list -
 * a finished one once seen or dismissed, never a running one but by
 * dismiss().
 */
class TestTasks : public QObject
{
    Q_OBJECT

private slots:
    void several()
    {
        TaskList list;
        QSignalSpy added(&list, &TaskList::added);
        Task *build = list.add(Task::Kind::QemuBuild, "QEMU build");
        Task *clone = list.add(Task::Kind::Clone, "Clone of Alpha");

        QCOMPARE(list.tasks(), QList<Task *>({build, clone}));
        QCOMPARE(added.size(), 2);
        QCOMPARE(added[1][0].value<Task *>(), clone);
        QCOMPARE(build->kind(), Task::Kind::QemuBuild);
        QCOMPARE(clone->title(), QString("Clone of Alpha"));
        QVERIFY(build->isRunning());
        QCOMPARE(build->state(), Task::State::Running);
        /* busy until told how far */
        QCOMPARE(build->fraction(), -1.0);
    }

    void progress()
    {
        TaskList list;
        Task *task = list.add(Task::Kind::Clone, "Clone of Alpha");
        QSignalSpy changed(task, &Task::changed);
        QSignalSpy listChanged(&list, &TaskList::taskChanged);

        task->setProgress(25, 100);
        QCOMPARE(task->fraction(), 0.25);
        QCOMPARE(task->done(), 25);
        QCOMPARE(task->total(), 100);
        /* the same again: nothing to tell */
        task->setProgress(25, 100);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(listChanged.size(), 1);
        QCOMPARE(listChanged[0][0].value<Task *>(), task);
        /* in bytes, past 2 GiB; never past the end */
        task->setProgress(3LL << 30, 4LL << 30);
        QCOMPARE(task->fraction(), 0.75);
        task->setProgress(5, 4);
        QCOMPARE(task->fraction(), 1.0);
        /* busy again: a step that cannot tell */
        task->setProgress(0, 0);
        QCOMPARE(task->fraction(), -1.0);
        task->setText("Step 2 of 5: configuring");
        task->setText("Step 2 of 5: configuring");
        task->setTitle("QEMU update");
        QCOMPARE(task->text(), QString("Step 2 of 5: configuring"));
        QCOMPARE(task->title(), QString("QEMU update"));
        QCOMPARE(changed.size(), 6);
    }

    void finish()
    {
        TaskList list;
        Task *done = list.add(Task::Kind::QemuBuild, "QEMU build");
        Task *failed = list.add(Task::Kind::GuestToolsBuild, "Guest tools build");
        Task *stopped = list.add(Task::Kind::Clone, "Clone of Alpha");
        QSignalSpy finished(&list, &TaskList::taskFinished);
        QSignalSpy doneFinished(done, &Task::finished);

        failed->setText("Step 2 of 4: building Mesa");
        done->setText("Step 5 of 5: installing");
        done->finish(Task::State::Done);
        /* nothing to say but that it is done */
        QCOMPARE(done->text(), QString());
        failed->finish(Task::State::Failed, "Build dependencies are missing");
        stopped->finish(Task::State::Stopped);
        QCOMPARE(finished.size(), 3);
        QCOMPARE(doneFinished.size(), 1);
        QCOMPARE(done->state(), Task::State::Done);
        QVERIFY(!done->isRunning());
        QCOMPARE(failed->state(), Task::State::Failed);
        /* why it failed, rather than what it was doing */
        QCOMPARE(failed->text(), QString("Build dependencies are missing"));
        QCOMPARE(stopped->state(), Task::State::Stopped);
        /* once: a late stop does not turn a success into anything else */
        done->finish(Task::State::Stopped);
        done->finish(Task::State::Running);
        QCOMPARE(done->state(), Task::State::Done);
        QCOMPARE(doneFinished.size(), 1);
        /* finished ones stay in the list until seen */
        QCOMPARE(list.tasks().size(), 3);
    }

    /* Each its own details, else its kind's; a finished one, seen, goes */
    void details()
    {
        TaskList list;
        Task *build = list.add(Task::Kind::QemuBuild, "QEMU build");
        Task *clone = list.add(Task::Kind::Clone, "Clone of Alpha");
        Task *other = list.add(Task::Kind::Other, "Other");
        QList<Task *> shown;
        int cloneShown = 0;

        QVERIFY(!build->hasDetails());
        build->showDetails();
        list.setDetails(Task::Kind::QemuBuild, [&shown](Task *task) { shown << task; });
        list.setDetails(Task::Kind::Clone, [&shown](Task *task) { shown << task; });
        clone->setDetails([&cloneShown]() { cloneShown++; });
        QVERIFY(build->hasDetails());
        QVERIFY(clone->hasDetails());
        QVERIFY(!other->hasDetails());

        /* running: shown, and it stays */
        build->showDetails();
        clone->showDetails();
        other->showDetails();
        QCOMPARE(shown, QList<Task *>({build}));
        QCOMPARE(cloneShown, 1);
        QCOMPARE(list.tasks().size(), 3);

        /* finished: shown once more, then out of the list */
        QSignalSpy removed(&list, &TaskList::removed);
        const QPointer<Task> gone(build);
        build->finish(Task::State::Failed, "The build failed (status 2)");
        build->showDetails();
        QCOMPARE(shown, QList<Task *>({build, build}));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(list.tasks(), QList<Task *>({clone, other}));
        /* deleted later, not under the call */
        QVERIFY(gone);
        QTRY_VERIFY(!gone);

        list.setDetails(Task::Kind::Clone, {});
        QVERIFY(clone->hasDetails());
    }

    /* Details that start the job again, which dismisses its old task: no use after free */
    void detailsDismissing()
    {
        TaskList list;
        Task *task = list.add(Task::Kind::QemuBuild, "QEMU build");
        QSignalSpy removed(&list, &TaskList::removed);

        task->finish(Task::State::Failed, "Stopped");
        task->setDetails([task]() { task->dismiss(); });
        task->showDetails();
        QCOMPARE(removed.size(), 1);
        QVERIFY(list.tasks().isEmpty());
    }

    void seen()
    {
        TaskList list;
        Task *running = list.add(Task::Kind::QemuBuild, "QEMU build");
        Task *done = list.add(Task::Kind::Clone, "Clone of Alpha");

        done->finish(Task::State::Done);
        running->seen();
        done->seen();
        QCOMPARE(list.tasks(), QList<Task *>({running}));
    }

    void dismiss()
    {
        TaskList list;
        Task *running = list.add(Task::Kind::QemuBuild, "QEMU build");
        Task *failed = list.add(Task::Kind::Clone, "Clone of Alpha");
        QSignalSpy removed(&list, &TaskList::removed);
        const QPointer<Task> gone(failed);

        failed->finish(Task::State::Failed, "No space left on device");
        failed->dismiss();
        failed->dismiss();
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed[0][0].value<Task *>(), failed);
        QTRY_VERIFY(!gone);
        /* the job's own task: it may drop it, running or not */
        running->dismiss();
        QVERIFY(list.tasks().isEmpty());
    }

    void clearFinished()
    {
        TaskList list;
        Task *running = list.add(Task::Kind::QemuBuild, "QEMU build");
        Task *done = list.add(Task::Kind::Clone, "Clone of Alpha");
        Task *failed = list.add(Task::Kind::Clone, "Clone of Bravo");
        Task *stopped = list.add(Task::Kind::GuestToolsBuild, "Guest tools build");
        QSignalSpy removed(&list, &TaskList::removed);

        done->finish(Task::State::Done);
        failed->finish(Task::State::Failed, "Copying disk.qcow2 failed");
        stopped->finish(Task::State::Stopped);
        list.clearFinished();
        QCOMPARE(removed.size(), 3);
        QCOMPARE(list.tasks(), QList<Task *>({running}));
    }

    void instance()
    {
        TaskList *list = TaskList::instance();

        QVERIFY(list);
        QCOMPARE(TaskList::instance(), list);
        QCOMPARE(list->parent(), QCoreApplication::instance());
    }
};

QTEST_GUILESS_MAIN(TestTasks)
#include "test_tasks.moc"
