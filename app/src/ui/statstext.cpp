// SPDX-License-Identifier: GPL-2.0-or-later
#include "statstext.h"

#include <QStringList>

#include "core/guesttools.h"
#include "core/perfstats.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

QString StatsText::name(Stat stat)
{
    switch (stat) {
    case Cpu:
        return tr("CPU");
    case Memory:
        return tr("Memory");
    case Disk:
        return tr("Disk");
    case Network:
        return tr("Network");
    case Gpu:
        return tr("GPU");
    case Frames:
        return tr("Frame Rate and Latency");
    case MainLoop:
        return tr("Main Loop Wait");
    }
    return {};
}

QString StatsText::tip(Stat stat)
{
    switch (stat) {
    case Cpu:
        return tr("How busy the guest keeps its vCPUs; the rest of QEMU in the details");
    case Memory:
        return tr("The host's memory QEMU holds: the guest's RAM it touched, and its own");
    case Disk:
        return tr("What the guest reads from its disks and writes to them");
    case Network:
        return tr("What the guest receives and sends, by its own counters: needs the guest tools");
    case Gpu:
        return tr("How busy QEMU keeps the host's GPU, and the GPU memory it holds");
    case Frames:
        return tr("The frames the screen shows each second, their latency and the input's");
    case MainLoop:
        return tr("How long QEMU's main loop waits for a CPU: a starved main loop stutters");
    }
    return {};
}

QString StatsText::label(Stat stat, const VmStats::Snapshot &s)
{
    using namespace VmStats;

    switch (stat) {
    case Cpu:
        /* the guest's view: 100 % when all its vCPUs run all the time */
        if (!s.perf.threads || !s.perf.vcpus.threads) {
            return {};
        }
        return tr("CPU %1").arg(formatWholePercent(s.perf.vcpus.cpu / s.perf.vcpus.threads));
    case Memory:
        return s.memory ? tr("RAM %1").arg(formatBytes(s.qemuMemory.resident)) : QString();
    case Disk:
        return s.disk ? tr("Disk R %1 · W %2")
                            .arg(formatRate(s.diskUse.read), formatRate(s.diskUse.written))
                      : QString();
    case Network:
        return s.network ? tr("Net ↓ %1 · ↑ %2").arg(formatRate(s.net.rx), formatRate(s.net.tx))
                         : QString();
    case Gpu:
        if (!s.gpu || s.gpus.isEmpty()) {
            return {};
        }
        return tr("GPU %1 · %2")
            .arg(formatWholePercent(s.gpus.first().busy), formatBytes(s.gpus.first().memory));
    case Frames:
        return PerfStats::displaySummary(s.perf);
    case MainLoop:
        return PerfStats::mainLoopSummary(s.perf);
    }
    return {};
}

/* The guest tools are for this VM's guest: its system is one they are for,
   or not known yet (GuestTools::supports()) */
QString StatsText::value(Stat stat, const VmStats::Snapshot &s)
{
    using namespace VmStats;

    switch (stat) {
    case Cpu:
        if (!s.perf.threads || !s.perf.vcpus.threads) {
            return {};
        }
        return formatWholePercent(s.perf.vcpus.cpu / s.perf.vcpus.threads);
    case Memory:
        return s.memory ? formatBytes(s.qemuMemory.resident) : QString();
    case Disk:
        return s.disk ? tr("R %1 · W %2").arg(formatRate(s.diskUse.read), formatRate(s.diskUse.written))
                      : QString();
    case Network:
        return s.network ? tr("↓ %1 · ↑ %2").arg(formatRate(s.net.rx), formatRate(s.net.tx))
                         : QString();
    case Gpu:
        if (!s.gpu || s.gpus.isEmpty()) {
            return {};
        }
        return tr("%1 · %2").arg(formatWholePercent(s.gpus.first().busy),
                                 formatBytes(s.gpus.first().memory));
    case Frames:
        return PerfStats::displaySummary(s.perf);
    case MainLoop:
        return s.perf.threads ? PerfStats::formatPercent(s.perf.mainLoop.wait) : QString();
    }
    return {};
}

bool StatsText::toolsAreFor(const Vm *vm)
{
    const GuestTools::System system = GuestTools::system(vm->id());
    return !system.isKnown() ||
           GuestTools::supports(system, GuestTools::medium(GuestTools::releaseFor(system)));
}

