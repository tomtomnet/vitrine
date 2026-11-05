// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkdebanner.h"

#include <QGuiApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/hostkde.h"
#include "ui/banner.h"
#include "ui/hostkdetext.h"

HostKdeBanner::HostKdeBanner(QWidget *parent)
    : QWidget(parent), m_banner(new Banner(Banner::Warning))
{
    auto *layout = new QVBoxLayout(this);
    HostKdeMonitor *monitor = HostKdeMonitor::instance();

    setObjectName("hostKdeBanner");
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_banner);
    m_banner->button()->setText(tr("Rebuild…"));
    m_banner->button()->setToolTip(tr("The Builds window, on the package to build again"));
    QPushButton *revert = m_banner->addButton(tr("Revert…"));
    revert->setToolTip(tr("Put the distribution's packages back"));
    connect(m_banner->button(), &QPushButton::clicked, this,
            [this]() { emit rebuildRequested(rebuildable()); });
    connect(revert, &QPushButton::clicked, this, [this]() { emit revertRequested(outdated()); });
    connect(monitor, &HostKdeMonitor::changed, this, &HostKdeBanner::refresh);
    /*
     * Back in front, from a terminal say: Plasma, the KDE Frameworks or Qt
     * updated meanwhile, or Vitrine's packages put back by hand.  rpm and
     * dnf take about a second to tell: read in the background, as at the
     * start, and not again while a reading is under way (the Builds window,
     * in sight, asks at the same time).
     */
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [monitor](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            monitor->lookAgain();
        }
    });
    refresh();
    /* the system as it is now: it may have moved since the last run */
    if (!monitor->isRead()) {
        monitor->refresh();
    }
}

HostKdeBanner *HostKdeBanner::addTo(QMainWindow *window)
{
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    auto *banner = new HostKdeBanner;

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(banner);
    layout->addWidget(window->takeCentralWidget(), 1);
    window->setCentralWidget(container);
    return banner;
}

QStringList HostKdeBanner::outdated() const
{
    return HostKdeNotice::outdated(HostKdeMonitor::instance()->statuses());
}

QString HostKdeBanner::rebuildable() const
{
    return HostKdeNotice::rebuildable(HostKdeMonitor::instance()->statuses());
}

QString HostKdeBanner::text() const
{
    return HostKdeNotice::text(HostKdeMonitor::instance()->statuses());
}

void HostKdeBanner::refresh()
{
    const QString text = this->text();

    m_banner->setText(text);
    m_banner->button()->setVisible(!rebuildable().isEmpty());
    setVisible(!text.isEmpty());
}
