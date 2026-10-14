// SPDX-License-Identifier: GPL-2.0-or-later
#include "taskbutton.h"

#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QtMath>

#include <algorithm>

#include "core/tasks.h"
#include "ui/icons.h"
#include "ui/sidebar.h"

namespace {

/* A turn of the ring while a task cannot tell how far it is, and its frames */
constexpr int kTurnMs = 1200;
constexpr int kFrameMs = 40;
/* The part of the ring that turns, in degrees */
constexpr int kTurningArc = 100;
/* The text's margin, as the VMs' (VmItemDelegate) */
constexpr int kTextMargin = 4;
/* An entry of the menu at most, in average characters */
constexpr int kEntryChars = 70;

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Base).lightnessF() < 0.5;
}

/* The colours of the VMs' state lines: running, stopped with an error */
QColor positive(const QPalette &palette)
{
    return isDark(palette) ? QColor(0x5f, 0xd3, 0x8d) : QColor(0x1e, 0x8a, 0x4c);
}

QColor negative(const QPalette &palette)
{
    return isDark(palette) ? QColor(0xff, 0x6b, 0x6b) : QColor(0xda, 0x44, 0x53);
}

/* As the actions that start the job have it */
QIcon kindIcon(Task::Kind kind)
{
    switch (kind) {
    case Task::Kind::Build:
        return Icons::themed({"run-build", "run-build-install"}, QStyle::SP_BrowserReload);
    case Task::Kind::Clone:
        return Icons::themed({"edit-copy"}, QStyle::SP_FileDialogNewFolder);
    case Task::Kind::Other:
        break;
    }
    return Icons::themed({"system-run", "media-playback-start"}, QStyle::SP_MediaPlay);
}

/* "45%"; nothing while it cannot tell */
QString percent(double fraction)
{
    return fraction < 0 ? QString() : TaskButton::tr("%1%").arg(int(fraction * 100));
}

/* What a task shows beside its title: how far, or how it ended */
QString statusOf(const Task *task)
{
    switch (task->state()) {
    case Task::State::Running:
        return percent(task->fraction());
    case Task::State::Done:
        return TaskButton::tr("Done");
    case Task::State::Failed:
        return TaskButton::tr("Failed");
    case Task::State::Stopped:
        return TaskButton::tr("Stopped");
    }
    return {};
}

/* What the ring of one task, or of several, shows */
struct Ring {
    enum class Inside { Icon, Check, Alert, Count };
    Inside inside = Inside::Icon;
    QIcon icon;
    QIcon::Mode mode = QIcon::Normal;
    int count = 0;
    /* how far, -1 turning; or whole, once over */
    double fraction = -1;
    bool whole = false;
    QColor color;
    /* one of several failed */
    bool dot = false;
};

Ring ringOf(const QList<Task *> &tasks, const QPalette &palette)
{
    Ring ring;
    QList<const Task *> running;
    bool failed = false;
    bool done = false;

    for (const Task *task : tasks) {
        if (task->isRunning()) {
            running << task;
        }
        failed = failed || task->state() == Task::State::Failed;
        done = done || task->state() == Task::State::Done;
    }
    ring.color = palette.color(QPalette::Highlight);
    if (tasks.size() == 1) {
        const Task *task = tasks.first();
        ring.icon = kindIcon(task->kind());
        switch (task->state()) {
        case Task::State::Running:
            ring.fraction = task->fraction();
            break;
        case Task::State::Done:
            ring.inside = Ring::Inside::Check;
            ring.whole = true;
            ring.color = positive(palette);
            break;
        case Task::State::Failed:
            ring.inside = Ring::Inside::Alert;
            ring.whole = true;
            ring.color = negative(palette);
            break;
        case Task::State::Stopped:
            ring.mode = QIcon::Disabled;
            ring.whole = true;
            ring.color = palette.color(QPalette::PlaceholderText);
            break;
        }
        return ring;
    }
    ring.inside = Ring::Inside::Count;
    ring.count = int(tasks.size());
    if (!running.isEmpty()) {
        /* how far those running are, if they all can tell */
        double sum = 0;
        for (const Task *task : std::as_const(running)) {
            if (task->fraction() < 0) {
                sum = -1;
                break;
            }
            sum += task->fraction();
        }
        ring.fraction = sum < 0 ? -1 : sum / running.size();
        ring.dot = failed;
        return ring;
    }
    ring.whole = true;
    ring.color = failed ? negative(palette)
                 : done ? positive(palette)
                        : palette.color(QPalette::PlaceholderText);
    return ring;
}