QString StatsText::note(Stat stat, const Vm *vm, VmStats::GuestSource guest,
                        const VmStats::Snapshot &s)
{
    if (!vm || !vm->runner()->isActive()) {
        return {};
    }
    switch (stat) {
    case Network:
        switch (guest) {
        case VmStats::GuestSource::NoAgent:
            return toolsAreFor(vm) ? tr("needs the guest tools") : tr("not reported by this guest");
        case VmStats::GuestSource::OldAgent:
            return tr("needs newer guest tools");
        default:
            break;
        }
        break;
    case Gpu:
        if (!s.gpuError.isEmpty()) {
            return tr("cannot be read");
        }
        if (s.drmClients == 0) {
            return tr("QEMU does not use it");
        }
        break;
    default:
        break;
    }
    return {};
}

static PerfStats::Latency latency(int samples, double median, double p99)
{
    PerfStats::Latency l;

    l.samples = samples;
    l.median = median;
    l.p99 = p99;
    l.max = p99;
    return l;
}

void StatsText::mergeScreen(VmStats::Snapshot *snapshot, const Stats::Summary &v, int width,
                            int height, double refreshHz)
{
    VmStats::Snapshot &s = *snapshot;
    /* the screen counts what it puts on the screen; QEMU's numbers are the SDL display's */
    const double seconds = v.seconds > 0 ? v.seconds : 1;
    /* without presentation feedback (X11), frames end at the swap */
    const bool presented = v.presented > 0;
    PerfStats::Screen &d = s.perf.screen;

    d = PerfStats::Screen();
    d.width = width;
    d.height = height;
    if (refreshHz > 0) {
        d.refreshHz = refreshHz;
    }
    d.method = presented || !v.drawn ? "presentation" : "swap";
    d.flushes = v.updates / seconds;
    d.presented = (presented ? v.presented : v.drawn) / seconds;
    d.dropped = v.discarded / seconds;
    d.frame = presented ? latency(v.presented, v.presentMsP50, v.presentMsP99)
                        : latency(v.drawn, v.drawMsP50, v.drawMsP99);
    if (presented) {
        d.interval = latency(v.presented, v.intervalMsP50, v.intervalMsP99);
    }
    s.perf.display = v.seconds > 0;
    s.perf.displayType = "dbus";
}

using PerfStats::tableRow;

/* % of one CPU, as host CPUs: "1.7 CPUs" */
static QString cpus(double percent)
{
    return StatsText::tr("%1 CPUs").arg(percent / 100, 0, 'f', percent < 995 ? 2 : 1);
}

static QString cpuDetails(const VmStats::Snapshot &s)
{
    const PerfStats::Snapshot &p = s.perf;
    const double besides = p.mainLoop.cpu + p.others.cpu;
    const double total = p.vcpus.cpu + besides;
    QString html = StatsText::tr("<b>CPU</b>, over the last second") + "<table>";

    if (p.vcpus.threads) {
        html += tableRow(StatsText::tr("Guest"),
                         PerfStats::formatPercent(p.vcpus.cpu / p.vcpus.threads),
                         StatsText::tr("of its %n vCPU(s): %1 of the host's", nullptr,
                                         p.vcpus.threads)
                             .arg(cpus(p.vcpus.cpu)));
    }
    html += tableRow(StatsText::tr("QEMU besides"), cpus(besides),
                     StatsText::tr("main loop %1, %n other thread(s) %2", nullptr,
                                     p.others.threads)
                         .arg(PerfStats::formatPercent(p.mainLoop.cpu),
                              PerfStats::formatPercent(p.others.cpu)));
    if (s.hostCpus > 0) {
        html += tableRow(StatsText::tr("In all"), cpus(total),
                         StatsText::tr("%1 of the host's %n CPU(s)", nullptr, s.hostCpus)
                             .arg(PerfStats::formatPercent(total / s.hostCpus)));
    }
    html += "</table>";
    if (!s.busiest.isEmpty()) {
        html += StatsText::tr("<b>Busiest threads</b>") + "<table>";
        for (const VmStats::ThreadUse &t : s.busiest) {
            html += tableRow(t.name.isEmpty() ? StatsText::tr("thread %1").arg(t.tid) : t.name,
                             PerfStats::formatPercent(t.cpu));
        }
        html += "</table>";
    }
    /* KVM's counters, read while this shows */
    PerfStats::Snapshot kvm = p;
    kvm.threads = false;
    return html + PerfStats::hostDetails(kvm);
}

