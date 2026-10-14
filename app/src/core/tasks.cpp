// SPDX-License-Identifier: GPL-2.0-or-later
#include "tasks.h"

#include <QCoreApplication>
#include <QPointer>

Task::Task(Kind kind, const QString &title, TaskList *list)
    : QObject(list), m_list(list), m_kind(kind), m_title(title)
{
}

double Task::fraction() const
{
    if (m_total <= 0) {
        return -1;
    }
    return qBound(0.0, double(m_done) / double(m_total), 1.0);
}

void Task::setTitle(const QString &title)
{
    if (title != m_title) {
        m_title = title;
        emit changed();
    }
}

void Task::setText(const QString &text)
{
    if (text != m_text) {
        m_text = text;
        emit changed();
    }
}

void Task::setProgress(qint64 done, qint64 total)
{
    /* ninja's lines come by the thousand, most of them the same */
    if (done != m_done || total != m_total) {
        m_done = done;
        m_total = total;
        emit changed();
    }
}

void Task::finish(State state, const QString &text)
{
    if (!isRunning() || state == State::Running) {
        return;
    }
    m_state = state;
    if (!text.isEmpty()) {
        m_text = text;
    }
    /* how far it went is no longer what it shows */
    emit changed();
    emit finished();
}

void Task::setDetails(const std::function<void()> &show)
{
    m_details = show;
}

bool Task::hasDetails() const
{
    return m_details || m_list->m_details.contains(m_kind);
}

void Task::showDetails()
{
    /* what shows them may dismiss it, or start the job again */
    const QPointer<Task> self(this);

    if (m_details) {
        m_details();
    } else if (const auto show = m_list->m_details.value(m_kind)) {
        show(this);
    }
    if (self) {
        seen();
    }
}

void Task::seen()
{
    if (!isRunning()) {
        dismiss();
    }
}

void Task::dismiss()
{
    if (!m_dismissed) {
        m_dismissed = true;
        m_list->remove(this);
    }
}

TaskList *TaskList::instance()
{
    static QPointer<TaskList> list;

    if (!list) {
        list = new TaskList(QCoreApplication::instance());
    }
    return list;
}

TaskList::TaskList(QObject *parent) : QObject(parent)
{
}

Task *TaskList::add(Task::Kind kind, const QString &title)
{
    auto *task = new Task(kind, title, this);

    m_tasks << task;
    connect(task, &Task::changed, this, [this, task]() { emit taskChanged(task); });
    connect(task, &Task::finished, this, [this, task]() { emit taskFinished(task); });
    emit added(task);
    return task;
}

void TaskList::clearFinished()
{
    for (Task *task : QList<Task *>(m_tasks)) {
        if (!task->isRunning()) {
            task->dismiss();
        }
    }
}

void TaskList::setDetails(Task::Kind kind, const std::function<void(Task *)> &show)
{
    if (show) {
        m_details.insert(kind, show);
    } else {
        m_details.remove(kind);
    }
}

void TaskList::remove(Task *task)
{
    if (m_tasks.removeOne(task)) {
        emit removed(task);
        /* not from under a call of its own, as showDetails() */
        task->deleteLater();
    }
}
