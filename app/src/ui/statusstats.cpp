// SPDX-License-Identifier: GPL-2.0-or-later
#include "statusstats.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>

#include <algorithm>
#include <iterator>

#include "core/guesttools.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/icons.h"
#include "ui/statstext.h"
#include "ui/vmconsole.h"
#include "ui/widgets.h"
#include "vmview/vmview.h"

using VmStats::Sampler;

/* In the settings: statusbar/KEY */
static const char *const kKeys[StatusStats::kStats] = {"cpu", "memory", "disk", "network",
                                                       "gpu", "frames", "mainloop"};
/* What shows at first: the CPU and memory, and what the status bar had before */
static const bool kDefaults[StatusStats::kStats] = {true, true, false, false, false, true, true};
/* A statistic narrower than its place this long gets a narrower place */
static int shrinkMs = 10000;

static QString settingKey(const QString &key)
{
    return "statusbar/" + key;
}

static bool setting(const QString &key, bool fallback)
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat)
        .value(settingKey(key), fallback)
        .toBool();
}

static void setSetting(const QString &key, bool on)
{
    QSettings(Paths::settingsPath(), QSettings::IniFormat).setValue(settingKey(key), on);
}

StatusStats::StatusStats(QWidget *parent)
    : QWidget(parent), m_sampler(new Sampler(this)), m_refresh(new QTimer(this)),
      m_button(Widgets::statusButton(
          "statsMenu",
          Icons::themed({"view-statistics", "office-chart-bar", "utilities-system-monitor"},
                        QStyle::SP_FileDialogDetailedView))),
      m_menu(new QMenu(this))
{
    setObjectName("statusStats");
    for (int i = 0; i < kStats; i++) {
        m_names[i] = StatsText::name(StatsText::Stat(i));
    }
    for (int i = 0; i < kStats; i++) {
        m_labels[i] = new QLabel(this);
        m_labels[i]->setObjectName(QString("stat-") + kKeys[i]);
        m_labels[i]->setTextFormat(Qt::PlainText);
        m_labels[i]->installEventFilter(this);
        m_labels[i]->hide();
        m_actions[i] = m_menu->addAction(m_names[i]);
        m_actions[i]->setCheckable(true);
        m_actions[i]->setChecked(setting(kKeys[i], kDefaults[i]));
        m_actions[i]->setToolTip(StatsText::tip(StatsText::Stat(i)));
        connect(m_actions[i], &QAction::toggled, this, [this, i](bool on) {
            setSetting(kKeys[i], on);
            updateSources();
            refresh();
        });
    }
    m_menu->addSeparator();
    m_menu->setToolTipsVisible(true);
    connect(m_menu, &QMenu::aboutToShow, this, &StatusStats::updateMenu);
    /* apart from what follows, as the status bar's other labels are */
    setContentsMargins(0, 0, gap(), 0);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    std::fill(std::begin(m_narrowSince), std::end(m_narrowSince), -1);
    m_clock.start();

    /* the end of the status bar: an icon, no text, the menu at a click */
    m_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_button->setPopupMode(QToolButton::InstantPopup);
    m_button->setMenu(m_menu);
    m_button->setToolTip(tr("What the status bar shows"));
    /* shown once in the status bar: without a parent it would be a window */
    m_button->installEventFilter(this);

    /* the sampler's readers and the screen tell one by one: one refresh
       for those of a moment */
    m_refresh->setSingleShot(true);
    m_refresh->setInterval(0);
    connect(m_refresh, &QTimer::timeout, this, &StatusStats::refresh);
    connect(m_sampler, &Sampler::changed, m_refresh, qOverload<>(&QTimer::start));
    hide();
    if (parent) {
        watchWindow();
    }
}

void StatusStats::setVm(Vm *vm, VmConsole *console)
{
    if (vm != m_vm) {
        /* another VM, other numbers: as wide as they need */
        std::fill(std::begin(m_widths), std::end(m_widths), 0);
        std::fill(std::begin(m_narrowSince), std::end(m_narrowSince), -1);
    }
    if (m_console) {
        disconnect(m_console, nullptr, this, nullptr);
        disconnect(m_console, nullptr, m_refresh, nullptr);
    }
    m_vm = vm;
    m_console = console;
    if (console) {
        connect(console, &VmConsole::statsChanged, m_refresh, qOverload<>(&QTimer::start));
        /* its screen came or went: the frames from it, or from QEMU */
        connect(console, &VmConsole::changed, this, &StatusStats::updateSources);
    }
    m_sampler->setVm(vm);
    updateSources();
    refresh();
}

QWidget *StatusStats::optional(const QString &key, const QString &text, QWidget *widget)
{
    auto *holder = new QWidget;
    auto *layout = new QHBoxLayout(holder);
    QAction *action = m_menu->addAction(text);

    holder->setObjectName(key + "Item");
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(widget);
    action->setCheckable(true);
    action->setChecked(setting(key, true));
    m_optional.append({action, widget, holder});
    /* the widget's own show() and hide() come here */
    widget->installEventFilter(this);
    showOptional(m_optional.last());
    connect(action, &QAction::toggled, this, [this, key, action](bool on) {
        setSetting(key, on);
        for (const Optional &item : std::as_const(m_optional)) {
            if (item.action == action) {
                showOptional(item);
            }
        }
    });
    return holder;
}

