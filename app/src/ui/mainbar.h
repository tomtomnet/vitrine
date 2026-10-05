// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QComboBox>
#include <QList>
#include <QToolBar>

class QMenu;
class QToolButton;

/*
 * The tabs of a pane in a bar: a button for each, the current one shown
 * pressed; or, short of room (compact), one button with the current
 * tab's name, which lists them all.
 */
class TabSwitcher : public QWidget
{
public:
    explicit TabSwitcher(const QList<QAction *> &tabs, QWidget *parent = nullptr);

    bool isCompact() const { return m_compact; }
    void setCompact(bool compact);
    /* As wide as its layout makes it, compact or not */
    int widthFor(bool compact) const;

protected:
    void changeEvent(QEvent *event) override;
    /* The arrows from one tab's button to the next, as on a QTabBar */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateCurrent();
    /* The compact button, as wide with any tab's name */
    void fitMenuButton();

    QList<QAction *> m_tabs;
    QList<QToolButton *> m_buttons;
    QToolButton *m_menuButton;
    QMenu *m_menu;
    bool m_compact = false;
};

/*
 * A drop-down of the VMs, for a bar, where their list was while it is
 * hidden: the selected one's icon and name, elided when it is long.  The
 * wheel does not go through the VMs, as it would over a bar of buttons.
 */
class VmChooser : public QComboBox
{
public:
    explicit VmChooser(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    /* The role of the line its list draws under each name, for its width */
    void setStateRole(int role) { m_stateRole = role; }
    /* As wide as the names, in bold there, and the lines under them, if there is room */
    void showPopup() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    int m_stateRole = -1;
};

/*
 * The one bar of the main window, under its title: the actions on the
 * selected VM, the tabs of its pane at the other end, and the window's
 * menus behind a button there (KDE's hamburger menu) while the menu bar
 * is hidden.
 *
 * Short of room, the actions lose their text one by one, in the order
 * setTextOrder() gives, then the tabs become one button that lists them.
 * Narrower than that, the bar holds the window's width: nothing goes into
 * the toolbar's overflow menu, where the menus and the tabs would be the
 * first to go.
 */
class MainBar : public QToolBar
{
    Q_OBJECT

public:
    explicit MainBar(const QString &title, QWidget *parent = nullptr);

    /* Room between the parts added before and after: the latter at the end */
    void addStretch();
    /* The tabs of a pane, as VmPane::tabActions() gives them */
    TabSwitcher *addTabs(const QList<QAction *> &tabs);
    /*
     * A button showing @menu under it, the theme's menu icon alone: its
     * action, which hides the button while the menus are elsewhere
     */
    QAction *addMenuButton(QMenu *menu, const QString &text);
    /* @action's button an icon alone, whatever the room */
    void setIconOnly(QAction *action);
    /* The actions that lose their text when room is short, the first first */
    void setTextOrder(const QList<QAction *> &actions);

    /* How many of the steps that make room are taken now: the actions of
       setTextOrder() without text, one by one, then the tabs compact */
    int steps() const { return m_steps; }
    int stepCount() const;
    /*
     * The width the bar needs with @steps taken, as its layout lays it
     * out: its items' preferred widths, or the least they can take (@least)
     */
    int neededWidth(int steps, bool least = false) const;

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    /* The fewest steps that let the bar show all it has in its width */
    void fit();
    void apply(int steps);
    /* @action's button with its text or without, the style of the bar else */
    int buttonWidth(QAction *action, bool text) const;

    QList<QAction *> m_textOrder;
    TabSwitcher *m_tabs = nullptr;
    /* A button out of the layout, never shown, to measure the others */
    QToolButton *m_probe;
    /* The probe's, for the buttons that open a menu: the arrow takes room */
    QMenu *m_probeMenu;
    int m_steps = 0;
    bool m_fitting = false;
};
