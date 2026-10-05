// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainbar.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QToolButton>
#include <QWheelEvent>

#include "ui/icons.h"

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

/* Three bars, for a theme without a menu icon, drawn at the ratio asked */
class MenuIconEngine : public QIconEngine
{
public:
    QIconEngine *clone() const override { return new MenuIconEngine; }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal ratio = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, ratio));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State,
                         qreal scale) override
    {
        const QSize device = (QSizeF(size) * scale).toSize();
        QPixmap pixmap(device);
        const QPalette palette = QApplication::palette();
        /* whole device pixels: crisp bars at any ratio */
        const int bar = qMax(1, device.height() / 8);
        const int gap = (device.height() - 3 * bar) / 4;
        const int top = (device.height() - 3 * bar - 2 * gap) / 2;
        const int side = device.width() / 8;

        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        for (int i = 0; i < 3; i++) {
            p.fillRect(side, top + i * (bar + gap), device.width() - 2 * side, bar,
                       palette.color(mode == QIcon::Disabled ? QPalette::Disabled
                                                             : QPalette::Normal,
                                     QPalette::ButtonText));
        }
        p.end();
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }
};

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
        button->setFocusPolicy(Qt::NoFocus);
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
    m_menuButton->setFocusPolicy(Qt::NoFocus);
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

void TabSwitcher::updateCurrent()
{
    for (const QAction *tab : std::as_const(m_tabs)) {
        if (tab->isChecked()) {
            m_menuButton->setText(tab->iconText());
            m_menuButton->setToolTip(tab->iconText());
        }
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

VmChooser::VmChooser(QWidget *parent) : QComboBox(parent)
{
    setObjectName("vmChooser");
    setSizeAdjustPolicy(QComboBox::AdjustToContents);
    /* reached with Tab, not taken by a click: as the bar's buttons */
    setFocusPolicy(Qt::TabFocus);
}

/* As wide as the longest name, up to a long one */
QSize VmChooser::sizeHint() const
{
    QSize size = QComboBox::sizeHint();

    size.setWidth(qMin(size.width(),
                       minimumSizeHint().width() + 14 * fontMetrics().averageCharWidth()));
    return size;
}

/* The icon and a few letters */
QSize VmChooser::minimumSizeHint() const
{
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const QSize content(iconSize().width() + 4 + 6 * fontMetrics().averageCharWidth(),
                        qMax(iconSize().height(), fontMetrics().height()));
    return style()->sizeFromContents(QStyle::CT_ComboBox, &option, content, this);
}

/* As QComboBox paints itself, the name elided to the room it has */
void VmChooser::paintEvent(QPaintEvent *)
{
    QStylePainter painter(this);
    QStyleOptionComboBox option;

    painter.setPen(palette().color(QPalette::Text));
    initStyleOption(&option);
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    const QRect field =
        style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
    /* where the style puts the text: after the icon and its gap, inside a pixel each side */
    const int room = field.width() - 2 -
                     (option.currentIcon.isNull() ? 0 : option.iconSize.width() + 4);
    option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight, room);
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}

void VmChooser::wheelEvent(QWheelEvent *event)
{
    event->ignore();
}

MainBar::MainBar(const QString &title, QWidget *parent)
    : QToolBar(title, parent), m_probe(new QToolButton(this))
{
    /* as the buttons the bar makes for its actions, but out of its layout */
    m_probe->hide();
    m_probe->setAutoRaise(true);
    m_probe->setFocusPolicy(Qt::NoFocus);
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

QAction *MainBar::addMenuButton(QMenu *menu, const QString &text)
{
    QAction *action = menu->menuAction();
    QIcon icon(new MenuIconEngine);

    /* KDE's, GNOME's */
    for (const char *name : {"application-menu", "open-menu-symbolic", "open-menu"}) {
        if (QIcon::hasThemeIcon(name)) {
            icon = QIcon::fromTheme(name);
            break;
        }
    }
    action->setIcon(icon);
    action->setText(text);
    addAction(action);
    if (auto *button = qobject_cast<QToolButton *>(widgetForAction(action))) {
        button->setObjectName("menuButton");
        /* the whole button opens it, as KDE's hamburger menu does */
        button->setPopupMode(QToolButton::InstantPopup);
    }
    setIconOnly(action);
    return action;
}

void MainBar::setIconOnly(QAction *action)
{
    if (auto *button = qobject_cast<QToolButton *>(widgetForAction(action))) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
}

void MainBar::setTextOrder(const QList<QAction *> &actions)
{
    m_textOrder = actions;
    fit();
}

int MainBar::stepCount() const
{
    return int(m_textOrder.size()) + (m_tabs ? 1 : 0);
}

int MainBar::buttonWidth(QAction *action, bool text) const
{
    m_probe->setIconSize(iconSize());
    m_probe->setToolButtonStyle(text ? toolButtonStyle() : Qt::ToolButtonIconOnly);
    /* as QToolButton shows an action, without the mnemonic */
    m_probe->setText(action->iconText().replace('&', "&&"));
    m_probe->setIcon(action->icon());
    return hintWidth(m_probe);
}

int MainBar::neededWidth(int steps, bool least) const
{
    const QMargins margins = layout()->contentsMargins();
    int width = margins.left() + margins.right();
    int count = 0;

    for (QAction *action : actions()) {
        QWidget *widget = widgetForAction(action);
        const qsizetype order = m_textOrder.indexOf(action);
        int item;

        if (!action->isVisible() || !widget) {
            continue;
        }
        if (order >= 0) {
            item = buttonWidth(action, order >= steps);
        } else if (widget == m_tabs) {
            item = m_tabs->widthFor(steps > m_textOrder.size());
        } else {
            item = least ? leastWidth(widget) : hintWidth(widget);
        }
        width += (count++ > 0 ? layout()->spacing() : 0) + item;
    }
    return width;
}

void MainBar::apply(int steps)
{
    bool changed = false;

    for (int i = 0; i < m_textOrder.size(); i++) {
        auto *button = qobject_cast<QToolButton *>(widgetForAction(m_textOrder[i]));
        const Qt::ToolButtonStyle style = i < steps ? Qt::ToolButtonIconOnly : toolButtonStyle();
        if (button && button->toolButtonStyle() != style) {
            button->setToolButtonStyle(style);
            changed = true;
        }
    }
    if (m_tabs && m_tabs->isCompact() != (steps > m_textOrder.size())) {
        m_tabs->setCompact(steps > m_textOrder.size());
        changed = true;
    }
    m_steps = steps;
    /* hidden buttons do not tell the layout themselves */
    if (changed) {
        layout()->invalidate();
    }
}

void MainBar::fit()
{
    if (m_fitting) {
        return;
    }
    m_fitting = true;

    const int last = stepCount();
    int steps = 0;
    while (steps < last && neededWidth(steps) > width()) {
        steps++;
    }
    apply(steps);
    /* never narrower than with every step taken, the VMs' names elided */
    const int least = neededWidth(last, true);
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
        /* something changed size: the text of an action, the VMs' drop-down */
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
