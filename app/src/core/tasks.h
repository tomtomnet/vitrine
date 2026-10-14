// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class TaskList;

/*
 * A long job of the app, as the main window shows it under the list of
 * VMs: a build of Vitrine's QEMU or of the guest tools, the clone of a VM.
 * The job tells what it does and how far it is, then how it ended; the
 * window shows its details when asked: the build's window, the clone's
 * dialog.
 *
 * A finished task stays in the list until its details are seen or it is
 * dismissed: a failure never goes unseen.  How long a success stays
 * otherwise is the window's to say.
 */
class Task : public QObject
{
    Q_OBJECT

public:
    /* What the job is, for its icon and the window that details it */
    enum class Kind { QemuBuild, GuestToolsBuild, Clone, Other };
    enum class State { Running, Done, Failed, Stopped };
    Q_ENUM(State)

    Kind kind() const { return m_kind; }
    /* What it is, in a few words: "QEMU build", "Clone of Fedora" */
    QString title() const { return m_title; }
    /* What it does now ("Step 3 of 5: building QEMU"), or how it ended */
    QString text() const { return m_text; }
    State state() const { return m_state; }
    bool isRunning() const { return m_state == State::Running; }
    /* How far: @done of @total, in any unit; a total of 0 while it cannot tell */
    qint64 done() const { return m_done; }
    qint64 total() const { return m_total; }
    /* From 0 to 1, or -1 while it cannot tell (busy) */
    double fraction() const;

    void setTitle(const QString &title);
    void setText(const QString &text);
    void setProgress(qint64 done, qint64 total);
    /* It ended, once: Done, Failed (@text: why) or Stopped */
    void finish(State state, const QString &text = {});

    /* What shows its details, instead of what its kind has (TaskList::setDetails()) */
    void setDetails(const std::function<void()> &show);
    bool hasDetails() const;
    /* Shows them; a finished task, seen, leaves the list */
    void showDetails();
    /* Its details were seen otherwise, its window in front: a finished
       task leaves the list, a running one stays */
    void seen();
    /* Out of the list, deleted later, whatever its state: the job goes on */
    void dismiss();

signals:
    /* Its title, text, progress or state */
    void changed();
    void finished();

private:
    friend class TaskList;
    Task(Kind kind, const QString &title, TaskList *list);

    TaskList *m_list;
    Kind m_kind;
    QString m_title;
    QString m_text;
    State m_state = State::Running;
    qint64 m_done = 0;
    qint64 m_total = 0;
    std::function<void()> m_details;
    bool m_dismissed = false;
};

/*
 * The tasks of the app, which the long jobs report to, in the order they
 * started; a job finished and seen leaves it.
 */
class TaskList : public QObject
{
    Q_OBJECT

public:
    /* The app's */
    static TaskList *instance();

    explicit TaskList(QObject *parent = nullptr);

    /* A new task, running, at the end */
    Task *add(Task::Kind kind, const QString &title);
    QList<Task *> tasks() const { return m_tasks; }
    /* The finished ones leave it */
    void clearFinished();
    /* What shows the details of the tasks of @kind: the window of the builds */
    void setDetails(Task::Kind kind, const std::function<void(Task *)> &show);

signals:
    void added(Task *task);
    /* It left the list, and is deleted later */
    void removed(Task *task);
    /* Its title, text, progress or state */
    void taskChanged(Task *task);
    /* It ended */
    void taskFinished(Task *task);

private:
    friend class Task;
    void remove(Task *task);

    QList<Task *> m_tasks;
    QHash<Task::Kind, std::function<void(Task *)>> m_details;
};
