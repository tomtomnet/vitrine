// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkdebanner.h"

#include <QGuiApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/hostkde.h"
#include "ui/banner.h"

/* "Plasma 6.8.0" of "plasma-workspace 6.7.91 6.8.0", "Qt 6.11.3" of qt6-qtbase's */
static QString now(const QString &line)
{
    const QStringList f = line.split(' ');
    return HostKde::packageTitle(f.value(0)) + ' ' + f.value(2);
}

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
    QStringList ids;

    for (const HostKde::Status &s : HostKdeMonitor::instance()->statuses()) {
        if (s.isOutdated()) {
            ids << s.target.id;
        }
    }
    return ids;
}

QString HostKdeBanner::rebuildable() const
{
    for (const HostKde::Status &s : HostKdeMonitor::instance()->statuses()) {
        if (s.isOutdated() && s.canBuild()) {
            return s.target.id;
        }
    }
    return {};
}

QString HostKdeBanner::text() const
{
    QStringList sentences;
    bool kwin = false;

    for (const HostKde::Status &s : HostKdeMonitor::instance()->statuses()) {
        QStringList moved;
        if (!s.isOutdated()) {
            continue;
        }
        for (const QString &line : s.outdated) {
            moved << now(line).toHtmlEscaped();
        }
        const QString title = s.target.title.toHtmlEscaped();
        if (s.target.package == "kwin") {
            kwin = true;
            sentences << tr("<b>Vitrine's KWin no longer matches this computer's %1.</b> At your next "
                            "login the desktop would run KWin %2 with the new libraries, and might not "
                            "start.")
                             .arg(moved.join(tr(" and ")), s.installed.version.toHtmlEscaped());
        } else {
            sentences << tr("<b>Vitrine's %1 no longer matches this computer's %2.</b> Programs use "
                            "%1 %3 with the newer libraries, which upstream does not support: some "
                            "may not work as they should.")
                             .arg(title, moved.join(tr(" and ")), s.installed.version.toHtmlEscaped());
        }
    }
    if (sentences.isEmpty()) {
        return {};
    }
    const QString what = rebuildable().isEmpty()
                             ? tr("The research has no set for the new version yet: put the "
                                  "distribution's packages back")
                             : tr("Build and install the matching packages, or put the distribution's "
                                  "back");
    return sentences.join(' ') + ' ' +
           (kwin ? tr("%1 before you log out.").arg(what) : tr("%1.").arg(what));
}

void HostKdeBanner::refresh()
{
    const QString text = this->text();

    m_banner->setText(text);
    m_banner->button()->setVisible(!rebuildable().isEmpty());
    setVisible(!text.isEmpty());
}