static QString memoryDetails(const VmStats::Snapshot &s, qint64 ramMiB,
                             VmStats::GuestSource guest, bool toolsFor)
{
    using VmStats::formatBytes;
    const VmStats::Memory &m = s.qemuMemory;
    QString html = StatsText::tr("<b>Memory</b> of the host that QEMU holds") + "<table>";

    html += tableRow(StatsText::tr("Resident"), formatBytes(m.resident),
                     ramMiB > 0 ? StatsText::tr("the guest has %1 of RAM")
                                      .arg(formatBytes(ramMiB * 1024 * 1024))
                                : QString());
    if (m.shmem > 0) {
        html += tableRow(StatsText::tr("Shared"), formatBytes(m.shmem),
                         StatsText::tr("the guest's RAM, as far as it has used it"));
    }
    html += tableRow(StatsText::tr("Private"), formatBytes(m.anon),
                     m.shmem > 0 ? StatsText::tr("QEMU's own: its heap and buffers")
                                 : StatsText::tr("the guest's RAM and QEMU's own"));
    html += tableRow(StatsText::tr("Files"), formatBytes(m.file),
                     StatsText::tr("QEMU's program and libraries"));
    if (m.swap > 0) {
        html += tableRow(StatsText::tr("Swapped out"), formatBytes(m.swap));
    }
    if (m.proportional >= 0) {
        html += tableRow(StatsText::tr("Proportional"), formatBytes(m.proportional),
                         StatsText::tr("PSS: pages shared with passt or virtiofsd "
                                         "counted in part"));
    }
    html += "</table>";
    if (s.guestMemory) {
        html += StatsText::tr("<b>The guest</b>, by the guest tools") + "<table>" +
                tableRow(StatsText::tr("In use"), formatBytes(s.guestTotal - s.guestAvailable),
                         StatsText::tr("of %1, its caches left out")
                             .arg(formatBytes(s.guestTotal))) +
                "</table>";
    } else if (guest == VmStats::GuestSource::NoAgent && toolsFor) {
        html += StatsText::tr("<i>What the guest itself uses needs the guest tools.</i>");
    } else if (guest == VmStats::GuestSource::OldAgent) {
        html += StatsText::tr("<i>What the guest itself uses needs newer guest tools.</i>");
    }
    return html;
}

static QString diskDetails(const VmStats::Snapshot &s)
{
    using VmStats::formatBytes;
    using VmStats::formatRate;
    QString html = StatsText::tr("<b>Disks</b>, over the last second; in all since QEMU "
                                   "started") +
                   "<table>";

    for (const VmStats::DeviceRate &d : s.diskUse.devices) {
        /* not the empty drive the firmware and the guest looked at once */
        if (d.read == 0 && d.written == 0 && d.readTotal + d.writtenTotal < (1 << 20)) {
            continue;
        }
        html += tableRow(d.name,
                         StatsText::tr("R %1 · W %2")
                             .arg(formatRate(d.read), formatRate(d.written)),
                         StatsText::tr("in all %1 read, %2 written")
                             .arg(formatBytes(d.readTotal), formatBytes(d.writtenTotal)));
    }
    return html + "</table>";
}

static QString networkDetails(const VmStats::Snapshot &s)
{
    using VmStats::formatBytes;
    using VmStats::formatRate;
    QString html = StatsText::tr("<b>Network</b>, by the guest's own counters (guest tools); "
                                   "in all since the guest started") +
                   "<table>";

    for (const VmStats::InterfaceRate &i : s.net.interfaces) {
        html += tableRow(i.name,
                         StatsText::tr("↓ %1 · ↑ %2").arg(formatRate(i.rx), formatRate(i.tx)),
                         StatsText::tr("in all %1 received, %2 sent")
                             .arg(formatBytes(i.rxTotal), formatBytes(i.txTotal)));
    }
    if (s.net.interfaces.isEmpty()) {
        html += tableRow(StatsText::tr("No network card"), QString());
    }
    return html + "</table>";
}

/* The drivers' names of their memory regions, said plainly */
static QString regionNote(const QString &region)
{
    if (region.startsWith("vram") || region.startsWith("local")) {
        return StatsText::tr("the GPU's own memory");
    }
    if (region == "gtt") {
        return StatsText::tr("host memory the GPU maps");
    }
    if (region == "cpu" || region.startsWith("system")) {
        return StatsText::tr("host memory");
    }
    return {};
}

