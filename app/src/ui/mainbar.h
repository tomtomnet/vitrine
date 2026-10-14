// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QToolBar>

class QMenu;
class QMenuBar;
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
    /*
     * @menu under @tab's button, which then has an arrow beside its name
     * (a click on the name still chooses the tab); none: a plain button
     */
    void setTabMenu(QAction *tab, QMenu *menu);

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
 * The one bar of the main window, under its title, as VMware's: the
 * window's menus at the start (after the list's button), the buttons that
 * start and stop the selected VM next to them, the tabs of its pane at the
 * other end.
 *
 * Short of room, the tabs become one button that lists them.  Narrower
 * than that, the bar holds the window's width: nothing goes into the
 * toolbar's overflow menu, nor a menu into the menu bar's own.
 */
class MainBar : public QToolBar
{
    Q_OBJECT

public:
    explicit MainBar(const QString &title, QWidget *parent = nullptr);

    /*
     * The window's menus, for the menus to be added to: in the bar, all of
     * them always shown, also with a global menu on the desktop, which
     * would take them out of the window.  Alt and a menu's letter open it.
     */
    QMenuBar *addMenuBar();
    /* Room between the parts added before and after: the latter at the end */
    void addStretch();
    /* The tabs of a pane, as VmPane::tabActions() gives them */
    TabSwitcher *addTabs(const QList<QAction *> &tabs);
    /*
     * The width the bar needs, its tabs @compact or not, as its layout lays
     * it out: its items' preferred widths, or the least they can take (@least)
     */
    int neededWidth(bool compact, bool least = false) const;

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    /* The tabs compact only if the bar cannot show them all in its width */
    void fit();

    TabSwitcher *m_tabs = nullptr;
    bool m_fitting = false;
};
