// SPDX-License-Identifier: GPL-2.0-or-later
#include "sidebar.h"

#include <QApplication>
#include <QHelpEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QSplitter>
#include <QToolTip>
#include <QVBoxLayout>

#include "core/tasks.h"
#include "ui/icons.h"
#include "ui/taskbutton.h"

namespace {

/* The text's margin in an item beside the lines of the name and the state */
constexpr int kTextMargin = 4;
/* The narrowest the list is expanded, as before it could collapse */
constexpr int kLeastExpanded = 200;

/* An item of @list as its delegate gets it, for the style's layout of it */
QStyleOptionViewItem sampleItem(const QWidget *list)
{
    QStyleOptionViewItem option;
    const auto *view = qobject_cast<const QAbstractItemView *>(list);
    const QSize icon = view ? view->iconSize() : QSize(32, 32);

    option.initFrom(list);
    option.widget = list;
    option.rect = QRect(0, 0, 400, 2 * icon.height());
    /* the icon's size is what the styles lay out, not the icon */
    option.features = QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay;
    option.decorationSize = icon;
    option.decorationPosition = QStyleOptionViewItem::Left;
    option.decorationAlignment = Qt::AlignCenter;
    option.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
    option.text = QStringLiteral("VM");
    return option;
}

QIcon::Mode iconMode(const QStyleOptionViewItem &option)
{
    if (!(option.state & QStyle::State_Enabled)) {
        return QIcon::Disabled;
    }
    return option.state & QStyle::State_Selected ? QIcon::Selected : QIcon::Normal;
}

}

int VmItemDelegate::iconInset(const QWidget *list)
{
    const QStyleOptionViewItem option = sampleItem(list);
    const QRect icon =
        list->style()->subElementRect(QStyle::SE_ItemViewItemDecoration, &option, list);

    return qMax(0, icon.left() - option.rect.left());
}

int VmItemDelegate::textInset(const QWidget *list)
{
    const QStyleOptionViewItem option = sampleItem(list);
    const QRect text = list->style()->subElementRect(QStyle::SE_ItemViewItemText, &option, list);

    return qMax(0, text.left() - option.rect.left());
}

/* Where the two lines go in an item drawn by the style without them */
static QRect textRect(const QStyleOptionViewItem &option)
{
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();

    return style->subElementRect(QStyle::SE_ItemViewItemText, &option, widget)
        .adjusted(kTextMargin, 0, -kTextMargin, 0);
}

void VmItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                           const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    const QString name = opt.text;
    const QString state = index.data(StateRole).toString();
    const bool selected = opt.state & QStyle::State_Selected;
    const QPalette::ColorGroup group =
        !(opt.state & QStyle::State_Enabled) ? QPalette::Disabled
        : (opt.state & QStyle::State_Active) ? QPalette::Normal
                                             : QPalette::Inactive;
    QFont bold = opt.font;
    QColor stateColor = index.data(StateColorRole).value<QColor>();

    opt.text.clear();
    if (m_compact) {
        /* the item as the style draws it, selected or hovered, the icon in its middle */
        const QIcon icon = opt.icon;
        const QIcon::Mode mode = iconMode(opt);
        opt.icon = QIcon();
        opt.features &= ~(QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration);
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        Icons::paint(painter, icon, opt.decorationSize.width(), QRectF(opt.rect).center(), mode);
        return;
    }
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    bold.setBold(true);
    const QRect rect = textRect(opt);
    const QFontMetrics boldMetrics(bold);
    const QFontMetrics metrics(opt.font);
    int y = rect.top() + (rect.height() - boldMetrics.height() - metrics.height()) / 2;

    if (!stateColor.isValid() || selected) {
        stateColor = opt.palette.color(group, selected ? QPalette::HighlightedText
                                                       : QPalette::PlaceholderText);
    }
    painter->save();
    painter->setFont(bold);
    painter->setPen(opt.palette.color(group, selected ? QPalette::HighlightedText
                                                      : QPalette::Text));
    painter->drawText(QRect(rect.left(), y, rect.width(), boldMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      boldMetrics.elidedText(name, Qt::ElideRight, rect.width()));
    y += boldMetrics.height();
    painter->setFont(opt.font);
    painter->setPen(stateColor);
    painter->drawText(QRect(rect.left(), y, rect.width(), metrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      metrics.elidedText(state, Qt::ElideRight, rect.width()));
    painter->restore();
}

QSize VmItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const int inset = option.widget ? iconInset(option.widget) : 0;
    QFont bold = option.font;

    bold.setBold(true);
    size.setHeight(qMax(size.height(), QFontMetrics(bold).height() +
                                           QFontMetrics(option.font).height() + 12));
    /*
     * As wide as the icon and its margins, compact; else with a few
     * letters: the list's width is the splitter's, the names elided to it.
     * Wider, the items would make the list scroll sideways.
     */
    size.setWidth(2 * inset + option.decorationSize.width() +
                  (m_compact ? 0 : 6 * QFontMetrics(bold).averageCharWidth()));
    return size;
}

