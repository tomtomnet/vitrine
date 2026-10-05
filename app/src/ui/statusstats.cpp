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
#include <QToolButton>
#include <QToolTip>

#include <algorithm>
#include <iterator>

#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/icons.h"
#include "ui/vmconsole.h"
#include "ui/widgets.h"
#include "vmview/vmview.h"

using VmStats::Sampler;

/* In the settings: statusbar/KEY */
static const char *const kKeys[StatusStats::kStats] = {"cpu", "memory", "disk", "network",
                                                       "gpu", "frames", "mainloop"};
/* What shows at first: the CPU and memory, and what the status bar had before */
static const bool kDefaults[StatusStats::kStats] = {true, true, false, false, false, true, true};

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
    : QWidget(parent), m_sampler(new Sampler(this)),
      m_button(Widgets::statusButton(
          "statsMenu",
          Icons::themed({"view-statistics", "office-chart-bar", "utilities-system-monitor"},
                        QStyle::SP_FileDialogDetailedView))),
      m_menu(new QMenu(this))
{
    const QString tips[kStats] = {
        tr("How busy the guest keeps its vCPUs; the rest of QEMU in the details"),
        tr("The host's memory QEMU holds: the guest's RAM it touched, and its own"),
        tr("What the guest reads from its disks and writes to them"),
        tr("What the guest receives and sends, by its own counters: needs the guest tools"),
        tr("How busy QEMU keeps the host's GPU, and the GPU memory it holds"),
        tr("The frames the screen shows each second, their latency and the input's"),
        tr("How long QEMU's main loop waits for a CPU: a starved main loop stutters"),
    };

    setObjectName("statusStats");
    m_names[Cpu] = tr("CPU");
    m_names[Memory] = tr("Memory");
    m_names[Disk] = tr("Disk");
    m_names[Network] = tr("Network");
    m_names[Gpu] = tr("GPU");
    m_names[Frames] = tr("Frame Rate and Latency");
    m_names[MainLoop] = tr("Main Loop Wait");
    for (int i = 0; i < kStats; i++) {
        m_labels[i] = new QLabel(this);
        m_labels[i]->setObjectName(QString("stat-") + kKeys[i]);
        m_labels[i]->setTextFormat(Qt::PlainText);
        m_labels[i]->installEventFilter(this);
        m_labels[i]->hide();
        m_actions[i] = m_menu->addAction(m_names[i]);
        m_actions[i]->setCheckable(true);
        m_actions[i]->setChecked(setting(kKeys[i], kDefaults[i]));
        m_actions[i]->setToolTip(tips[i]);
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

    /* the end of the status bar: an icon, no text, the menu at a click */
    m_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_button->setPopupMode(QToolButton::InstantPopup);
    m_button->setMenu(m_menu);
    m_button->setToolTip(tr("What the status bar shows"));
    /* shown once in the status bar: without a parent it would be a window */
    m_button->installEventFilter(this);

    connect(m_sampler, &Sampler::changed, this, &StatusStats::refresh);
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
    }
    if (m_console) {
        disconnect(m_console, nullptr, this, nullptr);
    }
    m_vm = vm;
    m_console = console;
    if (console) {
        connect(console, &VmConsole::statsChanged, this, &StatusStats::refresh);
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
        sources |= Sampler::Pss;
    }
    m_sampler->setSources(sources);
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

VmStats::Snapshot StatusStats::snapshot() const
{
    VmStats::Snapshot s = m_sampler->snapshot();
    const VmView *view = m_console ? m_console->view() : nullptr;

    if (!view) {
        return s;
    }
    /* the screen counts what it puts on the screen; QEMU's numbers are the SDL display's */
    const Stats::Summary &v = m_console->stats();
    const double seconds = v.seconds > 0 ? v.seconds : 1;
    /* without presentation feedback (X11), frames end at the swap */
    const bool presented = v.presented > 0;
    PerfStats::Screen &d = s.perf.screen;

    d = PerfStats::Screen();
    d.width = view->guestSize().width();
    d.height = view->guestSize().height();
    if (const QScreen *screen = m_console->screen()) {
        d.refreshHz = screen->refreshRate();
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
    return s;
}

QString StatusStats::text(Stat stat, const VmStats::Snapshot &s) const
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

void StatusStats::refresh()
{
    const VmStats::Snapshot s = snapshot();

    for (int i = 0; i < kStats; i++) {
        const QString t = isShown(Stat(i)) ? text(Stat(i), s) : QString();
        m_labels[i]->setText(t);
        if (!t.isEmpty()) {
            m_widths[i] = qMax(m_widths[i], m_labels[i]->sizeHint().width());
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
    const bool runs = m_vm && m_vm->runner()->isActive();
    const VmStats::Snapshot &s = m_sampler->snapshot();
    QString notes[kStats];

    if (runs) {
        switch (m_sampler->guestSource()) {
        case VmStats::GuestSource::NoAgent:
            notes[Network] = tr("needs the guest tools");
            break;
        case VmStats::GuestSource::OldAgent:
            notes[Network] = tr("needs newer guest tools");
            break;
        default:
            break;
        }
        if (!s.gpuError.isEmpty()) {
            notes[Gpu] = tr("cannot be read");
        } else if (s.drmClients == 0) {
            notes[Gpu] = tr("QEMU does not use it");
        }
    }
    for (int i = 0; i < kStats; i++) {
        m_actions[i]->setText(notes[i].isEmpty() ? m_names[i]
                                                 : tr("%1 (%2)").arg(m_names[i], notes[i]));
    }
}

/* --- tooltips --- */

static QString row(const QString &label, const QString &value, const QString &note = {})
{
    return QString("<tr><td>%1</td><td align=\"right\">&nbsp;&nbsp;%2</td>"
                   "<td>&nbsp;&nbsp;%3</td></tr>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped(), note.toHtmlEscaped());
}

/* % of one CPU, as host CPUs: "1.7 CPUs" */
static QString cpus(double percent)
{
    return StatusStats::tr("%1 CPUs").arg(percent / 100, 0, 'f', percent < 995 ? 2 : 1);
}

static QString cpuDetails(const VmStats::Snapshot &s)
{
    const PerfStats::Snapshot &p = s.perf;
    const double besides = p.mainLoop.cpu + p.others.cpu;
    const double total = p.vcpus.cpu + besides;
    QString html = StatusStats::tr("<b>CPU</b>, over the last second") + "<table>";

    if (p.vcpus.threads) {
        html += row(StatusStats::tr("Guest"),
                    PerfStats::formatPercent(p.vcpus.cpu / p.vcpus.threads),
                    StatusStats::tr("of its %n vCPU(s): %1 of the host's", nullptr,
                                    p.vcpus.threads)
                        .arg(cpus(p.vcpus.cpu)));
    }
    html += row(StatusStats::tr("QEMU besides"), cpus(besides),
                StatusStats::tr("main loop %1, %n other thread(s) %2: GPU, I/O, audio…",
                                nullptr, p.others.threads)
                    .arg(PerfStats::formatPercent(p.mainLoop.cpu),
                         PerfStats::formatPercent(p.others.cpu)));
    if (s.hostCpus > 0) {
        html += row(StatusStats::tr("In all"), cpus(total),
                    StatusStats::tr("%1 of the host's %n CPU(s)", nullptr, s.hostCpus)
                        .arg(PerfStats::formatPercent(total / s.hostCpus)));
    }
    html += "</table>";
    if (!s.busiest.isEmpty()) {
        html += StatusStats::tr("<b>Busiest threads</b>") + "<table>";
        for (const VmStats::ThreadUse &t : s.busiest) {
            html += row(t.name.isEmpty() ? StatusStats::tr("thread %1").arg(t.tid) : t.name,
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
                             VmStats::GuestSource guest)
{
    using VmStats::formatBytes;
    const VmStats::Memory &m = s.qemuMemory;
    QString html = StatusStats::tr("<b>Memory</b> of the host that QEMU holds") + "<table>";

    html += row(StatusStats::tr("Resident"), formatBytes(m.resident),
                ramMiB > 0 ? StatusStats::tr("the guest has %1 of RAM")
                                 .arg(formatBytes(ramMiB * 1024 * 1024))
                           : QString());
    if (m.shmem > 0) {
        html += row(StatusStats::tr("Shared"), formatBytes(m.shmem),
                    StatusStats::tr("the guest's RAM, as far as it has used it"));
    }
    html += row(StatusStats::tr("Private"), formatBytes(m.anon),
                m.shmem > 0 ? StatusStats::tr("QEMU's own: its heap and buffers")
                            : StatusStats::tr("the guest's RAM and QEMU's own"));
    html += row(StatusStats::tr("Files"), formatBytes(m.file),
                StatusStats::tr("QEMU's program and libraries"));
    if (m.swap > 0) {
        html += row(StatusStats::tr("Swapped out"), formatBytes(m.swap));
    }
    if (m.proportional >= 0) {
        html += row(StatusStats::tr("Proportional"), formatBytes(m.proportional),
                    StatusStats::tr("PSS: pages shared with passt or virtiofsd counted in part"));
    }
    html += "</table>";
    if (s.guestMemory) {
        html += StatusStats::tr("<b>The guest</b>, by the guest tools") + "<table>" +
                row(StatusStats::tr("In use"), formatBytes(s.guestTotal - s.guestAvailable),
                    StatusStats::tr("of %1, its caches left out").arg(formatBytes(s.guestTotal))) +
                "</table>";
    } else if (guest == VmStats::GuestSource::NoAgent) {
        html += StatusStats::tr("<i>What the guest itself uses needs the guest tools.</i>");
    } else if (guest == VmStats::GuestSource::OldAgent) {
        html += StatusStats::tr("<i>What the guest itself uses needs newer guest tools.</i>");
    }
    return html;
}

static QString diskDetails(const VmStats::Snapshot &s)
{
    using VmStats::formatBytes;
    using VmStats::formatRate;
    QString html = StatusStats::tr("<b>Disks</b>, over the last second") + "<table>";

    for (const VmStats::DeviceRate &d : s.diskUse.devices) {
        if (d.readTotal == 0 && d.writtenTotal == 0) {
            continue;       // an empty drive
        }
        html += row(d.name,
                    StatusStats::tr("R %1 · W %2").arg(formatRate(d.read), formatRate(d.written)),
                    StatusStats::tr("%1 read and %2 written since QEMU started")
                        .arg(formatBytes(d.readTotal), formatBytes(d.writtenTotal)));
    }
    return html + "</table>";
}

static QString networkDetails(const VmStats::Snapshot &s)
{
    using VmStats::formatBytes;
    using VmStats::formatRate;
    QString html = StatusStats::tr("<b>Network</b>, by the guest's own counters (guest tools)") +
                   "<table>";

    for (const VmStats::InterfaceRate &i : s.net.interfaces) {
        html += row(i.name,
                    StatusStats::tr("↓ %1 · ↑ %2").arg(formatRate(i.rx), formatRate(i.tx)),
                    StatusStats::tr("%1 received and %2 sent since the guest started")
                        .arg(formatBytes(i.rxTotal), formatBytes(i.txTotal)));
    }
    if (s.net.interfaces.isEmpty()) {
        html += row(StatusStats::tr("No network card"), QString());
    }
    return html + "</table>";
}

/* The drivers' names of their memory regions, said plainly */
static QString regionNote(const QString &region)
{
    if (region.startsWith("vram") || region.startsWith("local")) {
        return StatusStats::tr("the GPU's own memory");
    }
    if (region == "gtt") {
        return StatusStats::tr("host memory the GPU maps");
    }
    if (region == "cpu" || region.startsWith("system")) {
        return StatusStats::tr("host memory");
    }
    return {};
}

static QString gpuDetails(const VmStats::Snapshot &s)
{
    QString html;

    for (const VmStats::GpuUse &g : s.gpus) {
        html += StatusStats::tr("<b>GPU</b> %1 (%2), over the last second")
                    .arg(g.pdev.toHtmlEscaped(), g.driver.toHtmlEscaped()) +
                "<table>";
        for (const VmStats::EngineUse &e : g.engines) {
            html += row(e.name, PerfStats::formatPercent(e.busy));
        }
        for (const VmStats::RegionUse &r : g.regions) {
            QString note = regionNote(r.name);
            if (r.total > r.resident) {
                note += (note.isEmpty() ? QString() : StatusStats::tr(", ")) +
                        StatusStats::tr("%1 allocated").arg(VmStats::formatBytes(r.total));
            }
            html += row(r.name, VmStats::formatBytes(r.resident), note);
        }
        html += "</table>";
        html += StatusStats::tr("<i>QEMU is %n client(s) of it: its display and the guest's 3D "
                                "contexts.</i><br>",
                                nullptr, g.clients);
    }
    return html;
}

/* The screen's own numbers: QEMU's updates as the view received them */
static QString screenDetails(const PerfStats::Snapshot &s, const Stats::Summary &v)
{
    const PerfStats::Screen &d = s.screen;
    const bool swap = d.method == "swap";
    QString html = StatusStats::tr("<b>Screen</b>, over the last second");
    QStringList about;

    if (d.width && d.height) {
        about << StatusStats::tr("%1×%2").arg(d.width).arg(d.height);
    }
    if (d.refreshHz > 0) {
        about << StatusStats::tr("monitor at %1 Hz").arg(d.refreshHz, 0, 'f',
                                                         d.refreshHz < 100 ? 1 : 0);
    }
    if (!about.isEmpty()) {
        html += StatusStats::tr(" (%1)").arg(about.join(StatusStats::tr(", ")));
    }
    html += "<table>";
    html += row(StatusStats::tr("Frames on screen"),
                StatusStats::tr("%1/s").arg(qRound(d.presented)),
                StatusStats::tr("of %1 updates from QEMU").arg(qRound(d.flushes)));
    if (d.frame.samples) {
        html += row(StatusStats::tr("Frame latency"), PerfStats::formatMs(d.frame.median),
                    StatusStats::tr("p99 %1: %2")
                        .arg(PerfStats::formatMs(d.frame.p99),
                             swap ? StatusStats::tr("update to buffer swap, without the "
                                                    "compositor")
                                  : StatusStats::tr("update to on screen")));
    }
    if (d.interval.samples) {
        html += row(StatusStats::tr("Frame interval"), PerfStats::formatMs(d.interval.median),
                    StatusStats::tr("p99 %1").arg(PerfStats::formatMs(d.interval.p99)));
    }
    if (v.inputs) {
        html += row(StatusStats::tr("Input"), PerfStats::formatMs(v.inputUsP50 / 1000),
                    StatusStats::tr("p99 %1: a key or the mouse to QEMU and back")
                        .arg(PerfStats::formatMs(v.inputUsP99 / 1000)));
    }
    html += "</table>";
    if (swap) {
        html += StatusStats::tr("<i>No presentation feedback from the compositor (X11): "
                                "latencies end at the buffer swap.</i><br>");
    }
    return html;
}

QString StatusStats::tooltip(Stat stat) const
{
    const VmStats::Snapshot s = snapshot();
    const QString name = m_vm ? m_vm->name().toHtmlEscaped() : QString();
    QString body;

    switch (stat) {
    case Cpu:
        body = cpuDetails(s);
        break;
    case Memory:
        body = memoryDetails(s, m_vm ? VmConfig::memoryMiB(m_vm->runner()->runArgs()) : 0,
                             m_sampler->guestSource());
        break;
    case Disk:
        body = diskDetails(s);
        break;
    case Network:
        body = networkDetails(s);
        break;
    case Gpu:
        body = gpuDetails(s);
        break;
    case Frames:
        if (m_console && m_console->view()) {
            body = screenDetails(s.perf, m_console->stats());
        } else {
            /* the display's part: QEMU's threads have an entry of their own */
            PerfStats::Snapshot display = s.perf;
            display.threads = false;
            display.kvm = false;
            body = PerfStats::details(display);
        }
        break;
    case MainLoop:
        body = PerfStats::hostDetails(s.perf);
        break;
    }
    return QString("<p style=\"white-space:pre\"><b>%1</b></p>%2").arg(name, body);
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
