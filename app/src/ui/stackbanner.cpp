// SPDX-License-Identifier: GPL-2.0-or-later
#include "stackbanner.h"

#include <QGuiApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/stackbuilder.h"
#include "ui/banner.h"
#include "ui/buildtexts.h"
#include "ui/qemudocs.h"

StackBanner::StackBanner(QWidget *parent)
    : QWidget(parent), m_warning(new Banner(Banner::Warning)),
      m_note(new Banner(Banner::Information))
{
    auto *layout = new QVBoxLayout(this);
    StackBuilder *builder = StackBuilder::instance();

    setObjectName("stackBanner");
    /* as much room under it as around it: the list of VMs touched it */
    layout->setContentsMargins(6, 6, 6, 6);
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
    /*
     * Back in front, from a terminal say: what host/build.sh made meanwhile,
     * run by hand.  The state takes a millisecond or two to tell (it hashes
     * host/'s inputs, about 1 MB, and reads the build's manifest): looked at
     * again each time, no file watched.
     */
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            refresh();
        }
    });
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
    QString button;
    bool note = false;
    const QString text =
        BuildTexts::stackNote(StackBuilder::instance(), m_percent, m_error, &button, &note);
    Banner *shown = note ? m_note : m_warning;

    for (Banner *banner : {m_warning, m_note}) {
        banner->setVisible(banner == shown && !text.isEmpty());
    }
    shown->setText(text);
    shown->button()->setText(button);
    shown->button()->setVisible(!button.isEmpty());
    setVisible(!text.isEmpty());
}
