// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QPointer>
#include <QStyledItemDelegate>
#include <QWidget>

class QListWidget;
class QSplitter;
class TaskButton;
class TaskList;

/* What the list's items hold besides the name and icon */
enum VmRole {
    IdRole = Qt::UserRole,
    StateRole,          // the line under the name: "Running", "Stopped with an error"
    StateColorRole,     // its colour; none: the dimmed text's
};

/*
 * The VMs of the list: the name in bold, the state under it, beside the
 * icon; compact, the icon alone in the middle of the item, which tells the
 * name and the state in a tooltip.  An item's height is the same either
 * way: the VMs stay where they are as the list collapses.
 */
class VmItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool isCompact() const { return m_compact; }
    void setCompact(bool compact) { m_compact = compact; }
    /* The space before the icon in an item, as the style lays it out: the
       same after it, compact */
    static int iconInset(const QWidget *list);
    /* Where the item's text starts, as the style lays it out */
    static int textInset(const QWidget *list);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

private:
    bool m_compact = false;
};

/*
 * The side of the main window: the list of VMs, and under it the button
 * of the app's tasks (TaskButton), which shows while there are some.
 *
 * Collapsed, it is a strip of the VMs' icons, the button's icon under
 * them: the same list, whose items show their icon alone and tell the
 * rest in a tooltip; a click, a double click and the context menu do what
 * they do expanded.  Neither the window nor the splitter hide it: dragging
 * the splitter's handle under half way between the strip and the narrowest
 * list snaps it to the strip, and dragging the strip's out past that
 * expands it again.
 */
class Sidebar : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(bool collapsed READ isCollapsed WRITE setCollapsed NOTIFY collapsedChanged)

public:
    explicit Sidebar(TaskList *tasks, QWidget *parent = nullptr);

    QListWidget *list() const { return m_list; }
    TaskButton *taskButton() const { return m_tasks; }

    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);
    /* As wide as its icons and their margins, collapsed */
    int stripWidth() const;
    /* The narrowest it is expanded */
    int minimumExpandedWidth() const;
    /* As wide as it was left expanded, which expanding it again gives back */
    int expandedWidth() const { return m_expandedWidth; }
    void setExpandedWidth(int width);
    /*
     * The splitter whose first pane it is: expanding it gives it its
     * width back, and dragging the handle collapses it and expands it
     */
    void setSplitter(QSplitter *splitter);

signals:
    void collapsedChanged(bool collapsed);

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    /* The splitter's handle: dragged, it may snap */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /* The widths the window and the splitter may give it now */
    void applyLimits();
    /* Its width in the splitter: @width, the rest to the other pane */
    void setSplitterWidth(int width);
    /* The handle moved while dragged: the strip, or expanded */
    void handleMoved();
    /* The handle released: as wide as it was left, within its limits */
    void endDrag();
    /* A drag whose release went elsewhere: over */
    void endLostDrag();
    /* Collapsed or expanded, during a drag or not */
    void applyCollapsed(bool collapsed);

    QListWidget *m_list;
    VmItemDelegate *m_delegate;
    TaskButton *m_tasks;
    QPointer<QSplitter> m_splitter;
    bool m_collapsed = false;
    bool m_dragging = false;
    int m_expandedWidth = 260;
};
