// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/tasks.h"
#include "gtk/gtkloop.h"
#include "gtk/taskbutton.h"
#endif

/*
 * The tasks at the bottom of the GTK app's list of VMs, on a display no
 * one sees: there only with tasks, one task's title and how far, several
 * counted; a click shows one's details, or the popover of several, each
 * row with its progress, and Clear Finished; the ring alone while the list
 * is hidden; a success leaves after a while with the window in front, a
 * failure once seen.
 */
class TestGtkTasks : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;

    static QString text(GtkWidget *label)
    {
        return QString::fromUtf8(gtk_label_get_text(GTK_LABEL(label)));
    }

    /* The popover's rows */
    static int rows(const TaskButton &button)
    {
        GtkWidget *list = nullptr;
        for (GtkWidget *w = gtk_popover_get_child(GTK_POPOVER(button.popover())); w && !list;) {
            if (GTK_IS_LIST_BOX(w)) {
                list = w;
            } else {
                w = gtk_widget_get_first_child(w);
            }
        }
        int n = 0;
        for (GtkWidget *row = list ? gtk_widget_get_first_child(list) : nullptr; row;
             row = gtk_widget_get_next_sibling(row)) {
            n += GTK_IS_LIST_BOX_ROW(row);
        }
        return n;
    }

private slots:
    void initTestCase()
    {
        QString error;

        QStandardPaths::setTestModeEnabled(true);
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        m_loop = new GtkLoop("org.vitrine.Vitrine");
        QCOMPARE(m_loop->start(&error), GtkLoop::Start::Primary);
    }

    void cleanupTestCase()
    {
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    /* One task: its title, how far, its details; a failure leaves once seen */
    void one()
    {
        TaskList tasks;
        GtkWidget *window = gtk_window_new();
        TaskButton button(&tasks, GTK_WINDOW(window));
        int details = 0;

        QVERIFY(!gtk_widget_get_visible(button.widget()));
        QVERIFY(!gtk_widget_get_visible(button.compactWidget()));
        tasks.setDetails(Task::Kind::Build, [&details](Task *) { details++; });
        Task *task = tasks.add(Task::Kind::Build, "Building QEMU");
        QVERIFY(gtk_widget_get_visible(button.widget()));
        QCOMPARE(button.title(), QString("Building QEMU"));
        /* busy: no number */
        QCOMPARE(button.status(), QString());
        task->setProgress(45, 100);
        QCOMPARE(button.status(), QString("45%"));
        task->setText("Compiling QEMU");
        const QString tip = QString::fromUtf8(gtk_widget_get_tooltip_markup(button.widget()));
        QCOMPARE(tip, QString("<b>Building QEMU</b> · 45%\nCompiling QEMU"));
        QCOMPARE(TaskButton::describe(task), QString("Building QEMU · 45% — Compiling QEMU"));

        button.activate();
        QCOMPARE(details, 1);
        /* running: it stays */
        QCOMPARE(tasks.tasks().size(), 1);
        task->finish(Task::State::Failed, "The build failed");
        QCOMPARE(button.status(), QString("Failed"));
        button.activate();
        QCOMPARE(details, 2);
        QTRY_VERIFY(tasks.tasks().isEmpty());
        QVERIFY(!gtk_widget_get_visible(button.widget()));
        gtk_window_destroy(GTK_WINDOW(window));
    }

    /* Several: counted, the popover of them, Clear Finished */
    void several()
    {
        TaskList tasks;
        GtkWidget *window = gtk_window_new();
        TaskButton button(&tasks, GTK_WINDOW(window));

        gtk_window_set_child(GTK_WINDOW(window), button.widget());
        gtk_window_present(GTK_WINDOW(window));
        Task *build = tasks.add(Task::Kind::Build, "Building QEMU");
        Task *clone = tasks.add(Task::Kind::Clone, "Clone of Fedora");
        build->setProgress(20, 100);
        clone->setProgress(60, 100);
        QCOMPARE(button.title(), QString("2 tasks"));
        QCOMPARE(button.status(), QString("40%"));
        button.activate();
        QTRY_VERIFY(gtk_widget_get_visible(button.popover()));
        QCOMPARE(rows(button), 2);

        clone->finish(Task::State::Done, "New VM: Fedora 2");
        QCOMPARE(button.status(), QString("20%"));
        build->finish(Task::State::Failed, "It failed");
        QCOMPARE(button.status(), QString("1 failed"));
        tasks.clearFinished();
        QTRY_VERIFY(tasks.tasks().isEmpty());
        QTRY_VERIFY(!gtk_widget_get_visible(button.popover()));
        QVERIFY(!gtk_widget_get_visible(button.widget()));
        gtk_window_destroy(GTK_WINDOW(window));
    }

    /* The list hidden: the ring alone, while there are tasks */
    void compact()
    {
        TaskList tasks;
        GtkWidget *window = gtk_window_new();
        TaskButton button(&tasks, GTK_WINDOW(window));

        button.setListShown(false);
        QVERIFY(!gtk_widget_get_visible(button.compactWidget()));
        Task *task = tasks.add(Task::Kind::Other, "Installing the guest tools");
        QVERIFY(gtk_widget_get_visible(button.compactWidget()));
        button.setListShown(true);
        QVERIFY(!gtk_widget_get_visible(button.compactWidget()));
        button.setListShown(false);
        task->dismiss();
        QTRY_VERIFY(!gtk_widget_get_visible(button.compactWidget()));
        gtk_window_destroy(GTK_WINDOW(window));
    }

    /* A success leaves after a while with the window in front */
    void linger()
    {
        TaskList tasks;
        GtkWidget *window = gtk_window_new();
        TaskButton button(&tasks, GTK_WINDOW(window));

        button.setLinger(100);
        gtk_window_set_child(GTK_WINDOW(window), button.widget());
        gtk_window_present(GTK_WINDOW(window));
        if (!QTest::qWaitFor([window]() { return gtk_window_is_active(GTK_WINDOW(window)); }, 3000)) {
            gtk_window_destroy(GTK_WINDOW(window));
            QSKIP("the window does not come to the front on this display");
        }
        Task *done = tasks.add(Task::Kind::Clone, "Clone of Fedora");
        Task *failed = tasks.add(Task::Kind::Build, "Building QEMU");
        done->finish(Task::State::Done);
        failed->finish(Task::State::Failed, "It failed");
        QTRY_COMPARE(tasks.tasks().size(), 1);
        QCOMPARE(tasks.tasks().first(), failed);
        QTest::qWait(300);
        QCOMPARE(tasks.tasks().size(), 1);
        gtk_window_destroy(GTK_WINDOW(window));
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtktasks-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkTasks test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtktasks.moc"