static QString gpuDetails(const VmStats::Snapshot &s)
{
    QString html;

    for (const VmStats::GpuUse &g : s.gpus) {
        html += StatsText::tr("<b>GPU</b> %1 (%2), over the last second")
                    .arg(g.pdev.toHtmlEscaped(), g.driver.toHtmlEscaped()) +
                "<table>";
        for (const VmStats::EngineUse &e : g.engines) {
            html += tableRow(e.name, PerfStats::formatPercent(e.busy));
        }
        for (const VmStats::RegionUse &r : g.regions) {
            QString note = regionNote(r.name);
            if (r.total > r.resident) {
                note += (note.isEmpty() ? QString() : StatsText::tr(", ")) +
                        StatsText::tr("%1 allocated").arg(VmStats::formatBytes(r.total));
            }
            html += tableRow(r.name, VmStats::formatBytes(r.resident), note);
        }
        html += "</table>";
        html += StatsText::tr("<i>QEMU is %n client(s) of it: its display and the guest's 3D "
                                "contexts.</i><br>",
                                nullptr, g.clients);
        if (g.shared) {
            html += StatsText::tr("<i>The buffers they share are counted once.</i><br>");
        }
    }
    return html;
}

/* The screen's own numbers: QEMU's updates as the view received them */
static QString screenDetails(const PerfStats::Snapshot &s, const Stats::Summary &v)
{
    const PerfStats::Screen &d = s.screen;
    const bool swap = d.method == "swap";
    QString html = StatsText::tr("<b>Screen</b>, over the last second");
    QStringList about;

    if (d.width && d.height) {
        about << StatsText::tr("%1×%2").arg(d.width).arg(d.height);
    }
    if (d.refreshHz > 0) {
        about << StatsText::tr("monitor at %1 Hz").arg(d.refreshHz, 0, 'f',
                                                         d.refreshHz < 100 ? 1 : 0);
    }
    if (!about.isEmpty()) {
        html += StatsText::tr(" (%1)").arg(about.join(StatsText::tr(", ")));
    }
    html += "<table>";
    html += tableRow(StatsText::tr("Frames on screen"),
                     StatsText::tr("%1/s").arg(qRound(d.presented)),
                     StatsText::tr("of %1 updates from QEMU").arg(qRound(d.flushes)));
    if (d.frame.samples) {
        html += tableRow(StatsText::tr("Frame latency"), PerfStats::formatMs(d.frame.median),
                         StatsText::tr("p99 %1: %2")
                             .arg(PerfStats::formatMs(d.frame.p99),
                                  swap ? StatsText::tr("update to buffer swap, without the "
                                                         "compositor")
                                       : StatsText::tr("update to on screen")));
    }
    if (d.interval.samples) {
        html += tableRow(StatsText::tr("Frame interval"), PerfStats::formatMs(d.interval.median),
                         StatsText::tr("p99 %1").arg(PerfStats::formatMs(d.interval.p99)));
    }
    if (v.inputs) {
        html += tableRow(StatsText::tr("Input"), PerfStats::formatMs(v.inputUsP50 / 1000),
                         StatsText::tr("p99 %1: a key or the mouse to QEMU and back")
                             .arg(PerfStats::formatMs(v.inputUsP99 / 1000)));
    }
    html += "</table>";
    if (swap) {
        html += StatsText::tr("<i>No presentation feedback from the compositor (X11): "
                                "latencies end at the buffer swap.</i><br>");
    }
    return html;
}

QString StatsText::details(Stat stat, const VmStats::Snapshot &s, const Context &context)
{
    switch (stat) {
    case Cpu:
        return cpuDetails(s);
    case Memory:
        return memoryDetails(s, context.ramMiB, context.guest, context.toolsFor);
    case Disk:
        return diskDetails(s);
    case Network:
        return networkDetails(s);
    case Gpu:
        return gpuDetails(s);
    case Frames:
        if (context.screen) {
            return screenDetails(s.perf, *context.screen);
        } else {
            /* the display's part: QEMU's threads have an entry of their own */
            PerfStats::Snapshot display = s.perf;
            display.threads = false;
            display.kvm = false;
            return PerfStats::details(display);
        }
    case MainLoop:
        return PerfStats::hostDetails(s.perf);
    }
    return {};
}
