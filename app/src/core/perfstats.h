// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

/*
 * How smoothly a running VM goes, for the status bar (VmStats::Sampler
 * reads them):
 * - its display, from QEMU (x-query-display-stats, qemu-gui's SDL display):
 *   frames on screen, how long they take to get there, input latency;
 * - QEMU's threads on the host, from /proc/PID/task/TID/schedstat: the CPU
 *   they use and how long they wait to run.  The main loop draws the frames
 *   and runs the emulated devices, so a starved main loop stutters;
 * - KVM's counters, from query-stats: VM exits, halts and halt polling.
 */
namespace PerfStats {

/* A duration's distribution over the time a statistic covers, in ms */
struct Latency {
    int samples = 0;
    double median = 0;
    double p99 = 0;
    double max = 0;
};

/* One console of x-query-display-stats */
struct Screen {
    int console = 0;
    int width = 0;
    int height = 0;
    double refreshHz = 0;
    QString method;             // none, swap (no compositor feedback), presentation
    double flushes = 0;         // per second
    double presented = 0;
    double dropped = 0;
    double direct = 0;
    double redraws = 0;
    Latency frame;              // guest flush -> on screen (or swap)
    Latency qemu;               // guest flush -> swap: QEMU's share
    Latency interval;           // between frames on screen
    Latency input;              // key press or click -> the guest's answer on screen
    Latency inputWait;          // before QEMU read the event
    bool hasInputWait = false;
};

/* The console to show: the one with the most frames, else the first */
bool parseDisplay(const QJsonObject &reply, Screen *display, QString *type = nullptr);

/* The scheduler's times of a thread, from schedstat */
struct Thread {
    qint64 runNs = 0;           // on a CPU
    qint64 waitNs = 0;          // runnable, waiting for a CPU
    qint64 slices = 0;          // times it ran
};
/* The threads of process @pid, by thread id */
QHash<qint64, Thread> readThreads(qint64 pid, const QString &proc = "/proc");

/* Use of a thread or a group of threads between two reads */
struct Load {
    int threads = 0;
    double cpu = 0;             // % of one CPU, summed over the threads
    double wait = 0;            // % of the time a thread waited to run, on average
    double waitPerRunMs = 0;    // average wait before each run
};
Load load(const QHash<qint64, Thread> &before, const QHash<qint64, Thread> &after,
          const QList<qint64> &tids, qint64 elapsedNs);

/* The sum over the vCPUs of each KVM counter of a query-stats reply */
QHash<QString, double> sumKvmStats(const QJsonArray &reply);

/* What the status bar shows */
struct Snapshot {
    bool display = false;       // the display measures its frames
    QString displayType;        // sdl, gtk, ...: the display in use
    Screen screen;
    bool threads = false;
    Load mainLoop;
    Load vcpus;
    Load others;
    bool kvm = false;
    double exits = 0;           // per second, all vCPUs
    double haltExits = 0;
    double deviceExits = 0;     // I/O and MMIO: emulated devices
    double haltPollSuccess = -1;    // share of halt polls that ended the halt; -1 if none
    double haltPollCpu = 0;     // % of one CPU spent polling
};

/* "240 fps · frame 6.1 ms · input 11 ms · main loop wait 0.4 %" */
QString summary(const Snapshot &s);
/* Its parts: "240 fps · frame 6.1 ms · input 11 ms", or "display idle" */
QString displaySummary(const Snapshot &s);
/* "main loop wait 0.4 %" */
QString mainLoopSummary(const Snapshot &s);
/* The details, as rich text */
QString details(const Snapshot &s);
/* Those of QEMU's threads and KVM: details() without the display */
QString hostDetails(const Snapshot &s);

/* Milliseconds, to two significant digits or so: "0.35 ms", "6.1 ms", "21 ms" */
QString formatMs(double ms);
/* A percentage in tooltips: "< 0.1 %", "0.4 %", "12 %" */
QString formatPercent(double p);

}
