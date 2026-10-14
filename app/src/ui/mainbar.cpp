// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainbar.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>

namespace {

/* The width a layout gives @widget at most, as QWidgetItem::sizeHint() has it */
int hintWidth(const QWidget *widget)
{
    if (widget->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored) {
        return 0;
    }
    return qMax(0, widget->sizeHint()
                       .expandedTo(widget->minimumSizeHint())
                       .boundedTo(widget->maximumSize())
                       .expandedTo(widget->minimumSize())
                       .width());
}

/* The width a layout gives @widget at least, as qSmartMinSize() has it */
int leastWidth(const QWidget *widget)
{
    const QSizePolicy::Policy policy = widget->sizePolicy().horizontalPolicy();
    int width = 0;

    if (policy != QSizePolicy::Ignored) {
        width = policy & QSizePolicy::ShrinkFlag
                    ? widget->minimumSizeHint().width()
                    : qMax(widget->sizeHint().width(), widget->minimumSizeHint().width());
    }
    width = qMin(width, widget->maximumWidth());
    if (widget->minimumWidth() > 0) {
        width = widget->minimumWidth();
    }
    return qMax(0, width);
}

}

TabSwitcher::TabSwitcher(const QList<QAction *> &tabs, QWidget *parent)
    : QWidget(parent), m_tabs(tabs), m_menuButton(new QToolButton), m_menu(new QMenu(this))
{
    auto *layout = new QHBoxLayout(this);

    setObjectName("tabs");
    layout->setContentsMargins(0, 0, 0, 0);
    /* apart as the bar's buttons are */
    layout->setSpacing(style()->pixelMetric(QStyle::PM_ToolBarItemSpacing));
    for (QAction *tab : tabs) {
        auto *button = new QToolButton;
        button->setDefaultAction(tab);
        button->setAutoRaise(true);
        /* the current one Tab reaches (updateCurrent()), the arrows go to the others */
        button->setFocusPolicy(Qt::NoFocus);
        button->installEventFilter(this);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        /* as high as the bar's buttons, which have icons */
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        layout->addWidget(button);
        m_buttons << button;
        connect(tab, &QAction::toggled, this, [this](bool on) {
            if (on) {
                updateCurrent();
            }
        });
    }
    m_menu->addActions(tabs);
    m_menuButton->setObjectName("tabMenu");
    m_menuButton->setMenu(m_menu);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setAutoRaise(true);
    /* reached with Tab, as the tabs it stands for */
    m_menuButton->setFocusPolicy(Qt::TabFocus);
    m_menuButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_menuButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_menuButton->hide();
    layout->addWidget(m_menuButton);
    updateCurrent();
    fitMenuButton();
}

void TabSwitcher::setCompact(bool compact)
{
    if (compact == m_compact) {
        return;
    }
    m_compact = compact;
    for (QToolButton *button : std::as_const(m_buttons)) {
        button->setVisible(!compact);
    }
    m_menuButton->setVisible(compact);
}

int TabSwitcher::widthFor(bool compact) const
{
    if (compact) {
        return hintWidth(m_menuButton);
    }
    int width = 0;
    for (const QToolButton *button : m_buttons) {
        width += (width > 0 ? layout()->spacing() : 0) + hintWidth(button);
    }
    return width;
}

void TabSwitcher::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        layout()->setSpacing(style()->pixelMetric(QStyle::PM_ToolBarItemSpacing));
        fitMenuButton();
    }
}