bool VmItemDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view,
                               const QStyleOptionViewItem &option, const QModelIndex &index)
{
    if (event->type() != QEvent::ToolTip || !index.isValid() ||
        !index.data(Qt::ToolTipRole).toString().isEmpty()) {
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
    const QString name = index.data(Qt::DisplayRole).toString();
    const QString state = index.data(StateRole).toString();
    bool tell = m_compact;

    /* expanded: what is elided, if anything */
    if (!tell) {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const int room = textRect(opt).width();
        QFont bold = opt.font;
        bold.setBold(true);
        tell = QFontMetrics(bold).horizontalAdvance(name) > room ||
               QFontMetrics(opt.font).horizontalAdvance(state) > room;
    }
    if (!tell) {
        QToolTip::hideText();
        return false;
    }
    QToolTip::showText(event->globalPos(),
                       QString("<b>%1</b><br>%2").arg(name.toHtmlEscaped(), state.toHtmlEscaped()),
                       view->viewport(), option.rect);
    return true;
}

Sidebar::Sidebar(TaskList *tasks, QWidget *parent)
    : QWidget(parent), m_list(new QListWidget), m_delegate(new VmItemDelegate(m_list)),
      m_tasks(new TaskButton(tasks, m_list))
{
    auto *layout = new QVBoxLayout(this);

    setObjectName("sidebar");
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_tasks);
    /* the list's background under the button too: one pane, the side of the window */
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);

    m_list->setObjectName("vms");
    /* a large icon beside the two lines of each VM */
    const int large = style()->pixelMetric(QStyle::PM_LargeIconSize, nullptr, m_list);
    m_list->setIconSize(QSize(large, large));
    m_list->setItemDelegate(m_delegate);
    /* the list is the side of the window, as the pages are the side of the settings */
    m_list->setFrameShape(QFrame::NoFrame);
    /* the names elided to its width */
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tasks->setObjectName("tasks");
    applyLimits();
}

int Sidebar::stripWidth() const
{
    return 2 * VmItemDelegate::iconInset(m_list) + m_list->iconSize().width() +
           2 * m_list->frameWidth();
}

int Sidebar::minimumExpandedWidth() const
{
    return qMax(kLeastExpanded, stripWidth());
}

void Sidebar::setCollapsed(bool collapsed)
{
    /* as wide as it was left: raising the limits first resizes the strip
       to the narrowest list, which resizeEvent() would take for the user's */
    const int expanded = m_expandedWidth;

    if (collapsed == m_collapsed) {
        return;
    }
    m_collapsed = collapsed;
    m_delegate->setCompact(collapsed);
    m_tasks->setCompact(collapsed);
    /* the wheel and the keys still go through the VMs: no room for a bar */
    m_list->setVerticalScrollBarPolicy(collapsed ? Qt::ScrollBarAlwaysOff
                                                 : Qt::ScrollBarAsNeeded);
    m_list->doItemsLayout();
    if (QListWidgetItem *item = m_list->currentItem()) {
        m_list->scrollToItem(item);
    }
    m_list->viewport()->update();
    applyLimits();
    m_expandedWidth = expanded;
    /* dragged: the handle is where the mouse takes it */
    if (!m_dragging) {
        setSplitterWidth(collapsed ? stripWidth() : m_expandedWidth);
    }
    emit collapsedChanged(collapsed);
}