/* Shown when the user wants it and it has something to say (not hidden by
   its owner): a holder with nothing in it would still take room */
void StatusStats::showOptional(const Optional &item)
{
    const bool hidden = item.widget->isHidden() &&
                        item.widget->testAttribute(Qt::WA_WState_ExplicitShowHide);
    const bool shown = item.action->isChecked() && !hidden;

    /* not in the status bar yet: it shows the holder when added, unless hidden */
    if (!item.holder->parentWidget()) {
        if (!shown) {
            item.holder->hide();
        }
        return;
    }
    item.holder->setVisible(shown);
}

bool StatusStats::isShown(Stat stat) const
{
    return m_actions[stat]->isChecked();
}

void StatusStats::setShown(Stat stat, bool shown)
{
    m_actions[stat]->setChecked(shown);
}

Sampler::Sources StatusStats::sources() const
{
    return m_sampler->sources();
}

/* What the sampler reads: what shows, and the details of the tooltip open */
void StatusStats::updateSources()
{
    Sampler::Sources sources;
    const bool screen = m_console && m_console->view();

    if (!m_window || !m_window->isVisible() || m_window->isMinimized()) {
        m_sampler->setSources({});
        return;
    }
    if (isShown(Cpu) || isShown(MainLoop)) {
        sources |= Sampler::Cpu;
    }
    if (isShown(Memory)) {
        sources |= Sampler::Memory;
    }
    if (isShown(Disk)) {
        sources |= Sampler::Disk;
    }
    if (isShown(Network)) {
        sources |= Sampler::Network;
    }
    if (isShown(Gpu)) {
        sources |= Sampler::Gpu;
    }
    /* the screen in vitrine's window measures its own frames; QEMU's SDL display, its */
    if (isShown(Frames) && !screen) {
        sources |= Sampler::Display;
    }
    if (m_tip == Cpu || m_tip == MainLoop) {
        sources |= Sampler::Kvm;
    }
    if (m_tip == Memory) {
        sources |= Sampler::MemoryDetails;
    }
    m_sampler->setSources(sources);
}

VmStats::Snapshot StatusStats::snapshot() const
{
    VmStats::Snapshot s = m_sampler->snapshot();
    const VmView *view = m_console ? m_console->view() : nullptr;

    if (!view) {
        return s;
    }
    const QScreen *screen = m_console->screen();
    StatsText::mergeScreen(&s, m_console->stats(), view->guestSize().width(),
                           view->guestSize().height(), screen ? screen->refreshRate() : 0);
    return s;
}

QString StatusStats::text(Stat stat, const VmStats::Snapshot &s) const
{
    return StatsText::label(StatsText::Stat(stat), s);
}

QList<int> StatusStats::candidates() const
{
    QList<int> list;

    for (int i = 0; i < kStats; i++) {
        if (isShown(Stat(i)) && !m_labels[i]->text().isEmpty()) {
            list << i;
        }
    }
    return list;
}

int StatusStats::gap() const
{
    /* as the status bar's other labels keep from what follows them */
    return fontMetrics().averageCharWidth() * 3;
}

void StatusStats::setShrinkDelay(int ms)
{
    shrinkMs = ms;
}

void StatusStats::refresh()
{
    const VmStats::Snapshot s = snapshot();
    const qint64 now = m_clock.elapsed();

    for (int i = 0; i < kStats; i++) {
        const QString t = isShown(Stat(i)) ? text(Stat(i), s) : QString();
        m_labels[i]->setText(t);
        if (t.isEmpty()) {
            continue;
        }
        /*
         * Its place grows at once, and shrinks to the widest it was once
         * narrower for a while: what follows moves once in a while, not
         * at each new value, and keeps no gap for long after a value too
         * wide for once
         */
        const int need = m_labels[i]->sizeHint().width();
        if (need >= m_widths[i]) {
            m_widths[i] = need;
            m_narrowSince[i] = -1;
        } else if (m_narrowSince[i] < 0) {
            m_narrowSince[i] = now;
            m_widestSince[i] = need;
        } else {
            m_widestSince[i] = qMax(m_widestSince[i], need);
            if (now - m_narrowSince[i] >= shrinkMs) {
                m_widths[i] = m_widestSince[i];
                m_narrowSince[i] = -1;
            }
        }
    }
    /* never a window of its own */
    setVisible(parentWidget() && !candidates().isEmpty());
    /* the status bar lays out again only when the room asked changes */
    if (const QSize hint = sizeHint(); hint != m_hint) {
        m_hint = hint;
        updateGeometry();
    }
    relayout();
    /* an open tooltip follows the numbers; a closed one no longer asks for details */
    if (m_tip >= 0) {
        QLabel *label = m_labels[m_tip];
        if (QToolTip::isVisible() && label->isVisible() && label->underMouse()) {
            QToolTip::showText(QCursor::pos(), tooltip(Stat(m_tip)), label);
        } else {
            m_tip = -1;
            updateSources();
        }
    }
    if (m_menu->isVisible()) {
        updateMenu();
    }
}

