// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/guarded.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "core/vmstats.h"
#include "ui/statstext.h"

class QTimer;
class Vm;
class VmConsole;

/*
 * The statistics of the VM shown, as the Qt app's status bar has them
 * (StatusStats): a button in the VM page's header bar while the VM runs,
 * its popover listing CPU, memory, disk, network, GPU, the frames and the
 * main loop's wait, each with its value ("CPU 42 %") or why there is none
 * ("needs the guest tools"); a row opens its details in the popover, the
 * Qt tooltips' tables, and a back button.  GNOME has no status bar: a
 * popover is a surface of its own over the VM's screen.
 *
 * Read once a second, only while the popover shows: the details' extra
 * numbers (KVM's counters, PSS and the guest's own memory) while they show.
 * The frames are the screen's own while it shows in vitrine's window,
 * else QEMU's (its SDL display's).
 */
class StatsButton : public Gtk4::Guarded
{
public:
    /* @vm: the VM shown, @console: its console, if made; asked each time */
    StatsButton(std::function<Vm *()> vm, std::function<VmConsole *()> console);
    ~StatsButton();
    StatsButton(const StatsButton &) = delete;
    StatsButton &operator=(const StatsButton &) = delete;

    /* For the VM page's header bar */
    GtkWidget *widget() const { return m_button; }
    /* The VM shown, its state or its console changed */
    void update();

    /* The popover shown or hidden, a stat's details (-1: the list) */
    void showPopover(bool shown);
    void showDetails(int stat);
    /* What the sampler reads now, what a row says (tests) */
    VmStats::Sampler::Sources sources() const;
    QString value(StatsText::Stat stat) const;
    QString note(StatsText::Stat stat) const;
    /* The details shown, their text (tests) */
    QString detailsText() const;

    /* A part of the details: a line of rich text, or a table's rows (label, value, note) */
    struct Block {
        QString markup;
        QList<QStringList> rows;
        bool isTable() const { return !rows.isEmpty(); }
    };
    /* StatsText::details()' rich text, as such parts */
    static QList<Block> blocks(const QString &html);

private:
    void updateSources();
    void refresh();
    /* The sampler's, with the frames from the screen while it shows */
    VmStats::Snapshot snapshot() const;
    void fillDetails(const VmStats::Snapshot &s);

    std::function<Vm *()> m_vmOf;
    std::function<VmConsole *()> m_consoleOf;
    QObject m_qt;
    VmStats::Sampler *m_sampler;
    QTimer *m_refresh;
    QPointer<Vm> m_vm;
    /* the stat whose details show, -1 for the list */
    int m_details = -1;
    QString m_detailsHtml;

    GtkWidget *m_button;
    GtkWidget *m_popover;
    GtkWidget *m_stack;
    GtkWidget *m_title;
    GtkWidget *m_rows[StatsText::kStats];
    GtkWidget *m_values[StatsText::kStats];
    GtkWidget *m_detailsTitle;
    GtkWidget *m_detailsBox;
};