void Sidebar::setExpandedWidth(int width)
{
    m_expandedWidth = qMax(width, minimumExpandedWidth());
    if (!m_collapsed && !m_dragging) {
        setSplitterWidth(m_expandedWidth);
    }
}

void Sidebar::setSplitter(QSplitter *splitter)
{
    m_splitter = splitter;
    if (!splitter || splitter->count() < 2) {
        return;
    }
    connect(splitter, &QSplitter::splitterMoved, this, [this](int, int index) {
        if (index == 1 && m_dragging) {
            handleMoved();
        }
    });
    splitter->handle(1)->installEventFilter(this);
    setSplitterWidth(m_collapsed ? stripWidth() : m_expandedWidth);
}

void Sidebar::applyLimits()
{
    const int strip = stripWidth();

    if (m_dragging) {
        /* the splitter takes it across the strip, handleMoved() snaps it */
        setMinimumWidth(strip);
        setMaximumWidth(QWIDGETSIZE_MAX);
    } else if (m_collapsed) {
        setMinimumWidth(strip);
        setMaximumWidth(strip);
    } else {
        /* the window narrower takes the room from the VM's side, not from here */
        setMinimumWidth(minimumExpandedWidth());
        setMaximumWidth(QWIDGETSIZE_MAX);
    }
}

void Sidebar::setSplitterWidth(int width)
{
    if (!m_splitter || m_splitter->count() < 2) {
        return;
    }
    QList<int> sizes = m_splitter->sizes();
    const int total = sizes[0] + sizes[1];

    /* the rest to the other side; not laid out yet, the window shares it out */
    sizes[1] = total > width ? total - width : qMax(sizes[1], 800);
    sizes[0] = width;
    m_splitter->setSizes(sizes);
}

void Sidebar::handleMoved()
{
    const int strip = stripWidth();
    const int least = minimumExpandedWidth();
    const int width = m_splitter->sizes().value(0);

    if (width < (strip + least) / 2) {
        setCollapsed(true);
        if (width != strip) {
            setSplitterWidth(strip);
        }
    } else {
        setCollapsed(false);
        if (width < least) {
            setSplitterWidth(least);
        }
    }
}

bool Sidebar::eventFilter(QObject *watched, QEvent *event)
{
    const bool press = event->type() == QEvent::MouseButtonPress;
    const bool release = event->type() == QEvent::MouseButtonRelease;

    if ((press || release) && m_splitter && m_splitter->count() >= 2 &&
        watched == m_splitter->handle(1) &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        if (press) {
            m_dragging = true;
            applyLimits();
        } else if (m_dragging) {
            /* after the handle's own release, which moves it there when the
               splitter's resize is not opaque (a rubber band meanwhile) */
            QMetaObject::invokeMethod(this, &Sidebar::endDrag, Qt::QueuedConnection);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Sidebar::endDrag()
{
    if (!m_dragging) {
        return;
    }
    m_dragging = false;
    applyLimits();
    if (!m_collapsed) {
        m_expandedWidth = qMax(width(), minimumExpandedWidth());
    }
    setSplitterWidth(m_collapsed ? stripWidth() : m_expandedWidth);
}

void Sidebar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    /* as the user leaves it: by the handle once released (eventFilter()) */
    if (!m_collapsed && !m_dragging && isVisible() && width() >= minimumExpandedWidth()) {
        m_expandedWidth = width();
    }
}

void Sidebar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        applyLimits();
        if (m_collapsed && !m_dragging) {
            setSplitterWidth(stripWidth());
        }
    }
}