QSize StatusStats::sizeHint() const
{
    const QList<int> shown = candidates();
    int width = 0, height = 0;

    for (const int i : shown) {
        width += m_widths[i];
        height = qMax(height, m_labels[i]->sizeHint().height());
    }
    if (shown.size() > 1) {
        width += gap() * int(shown.size() - 1);
    }
    const QMargins m = contentsMargins();
    return QSize(width + m.left() + m.right(),
                 qMax(height, fontMetrics().height()) + m.top() + m.bottom());
}

QSize StatusStats::minimumSizeHint() const
{
    /* whatever is left of the window's width: the window does not keep
       wider for the statistics */
    return QSize(0, sizeHint().height());
}

void StatusStats::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

/* As many as fit, the first first, the rest left out */
void StatusStats::relayout()
{
    const QRect area = contentsRect();
    QList<int> placed;
    int used = 0;

    for (const int i : candidates()) {
        const int width = m_widths[i] + (placed.isEmpty() ? 0 : gap());
        if (used + width > area.width()) {
            break;
        }
        used += width;
        placed << i;
    }
    /* by the notices that follow */
    int x = area.right() + 1 - used;
    for (int i = 0; i < kStats; i++) {
        if (!placed.contains(i)) {
            m_labels[i]->hide();
            continue;
        }
        m_labels[i]->setGeometry(x, area.top(), m_widths[i], area.height());
        m_labels[i]->show();
        x += m_widths[i] + gap();
    }
}

/* The notes of what the selected VM cannot show */
void StatusStats::updateMenu()
{
    const VmStats::Snapshot &s = m_sampler->snapshot();

    for (int i = 0; i < kStats; i++) {
        const QString note =
            StatsText::note(StatsText::Stat(i), m_vm, m_sampler->guestSource(), s);
        m_actions[i]->setText(note.isEmpty() ? m_names[i] : tr("%1 (%2)").arg(m_names[i], note));
    }
}

QString StatusStats::tooltip(Stat stat) const
{
    const VmStats::Snapshot s = snapshot();
    const QString name = m_vm ? m_vm->name().toHtmlEscaped() : QString();
    StatsText::Context context;

    context.toolsFor = m_vm && StatsText::toolsAreFor(m_vm);
    if (m_vm) {
        context.ramMiB = VmConfig::memoryMiB(m_vm->runner()->runArgs());
    }
    context.guest = m_sampler->guestSource();
    if (m_console && m_console->view()) {
        context.screen = &m_console->stats();
    }
    const QString body = StatsText::details(StatsText::Stat(stat), s, context);
    /* on lines of their own, titles too: QToolTip's label narrows a
       word-wrapped text of a few lines */
    return QString("<div style=\"white-space:nowrap\"><p style=\"white-space:pre\"><b>%1</b>"
                   "</p>%2</div>")
        .arg(name, body);
}

/* --- events --- */

/* The window: hidden or minimized, nothing is read; the status bar: its
   context menu is ours */
void StatusStats::watchWindow()
{
    if (m_window) {
        m_window->removeEventFilter(this);
    }
    if (m_bar) {
        m_bar->removeEventFilter(this);
    }
    m_window = window();
    m_bar = qobject_cast<QStatusBar *>(parentWidget());
    m_window->installEventFilter(this);
    if (m_bar) {
        m_bar->installEventFilter(this);
    }
    updateSources();
}

bool StatusStats::event(QEvent *event)
{
    if (event->type() == QEvent::ParentChange) {
        watchWindow();
    } else if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        std::fill(std::begin(m_widths), std::end(m_widths), 0);
        setContentsMargins(0, 0, gap(), 0);
        refresh();
    }
    return QWidget::event(event);
}

bool StatusStats::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::ToolTip:
        for (int i = 0; i < kStats; i++) {
            if (watched == m_labels[i]) {
                m_tip = i;
                QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(),
                                   tooltip(Stat(i)), m_labels[i]);
                /* the details it shows, read from now on */
                updateSources();
                return true;
            }
        }
        break;
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
        for (const Optional &item : std::as_const(m_optional)) {
            if (watched == item.widget) {
                showOptional(item);
            }
        }
        break;
    case QEvent::ParentChange:
        if (watched == m_button && m_button->parentWidget()) {
            m_button->show();
        }
        break;
    case QEvent::ContextMenu:
        if (watched == m_bar) {
            m_menu->popup(static_cast<QContextMenuEvent *>(event)->globalPos());
            return true;
        }
        break;
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::WindowStateChange:
        if (watched == m_window) {
            updateSources();
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}