/*
 * @ring around @centre, @diameter wide; turned by @angle degrees while it
 * cannot tell how far; the task's icon in it @iconSize wide, on whole
 * device pixels (Icons::paint).  On the selection's colour (@selected),
 * the selected text's instead of the accent.
 */
void paintRing(QPainter *painter, const QPointF &centre, int diameter, int iconSize,
               const Ring &ring, qreal angle, const QPalette &palette, bool selected)
{
    const qreal stroke = qMax(2.0, diameter / 12.0);
    const qreal r = (diameter - stroke) / 2;
    const QRectF box(centre.x() - r, centre.y() - r, 2 * r, 2 * r);
    const QColor text = palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    const QColor color = selected && !ring.whole ? text : ring.color;
    QColor track = text;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    if (ring.whole) {
        painter->setPen(QPen(color, stroke));
        painter->drawEllipse(box);
    } else {
        track.setAlphaF(selected ? 0.35f : 0.18f);
        painter->setPen(QPen(track, stroke));
        painter->drawEllipse(box);
        painter->setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap));
        /* from the top, clockwise */
        if (ring.fraction < 0) {
            painter->drawArc(box, int((90 - angle) * 16), -kTurningArc * 16);
        } else if (ring.fraction > 0) {
            painter->drawArc(box, 90 * 16, -int(ring.fraction * 360 * 16));
        }
    }

    switch (ring.inside) {
    case Ring::Inside::Icon:
        Icons::paint(painter, ring.icon, iconSize, centre, ring.mode);
        break;
    case Ring::Inside::Check: {
        QPainterPath check;
        check.moveTo(centre + QPointF(-0.42, 0.02) * r);
        check.lineTo(centre + QPointF(-0.12, 0.32) * r);
        check.lineTo(centre + QPointF(0.44, -0.28) * r);
        painter->setPen(QPen(color, stroke * 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(check);
        break;
    }
    case Ring::Inside::Alert:
        painter->setPen(QPen(color, stroke * 1.2, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(centre + QPointF(0, -0.42) * r, centre + QPointF(0, 0.1) * r);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(centre + QPointF(0, 0.4) * r, stroke * 0.7, stroke * 0.7);
        break;
    case Ring::Inside::Count: {
        QFont font = painter->font();
        font.setBold(true);
        font.setPixelSize(qMax(6, qRound(r * 0.95)));
        painter->setFont(font);
        painter->setPen(text);
        painter->drawText(box, Qt::AlignCenter, QString::number(ring.count));
        break;
    }
    }
    if (ring.dot) {
        const QPointF at = centre + QPointF(0.72, -0.72) * r;
        painter->setPen(QPen(palette.color(QPalette::Base), stroke * 0.6));
        painter->setBrush(negative(palette));
        painter->drawEllipse(at, stroke * 1.5, stroke * 1.5);
    }
    painter->restore();
}

/*
 * A finished task leaves the list once its details are seen in @window:
 * when it ends with the window in front, or once the window comes there
 */
class SeenWatcher : public QObject
{
public:
    SeenWatcher(QWidget *window, const std::function<Task *()> &task)
        : QObject(window), m_window(window), m_task(task)
    {
        window->installEventFilter(this);
        connect(TaskList::instance(), &TaskList::taskFinished, this, [this](Task *finished) {
            if (finished == m_task()) {
                check();
            }
        });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_window && event->type() == QEvent::WindowActivate) {
            check();
        }
        return false;
    }

private:
    void check()
    {
        Task *task = m_task();
        if (task && !task->isRunning() && m_window->isVisible() && m_window->isActiveWindow()) {
            task->seen();
        }
    }

    QWidget *m_window;
    std::function<Task *()> m_task;
};

}

TaskButton::TaskButton(TaskList *tasks, QListWidget *list, QWidget *parent)
    : QAbstractButton(parent), m_tasks(tasks), m_list(list), m_spinner(new QTimer(this))
{
    setFocusPolicy(Qt::TabFocus);
    /* the moves over it: hovered again once its menu closed */
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_spinner->setInterval(kFrameMs);
    connect(m_spinner, &QTimer::timeout, this, [this]() { update(ringRect()); });
    connect(this, &QAbstractButton::clicked, this, &TaskButton::activate);
    connect(tasks, &TaskList::added, this, &TaskButton::refresh);
    connect(tasks, &TaskList::taskChanged, this, &TaskButton::refresh);
    connect(tasks, &TaskList::taskFinished, this, &TaskButton::lingerSuccesses);
    connect(tasks, &TaskList::removed, this, [this](Task *task) {
        m_lingering.remove(task);
        refresh();
    });
    /* the window in front: the successes start to go, once Qt knows it is */
    connect(qApp, &QGuiApplication::focusWindowChanged, this,
            [this]() { QTimer::singleShot(0, this, &TaskButton::lingerSuccesses); });
    refresh();
}

void TaskButton::setCompact(bool compact)
{
    if (compact != m_compact) {
        m_compact = compact;
        updateGeometry();
        update();
    }
}

QString TaskButton::title() const
{
    const QList<Task *> tasks = m_tasks->tasks();

    if (tasks.size() == 1) {
        return tasks.first()->title();
    }
    return tasks.isEmpty() ? QString() : tr("%n tasks", nullptr, int(tasks.size()));
}

QString TaskButton::status() const
{
    const QList<Task *> tasks = m_tasks->tasks();
    int failed = 0;
    int running = 0;
    bool done = false;

    if (tasks.size() == 1) {
        return statusOf(tasks.first());
    }
    for (const Task *task : tasks) {
        failed += task->state() == Task::State::Failed;
        running += task->isRunning();
        done = done || task->state() == Task::State::Done;
    }
    if (failed > 0) {
        return tr("%n failed", nullptr, failed);
    }
    if (running > 0) {
        return percent(ringOf(tasks, palette()).fraction);
    }
    return tasks.isEmpty() ? QString() : done ? tr("Done") : tr("Stopped");
}

QString TaskButton::describe(const Task *task)
{
    const QString status = statusOf(task);
    QString line = task->title();

    if (!status.isEmpty()) {
        line += " · " + status;
    }
    if (!task->text().isEmpty()) {
        line += " — " + task->text();
    }
    return line;
}

QIcon TaskButton::icon(const Task *task)
{
    switch (task->state()) {
    case Task::State::Running:
        break;
    case Task::State::Done:
        return Icons::themed({"task-complete", "emblem-success", "dialog-ok-apply"},
                             QStyle::SP_DialogApplyButton);
    case Task::State::Failed:
        return Icons::themed({"dialog-error", "emblem-error"}, QStyle::SP_MessageBoxCritical);
    case Task::State::Stopped:
        return Icons::themed({"process-stop"}, QStyle::SP_BrowserStop);
    }
    return kindIcon(task->kind());
}

void TaskButton::followSeen(QWidget *window, const std::function<Task *()> &task)
{
    new SeenWatcher(window, task);
}

QMenu *TaskButton::menu() const
{
    return m_menu;
}

int TaskButton::ringSize() const
{
    return style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this) + 10;
}

