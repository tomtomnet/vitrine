// SPDX-License-Identifier: GPL-2.0-or-later
#include "perfmonitor.h"

#include <QCursor>
#include <QEvent>
#include <QHelpEvent>
#include <QScreen>
#include <QToolTip>

#include "core/perfstats.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/vmconsole.h"
#include "vmview/vmview.h"

PerfMonitor::PerfMonitor(QWidget *parent)
    : QLabel(parent), m_sampler(new PerfStats::Sampler(this))
{
    setObjectName("perfMonitor");
    setTextFormat(Qt::PlainText);
    /* apart from the QEMU version that follows */
    setContentsMargins(0, 0, fontMetrics().averageCharWidth() * 3, 0);
    connect(m_sampler, &PerfStats::Sampler::changed, this, &PerfMonitor::refresh);
    hide();
}

void PerfMonitor::setVm(Vm *vm, VmConsole *console)
{
    if (m_console) {
        disconnect(m_console, nullptr, this, nullptr);
    }
    m_vm = vm;
    m_console = console;
    if (console) {
        connect(console, &VmConsole::statsChanged, this, &PerfMonitor::refresh);
    }
    m_sampler->setRunner(vm ? vm->runner() : nullptr);
    refresh();
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

bool PerfMonitor::snapshot(PerfStats::Snapshot *s) const
{
    const VmView *view = m_console ? m_console->view() : nullptr;

    *s = m_sampler->hasData() ? m_sampler->snapshot() : PerfStats::Snapshot();
    if (!view) {
        return m_sampler->hasData();
    }
    /* the screen counts what it puts on the screen; QEMU's numbers are the SDL display's */
    const Stats::Summary &v = m_console->stats();
    const double seconds = v.seconds > 0 ? v.seconds : 1;
    /* without presentation feedback (X11), frames end at the swap */
    const bool presented = v.presented > 0;
    PerfStats::Screen &d = s->screen;

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
    s->display = v.seconds > 0;
    s->displayType = "dbus";
    return true;
}

void PerfMonitor::refresh()
{
    PerfStats::Snapshot s;
    const QString text = snapshot(&s) ? PerfStats::summary(s) : QString();

    setText(text);
    setVisible(!text.isEmpty());
    /* an open tooltip follows the numbers */
    if (isVisible() && QToolTip::isVisible() && underMouse()) {
        QToolTip::showText(QCursor::pos(), tooltip(), this);
    }
}

static QString row(const QString &label, const QString &value, const QString &note = {})
{
    return QString("<tr><td>%1</td><td align=\"right\">&nbsp;&nbsp;%2</td>"
                   "<td>&nbsp;&nbsp;%3</td></tr>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped(), note.toHtmlEscaped());
}

/* The screen's own numbers: QEMU's updates as the view received them */
static QString screenDetails(const PerfStats::Snapshot &s, const Stats::Summary &v)
{
    const PerfStats::Screen &d = s.screen;
    const bool swap = d.method == "swap";
    QString html = PerfMonitor::tr("<b>Screen</b>, over the last second");
    QStringList about;

    if (d.width && d.height) {
        about << PerfMonitor::tr("%1×%2").arg(d.width).arg(d.height);
    }
    if (d.refreshHz > 0) {
        about << PerfMonitor::tr("monitor at %1 Hz").arg(d.refreshHz, 0, 'f',
                                                         d.refreshHz < 100 ? 1 : 0);
    }
    if (!about.isEmpty()) {
        html += PerfMonitor::tr(" (%1)").arg(about.join(PerfMonitor::tr(", ")));
    }
    html += "<table>";
    html += row(PerfMonitor::tr("Frames on screen"),
                PerfMonitor::tr("%1/s").arg(qRound(d.presented)),
                PerfMonitor::tr("of %1 updates from QEMU").arg(qRound(d.flushes)));
    if (d.frame.samples) {
        html += row(PerfMonitor::tr("Frame latency"), PerfStats::formatMs(d.frame.median),
                    PerfMonitor::tr("p99 %1: %2")
                        .arg(PerfStats::formatMs(d.frame.p99),
                             swap ? PerfMonitor::tr("update to buffer swap, without the "
                                                    "compositor")
                                  : PerfMonitor::tr("update to on screen")));
    }
    if (d.interval.samples) {
        html += row(PerfMonitor::tr("Frame interval"), PerfStats::formatMs(d.interval.median),
                    PerfMonitor::tr("p99 %1").arg(PerfStats::formatMs(d.interval.p99)));
    }
    if (v.inputs) {
        html += row(PerfMonitor::tr("Input"), PerfStats::formatMs(v.inputUsP50 / 1000),
                    PerfMonitor::tr("p99 %1: a key or the mouse to QEMU and back")
                        .arg(PerfStats::formatMs(v.inputUsP99 / 1000)));
    }
    html += "</table>";
    if (swap) {
        html += PerfMonitor::tr("<i>No presentation feedback from the compositor (X11): "
                                "latencies end at the buffer swap.</i><br>");
    }
    return html;
}

QString PerfMonitor::tooltip() const
{
    const QString name = m_vm ? m_vm->name().toHtmlEscaped() : QString();
    PerfStats::Snapshot s;

    snapshot(&s);
    const QString body = m_console && m_console->view()
                             ? screenDetails(s, m_console->stats()) + PerfStats::hostDetails(s)
                             : PerfStats::details(s);
    return QString("<p style=\"white-space:pre\"><b>%1</b></p>%2").arg(name, body);
}

bool PerfMonitor::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip && isVisible()) {
        QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(), tooltip(), this);
        return true;
    }
    return QLabel::event(event);
}
