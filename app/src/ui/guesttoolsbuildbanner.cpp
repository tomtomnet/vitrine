// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsbuildbanner.h"

#include <QDateTime>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "ui/banner.h"
#include "ui/buildswindow.h"

using State = GuestToolsBuilder::State;

/* The date of the medium, "5 October 2026"; empty if unknown */
static QString builtOn()
{
    const QDateTime built = GuestToolsBuilder::made(GuestToolsBuilder::mediumImage()).built;

    return built.isValid() ? QLocale().toString(built.toLocalTime().date(), "d MMMM yyyy")
                           : QString();
}

GuestToolsBuildBanner::GuestToolsBuildBanner(QWidget *parent)
    : QWidget(parent), m_warning(new Banner(Banner::Warning)),
      m_note(new Banner(Banner::Information))
{
    auto *layout = new QVBoxLayout(this);
    GuestToolsBuilder *builder = GuestToolsBuilder::instance();

    setObjectName("guestToolsBuildBanner");
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_warning);
    layout->addWidget(m_note);
    for (Banner *banner : {m_warning, m_note}) {
        connect(banner->button(), &QPushButton::clicked, this,
                [this]() { BuildsWindow::present(this, "medium"); });
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
    connect(builder, &GuestToolsBuilder::built, this, &GuestToolsBuildBanner::mediumChanged);
    refresh();
}

void GuestToolsBuildBanner::refresh()
{
    const GuestToolsBuilder *builder = GuestToolsBuilder::instance();
    Banner *shown = m_warning;
    QString text;
    QString button;

    if (builder->isRunning()) {
        shown = m_note;
        text = tr("The guest tools are being built: %1.")
                   .arg(builder->progressText().toHtmlEscaped());
        button = tr("Show");
    } else {
        const State state = GuestToolsBuilder::state();

        if (!builder->error().isEmpty() && (state == State::NotBuilt || state == State::Outdated)) {
            text = tr("The guest tools could not be built: %1.").arg(builder->error().toHtmlEscaped());
            button = tr("Show…");
        } else if (state == State::NotBuilt) {
            text = tr("<b>The guest tools are not built yet.</b> Vitrine builds them in a Fedora "
                      "container, which takes 20 to 40 minutes.");
            button = tr("Build…");
        } else if (state == State::Outdated) {
            shown = m_note;
            text = builtOn().isEmpty()
                       ? tr("<b>The guest tools medium is out of date:</b> this version of "
                            "Vitrine has other guest tools sources.")
                       : tr("<b>The guest tools medium is out of date:</b> this version of "
                            "Vitrine has other guest tools sources than the medium made on %1.")
                             .arg(builtOn());
            button = tr("Update…");
        } else if (state == State::NoSources && !GuestTools::medium().isValid()) {
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
