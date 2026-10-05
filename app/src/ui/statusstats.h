// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

#include "core/vmstats.h"

class QAction;
class QLabel;
class QMenu;
class QToolButton;
class Vm;
class VmConsole;

/*
 * The statistics of the selected VM in the status bar, as VirtualBox has
 * them: a short label and a value each ("CPU 42 %", "RAM 3.1 GiB"), the
 * details in their tooltips.  The menu of the small button at the end of
 * the status bar, or of a right click on it, shows or hides each of them
 * and a few other items of the status bar; the choice is remembered.
 * Only what shows is read, and nothing while the window is hidden.
 *
 * The statistics are as wide as the window lets them be: the last ones
 * are left out of a window too narrow for them, rather than keeping it
 * wide.  Each keeps the widest it was since its VM was selected, so that
 * what follows does not move with each new value.
 */
class StatusStats : public QWidget
{
    Q_OBJECT

public:
    enum Stat { Cpu, Memory, Disk, Network, Gpu, Frames, MainLoop };
    static constexpr int kStats = MainLoop + 1;

    explicit StatusStats(QWidget *parent = nullptr);

    /* The VM to follow, or none, and its console, whose screen measures
       its frames while it shows */
    void setVm(Vm *vm, VmConsole *console = nullptr);
    /* The button that opens the menu, for the end of the status bar */
    QToolButton *menuButton() const { return m_button; }
    QMenu *menu() const { return m_menu; }
    /*
     * @widget, an item of the status bar that shows while it has something
     * to say, made one the menu shows or hides as @text (@key in the
     * settings): the widget to add to the status bar in its place
     */
    QWidget *optional(const QString &key, const QString &text, QWidget *widget);

    bool isShown(Stat stat) const;
    void setShown(Stat stat, bool shown);
    QAction *action(Stat stat) const { return m_actions[stat]; }
    QLabel *label(Stat stat) const { return m_labels[stat]; }
    /* What the sampler reads now, for tests */
    VmStats::Sampler::Sources sources() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Optional {
        QAction *action;
        QWidget *widget;
        QWidget *holder;
    };

    void watchWindow();
    void refresh();
    void updateSources();
    void relayout();
    void updateMenu();
    void showOptional(const Optional &item);
    /* The sampler's, with the frames from the screen while it shows */
    VmStats::Snapshot snapshot() const;
    QString text(Stat stat, const VmStats::Snapshot &s) const;
    QString tooltip(Stat stat) const;
    /* The statistics with something to show, in their order */
    QList<int> candidates() const;
    int gap() const;

    VmStats::Sampler *m_sampler;
    QPointer<Vm> m_vm;
    QPointer<VmConsole> m_console;
    QLabel *m_labels[kStats];
    QAction *m_actions[kStats];
    QString m_names[kStats];        // the actions' text, without a note
    int m_widths[kStats] = {};      // the widest each was for this VM
    QSize m_hint;                   // the size hint the status bar has
    QToolButton *m_button;
    QMenu *m_menu;
    QList<Optional> m_optional;
    int m_tip = -1;                 // the statistic whose tooltip shows
    QPointer<QWidget> m_window;     // watched: hidden or minimized, nothing is read
    QPointer<QWidget> m_bar;        // watched: its context menu is ours
};
