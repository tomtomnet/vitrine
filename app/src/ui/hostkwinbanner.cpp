// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkwinbanner.h"

#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/hostkwin.h"
#include "ui/banner.h"

/* "Plasma 6.8.0" of "plasma-workspace 6.7.91 6.8.0", "Qt 6.11.3" of qt6-qtbase's */
static QString now(const QString &line)
{
    const QStringList f = line.split(' ');
    return HostKwin::packageTitle(f.value(0)) + ' ' + f.value(2);
}

HostKwinBanner::HostKwinBanner(QWidget *parent)
    : QWidget(parent), m_banner(new Banner(Banner::Warning))
{
    auto *layout = new QVBoxLayout(this);
    HostKwinMonitor *monitor = HostKwinMonitor::instance();

    setObjectName("hostKwinBanner");
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_banner);
    m_banner->button()->setText(tr("Rebuild…"));
    m_banner->button()->show();
    m_banner->button()->setToolTip(tr("The Builds window, on the host's KWin"));
    QPushButton *revert = m_banner->addButton(tr("Revert…"));
    revert->setToolTip(tr("Put the distribution's KWin back"));
    connect(m_banner->button(), &QPushButton::clicked, this, &HostKwinBanner::rebuildRequested);
    connect(revert, &QPushButton::clicked, this, &HostKwinBanner::revertRequested);
    connect(monitor, &HostKwinMonitor::changed, this, &HostKwinBanner::refresh);
    refresh();
    /* the system as it is now: Plasma may have moved since the last run */
    if (!monitor->isRead()) {
        monitor->refresh();
    }
}

HostKwinBanner *HostKwinBanner::addTo(QMainWindow *window)
{
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    auto *banner = new HostKwinBanner;

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(banner);
    layout->addWidget(window->takeCentralWidget(), 1);
    window->setCentralWidget(container);
    return banner;
}

QString HostKwinBanner::text() const
{
    const HostKwin::Status status = HostKwinMonitor::instance()->status();
    QStringList moved;

    if (!status.isOutdated()) {
        return {};
    }
    for (const QString &line : status.outdated) {
        moved << now(line).toHtmlEscaped();
    }
    return tr("<b>Vitrine's KWin no longer matches this computer's %1.</b> At your next login the "
              "desktop would run KWin %2 with the new libraries, and might not start. Build and "
              "install the matching KWin, or put the distribution's KWin back, before you log out.")
        .arg(moved.join(tr(" and ")), status.kwin.version.toHtmlEscaped());
}

void HostKwinBanner::refresh()
{
    const QString text = this->text();

    m_banner->setText(text);
    setVisible(!text.isEmpty());
}
