// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QString>

#include "core/vmstats.h"
#include "vmview/stats.h"

class Vm;

/*
 * The words of a running VM's statistics, for both apps: each one's name,
 * what it shows in a few characters ("CPU 42 %"), why the VM shows none
 * ("needs the guest tools"), and its details, rich text of tables (the Qt
 * app's status bar and its tooltips, StatusStats; the GTK app's popover).
 * Qt Core only.
 */
class StatsText
{
    /* the Qt app's StatusStats' words, in its translation context */
    Q_DECLARE_TR_FUNCTIONS(StatusStats)

public:
    enum Stat { Cpu, Memory, Disk, Network, Gpu, Frames, MainLoop };
    static constexpr int kStats = MainLoop + 1;

    /* "CPU", "Frame Rate and Latency" */
    static QString name(Stat stat);
    /* What it shows, a sentence */
    static QString tip(Stat stat);
    /* "CPU 42 %", "RAM 3.1 GiB": nothing while it has nothing to show */
    static QString label(Stat stat, const VmStats::Snapshot &s);
    /* The number alone, for a row that names it: "42 %", "3.1 GiB"; nothing while it has none */
    static QString value(Stat stat, const VmStats::Snapshot &s);
    /*
     * Why @vm, running, has none of @stat, or less: "needs the guest
     * tools", "QEMU does not use it"; empty otherwise.  @guest: where the
     * sampler's guest counters come from.
     */
    static QString note(Stat stat, const Vm *vm, VmStats::GuestSource guest,
                        const VmStats::Snapshot &s);

    /* What the details need besides the snapshot */
    struct Context {
        /* the guest's RAM, from the run's arguments; 0 if not known */
        qint64 ramMiB = 0;
        VmStats::GuestSource guest = VmStats::GuestSource::None;
        /* the guest tools are for the VM's guest (toolsAreFor()) */
        bool toolsFor = true;
        /* the screen in vitrine's window, its last second: its frames, not QEMU's display's */
        const Stats::Summary *screen = nullptr;
    };
    /* The details of @stat: rich text, titles and tables */
    static QString details(Stat stat, const VmStats::Snapshot &s, const Context &context);

    /* The guest tools are for @vm's guest: its system is one they are for,
       or not known yet (GuestTools::supports()) */
    static bool toolsAreFor(const Vm *vm);
    /*
     * The frames of the screen in vitrine's window in @s, over QEMU's
     * numbers (its SDL display's): @v, its last second, the guest's
     * @width and @height, the monitor's @refreshHz (0 if not known)
     */
    static void mergeScreen(VmStats::Snapshot *s, const Stats::Summary &v, int width, int height,
                            double refreshHz);
};
