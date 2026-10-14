// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QAbstractButton>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>

#include <functional>

class QListWidget;
class QMenu;
class QTimer;
class Task;
class TaskList;

/*
 * The app's tasks (TaskList) under the list of VMs: a button drawn as the
 * list draws a VM, less tall, hidden while there is no task.  A ring with
 * the task's icon in it fills as the task goes on, and turns while the
 * task cannot tell how far it is; beside it, the task's title and how far
 * it is.  Once the task is over, the ring is whole, around a check mark
 * or an exclamation mark.  With several tasks, the ring has their number
 * and follows those running, a dot on it while one of them failed.
 *
 * A click shows the task's details (the build's window, the clone's
 * dialog); with several, the menu of the tasks, each with its progress,
 * whose entries show their details.  The context menu has them too, and
 * Clear Finished.  A finished task leaves once its details are seen; a
 * success also after a while with the window in front (setLinger()),
 * never a failure.
 *
 * Compact, while the list is collapsed: the ring alone, the rest in the
 * tooltip.
 */
class TaskButton : public QAbstractButton
{
    Q_OBJECT

public:
    /* @list: the list it goes under, whose items it looks like */
    TaskButton(TaskList *tasks, QListWidget *list, QWidget *parent = nullptr);

    bool isCompact() const { return m_compact; }
    void setCompact(bool compact);
    /* How long a success stays once the window is in front, in ms */
    int linger() const { return m_linger; }
    void setLinger(int ms) { m_linger = ms; }
    /* What it shows: "QEMU build" or "2 tasks"; "45%", "Failed" */
    QString title() const;
    QString status() const;
    /* The menu of the tasks, while it is open */
    QMenu *menu() const;
    /* The ring's diameter, logical pixels */
    int ringSize() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    /* "QEMU build · 45% — Step 3 of 5: compiling QEMU": the menu's entries */
    static QString describe(const Task *task);
    /* The menu's icon of @task: its kind's while it runs, then how it ended */
    static QIcon icon(const Task *task);
    /*
     * The task of @window's job, as @task tells it (the builder's, now), is
     * seen while @window is in front: once it is over, it leaves the list
     */
    static void followSeen(QWidget *window, const std::function<Task *()> &task);

protected:
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    /* Hovered, by the pointer's own events: on Wayland the button hears
       of no leave after its menu took the pointer */
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    /* Shown or not, the tooltip, the spinner: after any change of the tasks */
    void refresh();
    /* A click: the details, or the menu */
    void activate();
    /* At @at, or over the button */
    void popupMenu(const QPoint &at, bool above);
    /* The successes go after a while in front of the user */
    void lingerSuccesses();
    /* Turning while the tasks running cannot tell how far they are */
    bool spins() const;
    /* Where the ring goes, and what it covers */
    QPointF ringCentre() const;
    QRect ringRect() const;

    TaskList *m_tasks;
    QListWidget *m_list;
    bool m_compact = false;
    int m_linger = 30000;
    QPointer<QMenu> m_menu;
    QTimer *m_spinner;
    QElapsedTimer m_spinClock;
    /* The successes whose time to go is counted */
    QSet<const Task *> m_lingering;
    bool m_hovered = false;
};
