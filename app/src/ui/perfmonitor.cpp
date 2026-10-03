// SPDX-License-Identifier: GPL-2.0-or-later
#include "perfmonitor.h"

#include <QCursor>
#include <QEvent>
#include <QHelpEvent>
#include <QToolTip>

#include "core/perfstats.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

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

void PerfMonitor::setVm(Vm *vm)
{
    m_vm = vm;
    m_sampler->setRunner(vm ? vm->runner() : nullptr);
    refresh();
}

void PerfMonitor::refresh()
{
    const QString text = m_sampler->hasData() ? PerfStats::summary(m_sampler->snapshot())
                                              : QString();

    setText(text);
    setVisible(!text.isEmpty());
    /* an open tooltip follows the numbers */
    if (isVisible() && QToolTip::isVisible() && underMouse()) {
        QToolTip::showText(QCursor::pos(), tooltip(), this);
    }
}

QString PerfMonitor::tooltip() const
{
    const QString name = m_vm ? m_vm->name().toHtmlEscaped() : QString();

    return QString("<p style=\"white-space:pre\"><b>%1</b></p>%2")
        .arg(name, PerfStats::details(m_sampler->snapshot()));
}

bool PerfMonitor::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip && m_sampler->hasData()) {
        QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(), tooltip(), this);
        return true;
    }
    return QLabel::event(event);
}