QPointF TaskButton::ringCentre() const
{
    /* where the VMs have their icon, its middle in their icons' middle */
    const qreal x = VmItemDelegate::iconInset(m_list) + m_list->iconSize().width() / 2.0;

    return QPointF(isRightToLeft() ? width() - x : x, height() / 2.0);
}

QRect TaskButton::ringRect() const
{
    const QPointF centre = ringCentre();
    const int half = ringSize() / 2 + 2;

    return QRect(qFloor(centre.x()) - half, qFloor(centre.y()) - half, 2 * half + 2, 2 * half + 2);
}

QSize TaskButton::sizeHint() const
{
    const QSize least = minimumSizeHint();

    return m_compact ? least
                     : QSize(least.width() + 12 * fontMetrics().averageCharWidth(), least.height());
}

QSize TaskButton::minimumSizeHint() const
{
    const int strip = 2 * VmItemDelegate::iconInset(m_list) + m_list->iconSize().width();

    /* the margins of the lines of a VM's item */
    return QSize(strip, qMax(ringSize(), fontMetrics().height()) + 12);
}

bool TaskButton::spins() const
{
    const QList<Task *> tasks = m_tasks->tasks();
    const Ring ring = tasks.isEmpty() ? Ring() : ringOf(tasks, palette());

    return !tasks.isEmpty() && isVisible() && ring.fraction < 0 && !ring.whole;
}

