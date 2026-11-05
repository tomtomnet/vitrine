// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsbuildbanner.h"

#include <QGuiApplication>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "ui/banner.h"
#include "ui/buildswindow.h"
#include "ui/guesttoolstext.h"

GuestToolsBuildBanner::GuestToolsBuildBanner(QWidget *parent)
    : QWidget(parent), m_release(GuestTools::newestRelease()),
      m_warning(new Banner(Banner::Warning)), m_note(new Banner(Banner::Information))
{
    auto *layout = new QVBoxLayout(this);
    GuestToolsBuilder *builder = GuestToolsBuilder::instance();

    setObjectName("guestToolsBuildBanner");
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_warning);
    layout->addWidget(m_note);
    for (Banner *banner : {m_warning, m_note}) {
        connect(banner->button(), &QPushButton::clicked, this,
                [this]() { BuildsWindow::present(this, "medium", m_release); });
    }
    connect(builder, &GuestToolsBuilder::started, this, &GuestToolsBuildBanner::refresh);
    connect(builder, &GuestToolsBuilder::waitingChanged, this, &GuestToolsBuildBanner::refresh);
    connect(builder, &GuestToolsBuilder::stepStarted, this, &GuestToolsBuildBanner::refresh);
    connect(builder, &GuestToolsBuilder::phaseChanged, this, &GuestToolsBuildBanner::refresh);
    /* by whole percents: ninja's lines come by the thousand */
    connect(builder, &GuestToolsBuilder::progress, this, [this](int done, int total) {
        const int percent = total > 0 ? done * 100 / total : -1;
        if (percent != m_percent) {
            m_percent = percent;
            refresh();
        }
    });
    connect(builder, &GuestToolsBuilder::finished, this, &GuestToolsBuildBanner::refresh);
    connect(builder, &GuestToolsBuilder::mediumChanged, this, &GuestToolsBuildBanner::mediumChanged);
    /*
     * Back in front, from a terminal say: what guest/build-rpms.sh and
     * guest/build-medium.sh made meanwhile, run by hand.  The state takes
     * a millisecond or two to tell (it hashes guest/'s inputs, about 1 MB):
     * looked at again each time, no file watched.
     */
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this, builder](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            builder->checkMedium();
            refresh();
        }
    });
    refresh();
}

void GuestToolsBuildBanner::setRelease(const QString &release)
{
    if (release != m_release) {
        m_release = release;
        refresh();
    }
}

void GuestToolsBuildBanner::refresh()
{
    QString button;
    bool warning = true;
    const QString text = GuestToolsText::mediumNote(m_release, &button, &warning);
    Banner *shown = warning ? m_warning : m_note;

    for (Banner *banner : {m_warning, m_note}) {
        banner->setVisible(banner == shown && !text.isEmpty());
    }
    shown->setText(text);
    shown->button()->setText(button);
    shown->button()->setVisible(!button.isEmpty());
    setVisible(!text.isEmpty());
}