bool TabSwitcher::eventFilter(QObject *watched, QEvent *event)
{
    const qsizetype from = m_buttons.indexOf(qobject_cast<QToolButton *>(watched));

    if (event->type() == QEvent::KeyPress && from >= 0) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        int step = 0;

        if (key == Qt::Key_Left || key == Qt::Key_Right) {
            step = (key == Qt::Key_Right) != isRightToLeft() ? 1 : -1;
        }
        /* the next tab that is enabled, as QTabBar goes: up to the last one */
        for (qsizetype i = from + step; step != 0 && i >= 0 && i < m_buttons.size(); i += step) {
            if (m_tabs[i]->isEnabled()) {
                m_tabs[i]->trigger();
                m_buttons[i]->setFocus(Qt::TabFocusReason);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TabSwitcher::updateCurrent()
{
    for (int i = 0; i < m_tabs.size(); i++) {
        if (m_tabs[i]->isChecked()) {
            m_menuButton->setText(m_tabs[i]->iconText());
            m_menuButton->setToolTip(m_tabs[i]->iconText());
        }
        /* one stop of Tab for all the tabs, as a QTabBar has */
        m_buttons[i]->setFocusPolicy(m_tabs[i]->isChecked() ? Qt::TabFocus : Qt::NoFocus);
    }
}

void TabSwitcher::fitMenuButton()
{
    const QString current = m_menuButton->text();
    int width = 0;

    m_menuButton->setMinimumWidth(0);
    for (const QAction *tab : std::as_const(m_tabs)) {
        m_menuButton->setText(tab->iconText());
        width = qMax(width, m_menuButton->sizeHint().width());
    }
    m_menuButton->setText(current);
    m_menuButton->setMinimumWidth(width);
}

MainBar::MainBar(const QString &title, QWidget *parent) : QToolBar(title, parent)
{
    /* where the window has it, without a handle, which neededWidth() would miss */
    setMovable(false);
    setFloatable(false);
}

QMenuBar *MainBar::addMenuBar()
{
    auto *menuBar = new QMenuBar;

    menuBar->setObjectName("menus");
    /* the menus in the window, as VMware's: a desktop's global menu would
       take them out of it, and the bar would lose them */
    menuBar->setNativeMenuBar(false);
    /* as wide as its menus, all of them: never one in its own overflow menu */
    menuBar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    addWidget(menuBar);
    return menuBar;
}

void MainBar::addStretch()
{
    auto *stretch = new QWidget;
    stretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(stretch);
}

TabSwitcher *MainBar::addTabs(const QList<QAction *> &tabs)
{
    m_tabs = new TabSwitcher(tabs);
    addWidget(m_tabs);
    return m_tabs;
}

void MainBar::addIconButtons(const QList<QAction *> &actions)
{
    for (QAction *action : actions) {
        addAction(action);
        if (auto *button = qobject_cast<QToolButton *>(widgetForAction(action))) {
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setProperty("namedInToolTip", true);
            button->installEventFilter(this);
        }
    }
}

QString MainBar::toolTipOf(const QAction *action)
{
    /* the name as a button's text has it, without the mnemonic */
    QString tip = action->iconText();
    const QString more = action->toolTip();

    if (!action->shortcut().isEmpty()) {
        tip += QString(" (%1)").arg(action->shortcut().toString(QKeySequence::NativeText));
    }
    /* the action's own tooltip, if it has one: by default, its name */
    if (more != action->iconText()) {
        tip += '\n' + more;
    }
    return tip;
}

bool MainBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ToolTip && watched->property("namedInToolTip").toBool()) {
        auto *button = static_cast<QToolButton *>(watched);
        if (const QAction *action = button->defaultAction()) {
            QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(), toolTipOf(action),
                               button);
            return true;
        }
    }
    return QToolBar::eventFilter(watched, event);
}

int MainBar::neededWidth(bool compact, bool least) const
{
    const QMargins margins = layout()->contentsMargins();
    int width = margins.left() + margins.right();
    int count = 0;

    for (QAction *action : actions()) {
        QWidget *widget = widgetForAction(action);
        int item;

        if (!action->isVisible() || !widget) {
            continue;
        }
        if (widget == m_tabs) {
            item = m_tabs->widthFor(compact);
        } else {
            item = least ? leastWidth(widget) : hintWidth(widget);
        }
        width += (count++ > 0 ? layout()->spacing() : 0) + item;
    }
    return width;
}

void MainBar::fit()
{
    if (m_fitting) {
        return;
    }
    m_fitting = true;
    if (m_tabs) {
        const bool compact = neededWidth(false) > width();
        if (m_tabs->isCompact() != compact) {
            m_tabs->setCompact(compact);
            /* hidden buttons do not tell the layout themselves */
            layout()->invalidate();
        }
    }
    /* never narrower than with the tabs compact: the rest has no less to show */
    const int least = neededWidth(true, true);
    if (minimumWidth() != least) {
        setMinimumWidth(least);
    }
    m_fitting = false;
}

bool MainBar::event(QEvent *event)
{
    const bool done = QToolBar::event(event);

    switch (event->type()) {
    case QEvent::LayoutRequest:
    case QEvent::Show:
    case QEvent::StyleChange:
    case QEvent::FontChange:
        /* something changed size: the text of an action, a tab's name */
        fit();
        break;
    default:
        break;
    }
    return done;
}

void MainBar::resizeEvent(QResizeEvent *event)
{
    QToolBar::resizeEvent(event);
    fit();
}