void TaskButton::refresh()
{
    const QList<Task *> tasks = m_tasks->tasks();
    QString tip;

    /* none without the list it goes under: it would be a window of its own */
    setVisible(parentWidget() && !tasks.isEmpty());
    if (tasks.size() == 1) {
        const Task *task = tasks.first();
        const QString status = statusOf(task);
        tip = "<b>" + task->title().toHtmlEscaped() + "</b>";
        if (!status.isEmpty()) {
            tip += " · " + status.toHtmlEscaped();
        }
        if (!task->text().isEmpty()) {
            tip += "<br>" + task->text().toHtmlEscaped();
        }
    } else if (!tasks.isEmpty()) {
        QStringList lines{"<b>" + title().toHtmlEscaped() + "</b>"};
        for (const Task *task : tasks) {
            lines << describe(task).toHtmlEscaped();
        }
        tip = lines.join("<br>");
    }
    setToolTip(tip);
    setAccessibleName(title());
    setAccessibleDescription(status());
    if (spins()) {
        if (!m_spinner->isActive()) {
            m_spinClock.start();
            m_spinner->start();
        }
    } else {
        m_spinner->stop();
    }
    update();
}

void TaskButton::activate()
{
    const QList<Task *> tasks = m_tasks->tasks();

    if (tasks.size() == 1) {
        tasks.first()->showDetails();
    } else if (tasks.size() > 1) {
        popupMenu({}, true);
    }
}

void TaskButton::popupMenu(const QPoint &at, bool above)
{
    const QList<Task *> tasks = m_tasks->tasks();
    const int room = kEntryChars * fontMetrics().averageCharWidth();
    auto *menu = new QMenu(this);
    bool finished = false;

    if (tasks.isEmpty()) {
        delete menu;
        return;
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName("taskMenu");
    for (Task *task : tasks) {
        QAction *entry = menu->addAction(QString());
        const auto show = [this, entry, task, room]() {
            entry->setIcon(icon(task));
            /* no mnemonic: a VM's name may have an & */
            entry->setText(fontMetrics().elidedText(describe(task), Qt::ElideRight, room)
                               .replace('&', "&&"));
        };
        show();
        connect(task, &Task::changed, entry, show);
        connect(task, &QObject::destroyed, entry, &QObject::deleteLater);
        connect(entry, &QAction::triggered, task, [task]() { task->showDetails(); });
        finished = finished || !task->isRunning();
    }
    if (finished) {
        menu->addSeparator();
        menu->addAction(Icons::themed({"edit-clear-history", "edit-clear-all", "edit-clear"},
                                      QStyle::SP_DialogResetButton),
                        tr("Clear Finished"), m_tasks, &TaskList::clearFinished);
    }
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        /* the menu had the pointer: hovered again when it moves over the button */
        m_hovered = false;
        update();
    });
    m_menu = menu;
    if (above) {
        /* over the button, as it is at the bottom of the window */
        const QSize size = menu->sizeHint();
        const QPoint corner = mapToGlobal(QPoint(isRightToLeft() ? width() - size.width() : 0, 0));
        menu->popup(corner - QPoint(0, size.height()));
    } else {
        menu->popup(at);
    }
    update();
}

void TaskButton::lingerSuccesses()
{
    const QWidget *top = window();
    const bool front = top && top != this && top->isActiveWindow();

    for (Task *task : m_tasks->tasks()) {
        if (task->state() != Task::State::Done && task->state() != Task::State::Stopped) {
            continue;
        }
        QTimer *timer = m_lingering.value(task);
        if (!timer) {
            /* the task's: gone with it */
            timer = new QTimer(task);
            timer->setSingleShot(true);
            timer->setInterval(m_linger);
            connect(timer, &QTimer::timeout, task, &Task::dismiss);
            m_lingering.insert(task, timer);
        }
        /* at a stretch: away from the front, counted again from the start */
        if (front && !timer->isActive()) {
            timer->start();
        } else if (!front) {
            timer->stop();
        }
    }
}

