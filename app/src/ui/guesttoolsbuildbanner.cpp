// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsbuildbanner.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "ui/banner.h"
#include "ui/buildswindow.h"

using State = GuestToolsBuilder::State;

/* The date of the medium of @release, "5 October 2026"; empty if unknown */
static QString builtOn(const QString &release)
{
    const QDateTime built = GuestToolsBuilder::made(GuestToolsBuilder::mediumImage(release)).built;

    return built.isValid() ? QLocale().toString(built.toLocalTime().date(), "d MMMM yyyy")
                           : QString();
}

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
    const GuestToolsBuilder *builder = GuestToolsBuilder::instance();
    Banner *shown = m_warning;
    QString text;
    QString button;

    if (builder->isRunning()) {
        shown = m_note;
        text = tr("The guest tools for Fedora %1 are being built: %2.")
                   .arg(builder->runRelease(), builder->progressText().toHtmlEscaped());
        button = tr("Show");
    } else {
        const State state = GuestToolsBuilder::releaseState(m_release);
        const QString date = builtOn(m_release);

        if (!builder->error().isEmpty() && (state == State::NotBuilt || state == State::Outdated)) {
            text = tr("The guest tools could not be built: %1.").arg(builder->error().toHtmlEscaped());
            button = tr("Show…");
        } else if (state == State::NotBuilt) {
            /* KWin, Qt base and the rest of KDE's take longer than Mesa itself */
            const bool kde = GuestToolsBuilder::packages(GuestToolsBuilder::guestDir(), m_release)
                                 .contains("kwin");
            text = tr("<b>The guest tools for Fedora %1 are not built yet.</b> Vitrine builds "
                      "them in a Fedora %1 container, which takes %2.")
                       .arg(m_release, kde ? tr("40 to 70 minutes") : tr("15 to 30 minutes"));
            button = tr("Build…");
        } else if (state == State::Outdated) {
            shown = m_note;
            text = date.isEmpty()
                       ? tr("<b>The guest tools medium for Fedora %1 is out of date:</b> this "
                            "version of Vitrine has other guest tools sources.")
                             .arg(m_release)
                       : tr("<b>The guest tools medium for Fedora %1 is out of date:</b> this "
                            "version of Vitrine has other guest tools sources than the medium "
                            "made on %2.")
                             .arg(m_release, date);
            button = tr("Update…");
        } else if (state == State::NoSources && !GuestTools::medium(m_release).isValid()) {
            text = tr("The guest tools are not built, and this installation of Vitrine has no "
                      "guest/ folder to build them from.");
        }
    }
    for (Banner *banner : {m_warning, m_note}) {
        banner->setVisible(banner == shown && !text.isEmpty());
    }
    shown->setText(text);
    shown->button()->setText(button);
    shown->button()->setVisible(!button.isEmpty());
    setVisible(!text.isEmpty());
}
