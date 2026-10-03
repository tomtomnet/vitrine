// SPDX-License-Identifier: GPL-2.0-or-later
#include "stackbanner.h"

#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/paths.h"
#include "core/stackbuilder.h"
#include "ui/banner.h"
#include "ui/qemubuilddialog.h"
#include "ui/qemudocs.h"

StackBanner::StackBanner(QWidget *parent)
    : QWidget(parent), m_warning(new Banner(Banner::Warning)),
      m_note(new Banner(Banner::Information))
{
    auto *layout = new QVBoxLayout(this);
    StackBuilder *builder = StackBuilder::instance();

    setObjectName("stackBanner");
    layout->setContentsMargins(6, 6, 6, 0);
    layout->addWidget(m_warning);
    layout->addWidget(m_note);
    for (Banner *banner : {m_warning, m_note}) {
        connect(banner->button(), &QPushButton::clicked, this, &StackBanner::buildRequested);
    }

    connect(builder, &StackBuilder::started, this, [this]() {
        m_error.clear();
        m_percent = -1;
        refresh();
    });
    connect(builder, &StackBuilder::stepStarted, this, [this]() {
        m_percent = -1;
        refresh();
    });
    /* by whole percents: ninja's lines come by the thousand */
    connect(builder, &StackBuilder::progress, this, [this](int done, int total) {
        const int percent = total > 0 ? done * 100 / total : -1;
        if (percent != m_percent) {
            m_percent = percent;
            refresh();
        }
    });
    connect(builder, &StackBuilder::finished, this, [this, builder](const QString &error) {
        /* stopped: as before it started */
        m_error = builder->wasStopped() ? QString() : error;
        refresh();
    });
    /* the preferences chose another QEMU, or the default again, even if
       that is the same binary: reloadPreferred() tells either way */
    connect(QemuDocs::preferred(), &QemuDocs::changed, this, &StackBanner::refresh);
    refresh();
}

StackBanner *StackBanner::addTo(QMainWindow *window)
{
    auto *container = new QWidget;
    auto *layout = new QVBoxLayout(container);
    auto *banner = new StackBanner;

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(banner);
    layout->addWidget(window->takeCentralWidget(), 1);
    window->setCentralWidget(container);
    return banner;
}

void StackBanner::refresh()
{
    const StackBuilder *builder = StackBuilder::instance();
    const StackBuilder::State state = StackBuilder::state();
    Banner *shown = m_warning;
    QString text;
    QString button;

    if (builder->isRunning()) {
        shown = m_note;
        text = tr("Building Vitrine's QEMU");
        if (builder->step() > 0) {
            text += tr(": step %1 of %2, %3").arg(builder->step()).arg(builder->steps())
                        .arg(builder->stepText().toHtmlEscaped());
            if (m_percent >= 0) {
                text += tr(" (%1%)").arg(m_percent);
            }
        }
        text += '.';
        button = tr("Show");
    } else if (!Paths::customQemuBinary().isEmpty() ||
               state == StackBuilder::State::UpToDate) {
        /* the user's QEMU, or nothing to do */
    } else if (!m_error.isEmpty()) {
        text = tr("Vitrine's QEMU could not be built. %1.").arg(m_error.toHtmlEscaped());
        button = tr("Show…");
    } else if (state == StackBuilder::State::NotBuilt) {
        text = tr("<b>Vitrine's QEMU is not built yet.</b> VMs run with it, for their display "
                  "in this window and for 3D acceleration: they start once it is built. "
                  "Building it takes a few minutes.");
        button = tr("Build…");
    } else if (state == StackBuilder::State::Outdated) {
        text = tr("<b>Vitrine's QEMU is out of date:</b> this version of Vitrine builds it from "
                  "other sources or patches. VMs running keep the QEMU they started with.");
        button = tr("Update…");
    } else {
        text = QemuBuildDialog::explain(state, {}).toHtmlEscaped();
    }

    for (Banner *banner : {m_warning, m_note}) {
        banner->setVisible(banner == shown && !text.isEmpty());
    }
    shown->setText(text);
    shown->button()->setText(button);
    shown->button()->setVisible(!button.isEmpty());
    setVisible(!text.isEmpty());
}