void TaskButton::paintEvent(QPaintEvent *)
{
    const QList<Task *> tasks = m_tasks->tasks();
    const QPalette palette = m_list->palette();
    const int inset = VmItemDelegate::iconInset(m_list);
    const qreal angle = m_spinner->isActive() ? (m_spinClock.elapsed() % kTurnMs) * 360.0 / kTurnMs
                                              : 0;
    QPainter painter(this);
    QStyleOptionViewItem option;

    if (tasks.isEmpty()) {
        return;
    }
    /* the list's background, then the button as the list draws an item */
    painter.fillRect(rect(), palette.brush(QPalette::Base));
    option.initFrom(this);
    option.widget = m_list;
    option.palette = palette;
    option.rect = rect();
    option.font = font();
    option.features = QStyleOptionViewItem::None;
    option.showDecorationSelected = true;
    option.viewItemPosition = QStyleOptionViewItem::OnlyOne;
    option.state &= ~(QStyle::State_MouseOver | QStyle::State_Selected | QStyle::State_HasFocus);
    if (isEnabled() && m_hovered) {
        option.state |= QStyle::State_MouseOver;
    }
    if (isDown() || (m_menu && m_menu->isVisible())) {
        option.state |= QStyle::State_Selected;
    }
    if (hasFocus()) {
        option.state |= QStyle::State_HasFocus;
    }
    style()->drawControl(QStyle::CE_ItemViewItem, &option, &painter, m_list);

    paintRing(&painter, ringCentre(), ringSize(), style()->pixelMetric(QStyle::PM_SmallIconSize),
              ringOf(tasks, palette), angle, palette, option.state & QStyle::State_Selected);
    if (m_compact) {
        return;
    }

    /* the title where the VMs have their names, how far at the end */
    const bool selected = option.state & QStyle::State_Selected;
    const QPalette::ColorGroup group = !(option.state & QStyle::State_Enabled) ? QPalette::Disabled
                                       : (option.state & QStyle::State_Active) ? QPalette::Normal
                                                                               : QPalette::Inactive;
    const QString title = this->title();
    const QString status = this->status();
    const QFontMetrics metrics = fontMetrics();
    const int left = VmItemDelegate::textInset(m_list) + kTextMargin;
    const QRect text = QStyle::visualRect(layoutDirection(), rect(),
                                          QRect(left, 0, width() - left - inset - kTextMargin,
                                                height()));
    const int statusWidth = status.isEmpty() ? 0 : metrics.horizontalAdvance(status);
    const int room = qMax(0, text.width() - statusWidth - (status.isEmpty() ? 0 : metrics.averageCharWidth()));
    QColor statusColor = palette.color(group, QPalette::PlaceholderText);
    int failed = 0;
    int running = 0;

    for (const Task *task : tasks) {
        failed += task->state() == Task::State::Failed;
        running += task->isRunning();
    }
    if (selected) {
        statusColor = palette.color(group, QPalette::HighlightedText);
    } else if (failed > 0) {
        statusColor = negative(palette);
    } else if (running == 0 &&
               std::any_of(tasks.begin(), tasks.end(),
                           [](const Task *task) { return task->state() == Task::State::Done; })) {
        statusColor = positive(palette);
    }
    painter.setPen(palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
    painter.drawText(QStyle::visualRect(layoutDirection(), text,
                                        QRect(text.left(), text.top(), room, text.height())),
                     QStyle::visualAlignment(layoutDirection(), Qt::AlignLeft | Qt::AlignVCenter),
                     metrics.elidedText(title, Qt::ElideRight, room));
    if (!status.isEmpty()) {
        painter.setPen(statusColor);
        painter.drawText(text, QStyle::visualAlignment(layoutDirection(),
                                                       Qt::AlignRight | Qt::AlignVCenter),
                         status);
    }
}

void TaskButton::contextMenuEvent(QContextMenuEvent *event)
{
    popupMenu(event->globalPos(), false);
}

void TaskButton::changeEvent(QEvent *event)
{
    QAbstractButton::changeEvent(event);
    switch (event->type()) {
    case QEvent::StyleChange:
    case QEvent::FontChange:
        updateGeometry();
        update();
        break;
    case QEvent::PaletteChange:
    case QEvent::ActivationChange:
        update();
        break;
    default:
        break;
    }
}

void TaskButton::enterEvent(QEnterEvent *event)
{
    QAbstractButton::enterEvent(event);
    m_hovered = true;
    update();
}

void TaskButton::resizeEvent(QResizeEvent *event)
{
    QAbstractButton::resizeEvent(event);
    /* the list collapsed under a pointer that does not move: no leave either */
    m_hovered = false;
}

void TaskButton::leaveEvent(QEvent *event)
{
    QAbstractButton::leaveEvent(event);
    m_hovered = false;
    update();
}

void TaskButton::mouseMoveEvent(QMouseEvent *event)
{
    QAbstractButton::mouseMoveEvent(event);
    if (!m_hovered && rect().contains(event->position().toPoint())) {
        m_hovered = true;
        update();
    }
}

void TaskButton::showEvent(QShowEvent *event)
{
    QAbstractButton::showEvent(event);
    refresh();
}

void TaskButton::hideEvent(QHideEvent *event)
{
    QAbstractButton::hideEvent(event);
    m_spinner->stop();
    m_hovered = false;
}
