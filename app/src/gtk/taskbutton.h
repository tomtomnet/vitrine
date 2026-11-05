// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/guarded.h"

#include <QHash>
#include <QObject>
#include <QString>

class QTimer;
class Task;
class TaskList;

/*
 * The app's tasks (TaskList) at the bottom of the list of VMs, as the user
 * asked for the Qt app: a button drawn as the list draws a VM, less tall,
 * there only while there are tasks.  A ring with the task's icon in it
 * fills as the task goes on, and turns while it cannot tell how far it
 * is; beside it the task's title and how far it is ("45%"), then how it
 * ended.  Once it is over the ring is whole, around a check mark or an
 * alert.  With several tasks the ring has their number and follows those
 * running, a dot on it while one of them failed.
 *
 * A click shows the task's details (TaskList::setDetails(): the Builds
 * window, the clone's dialog); with several, a popover lists them, each
 * with its progress, a row showing its details, and Clear Finished.  A
 * right click opens that popover too.  A finished task leaves once its
 * details are seen; a success also after a while with the window in front
 * (setLinger()), never a failure.
 *
 * While the list is hidden (F9, a narrow window), the ring alone is in the
 * VM page's header bar (compactWidget()), the same button.
 */
class TaskButton : public Gtk4::Guarded
{
public:
    /* @window: in front, the successes go after a while */
    TaskButton(TaskList *tasks, GtkWindow *window);
    ~TaskButton();
    TaskButton(const TaskButton &) = delete;
    TaskButton &operator=(const TaskButton &) = delete;

    /* For the bottom of the list of VMs */
    GtkWidget *widget() const { return m_button; }
    /* The ring alone, for the VM page's header bar while the list is hidden */
    GtkWidget *compactWidget() const { return m_compact; }
    /* The list of VMs shows: the ring alone hides */
    void setListShown(bool shown);

    /* How long a success stays with the window in front, in ms (30 s): counted
       again from the start each time the window comes back to the front */
    void setLinger(int ms) { m_linger = ms; }
    /* What it shows: "QEMU build" or "2 tasks"; "45%", "Failed" */
    QString title() const;
    QString status() const;
    /* The popover of the tasks, open or not; its rows (tests) */
    GtkWidget *popover() const { return m_popover; }
    /* A click on the button: the details, or the popover */
    void activate();
    void showPopover();

    /* "Building QEMU · 45% — Compiling QEMU": a task in a line */
    static QString describe(const Task *task);

private:
    struct Row {
        GtkWidget *row;
        GtkWidget *icon;
        GtkWidget *title;
        GtkWidget *text;
        GtkWidget *status;
        GtkWidget *progress;
    };

    /* Shown or not, the ring, the texts, the popover: after any change of the tasks */
    void refresh();
    void refreshRow(Task *task);
    void fillPopover();
    /* The successes go after a while in front of the user */
    void lingerSuccesses();
    /* @area, either ring: its tone's colour, its text's for the track */
    void draw(GtkWidget *area, cairo_t *cr, int width, int height) const;
    /* Turning while the tasks running cannot tell how far they are */
    bool spins() const;

    TaskList *m_tasks;
    GtkWindow *m_window;
    QObject m_qt;
    int m_linger = 30000;
    bool m_listShown = true;
    /* the successes' time to go, running while the window is in front */
    QHash<const Task *, QTimer *> m_lingering;
    /* the popover's rows, while it shows */
    QHash<const Task *, Row> m_rows;
    /* the rings' frame clock callbacks, while they turn */
    guint m_tick = 0;
    guint m_compactTick = 0;

    GtkWidget *m_button;
    GtkWidget *m_ring;
    GtkWidget *m_ringIcon;
    GtkWidget *m_ringCount;
    GtkWidget *m_title;
    GtkWidget *m_status;
    GtkWidget *m_compact;
    GtkWidget *m_compactRing;
    GtkWidget *m_compactIcon;
    GtkWidget *m_compactCount;
    GtkWidget *m_popover;
    GtkWidget *m_list;
    GtkWidget *m_clear;
};
