// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemubuilddialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/paths.h"
#include "ui/widgets.h"

/* The repository's page, from its git URL */
static QString webPage(const QString &url)
{
    return url.endsWith(".git") ? url.chopped(4) : url;
}

QString QemuBuildDialog::title(StackBuilder::State state)
{
    return state == StackBuilder::State::Outdated ? tr("Update Vitrine's QEMU")
                                                  : tr("Build Vitrine's QEMU");
}

QString QemuBuildDialog::describe(const StackBuilder::Build &build)
{
    if (!build.isValid()) {
        return tr("Not built yet");
    }
    return tr("Vitrine's build of %1, %2")
        .arg(QLocale().toString(build.built.toLocalTime().date(), "d MMMM yyyy"),
             build.summary());
}

QString QemuBuildDialog::explain(StackBuilder::State state, const StackBuilder::Build &build)
{
    switch (state) {
    case StackBuilder::State::NoSources:
        return tr("This installation of Vitrine has no host/build.sh: it cannot build its QEMU.");
    case StackBuilder::State::NotBuilt:
        return tr("Vitrine's QEMU is not built yet: VMs need it for their display in Vitrine's "
                  "window and for 3D acceleration.");
    case StackBuilder::State::Outdated:
        return tr("Out of date: this version of Vitrine builds QEMU from other sources or "
                  "patches than the build of %1. VMs running keep the QEMU they started with.")
            .arg(QLocale().toString(build.built.toLocalTime().date(), "d MMMM yyyy"));
    case StackBuilder::State::UpToDate:
        return tr("Up to date.");
    }
    return {};
}

QemuBuildDialog::QemuBuildDialog(QWidget *parent)
    : QDialog(parent), m_builder(StackBuilder::instance()), m_about(Widgets::note()),
      m_status(Widgets::note()), m_missing(Widgets::note()), m_step(new QLabel),
      m_progress(new QProgressBar), m_background(Widgets::hint()), m_log(new QPlainTextEdit)
{
    auto *layout = new QVBoxLayout(this);
    auto *status = new QHBoxLayout;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    const QString host = StackBuilder::hostDir();
    const StackBuilder::Versions versions = StackBuilder::versions(host);
    /* those build.sh applies: its glob matches with case */
    const auto patches = [&host](const char *component) {
        return QDir(host + "/patches/" + component)
            .entryList({"*.patch"}, QDir::Files | QDir::CaseSensitive)
            .size();
    };

    if (versions.isValid()) {
        m_about->setText(
            tr("Vitrine runs the VMs with its own QEMU: the <a href=\"%1\">qemu-gui fork</a> at "
               "%2 with %3 patches, and <a href=\"%4\">virglrenderer</a> at %5 with %6 patches, "
               "for the display in Vitrine's window and 3D acceleration by the host GPU's own "
               "driver. Building downloads their sources and takes a few minutes.")
                .arg(webPage(versions.qemuUrl).toHtmlEscaped(), versions.qemuCommit.left(8))
                .arg(patches("qemu"))
                .arg(webPage(versions.virglUrl).toHtmlEscaped(), versions.virglCommit.left(8))
                .arg(patches("virglrenderer")));
        m_about->setToolTip(tr("From %1, into %2").arg(host, Paths::stackDir()));
    }
    layout->addWidget(m_about);
    layout->addWidget(m_status);
    layout->addWidget(Widgets::hint(
        tr("The build needs an internet connection and the development packages of QEMU and "
           "virglrenderer: it tells which ones are missing.")));
    m_missing->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_missing->hide();
    layout->addWidget(m_missing);

    /* the step and its progress on one line */
    m_step->setTextFormat(Qt::PlainText);
    m_step->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_step->setWordWrap(true);
    m_progress->hide();
    status->addWidget(m_step, 1);
    status->addWidget(m_progress, 1);
    layout->addLayout(status);
    m_background->setText(tr("Closing this window does not stop the build."));
    layout->addWidget(m_background);

    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_log, 1);

    m_build = buttons->addButton(tr("Build"), QDialogButtonBox::ActionRole);
    m_cancel = buttons->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    m_build->setDefault(true);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_build, &QPushButton::clicked, this, &QemuBuildDialog::build);
    connect(m_cancel, &QPushButton::clicked, m_builder, &StackBuilder::cancel);
    connect(m_builder, &StackBuilder::started, this, &QemuBuildDialog::started);
    connect(m_builder, &StackBuilder::stepStarted, this, &QemuBuildDialog::stepStarted);
    connect(m_builder, &StackBuilder::output, this, [this](const QString &text) {
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(text);
        m_log->moveCursor(QTextCursor::End);
    });
    connect(m_builder, &StackBuilder::progress, this, [this](int done, int total) {
        m_progress->setMaximum(total);
        m_progress->setValue(done);
    });
    connect(m_builder, &StackBuilder::finished, this, &QemuBuildDialog::finished);

    /* a build running already, started from another window: where it is */
    m_log->setPlainText(m_builder->log());
    m_log->moveCursor(QTextCursor::End);
    if (m_builder->isRunning()) {
        m_progress->setMaximum(0);
        m_progress->show();
        if (m_builder->step() > 0) {
            stepStarted(m_builder->step(), m_builder->steps(), m_builder->stepText());
        }
    }
    showMissing();
    updateState();
    resize(760, 620);
}

void QemuBuildDialog::build()
{
    m_qemuBefore = Paths::qemuBinary();
    m_log->clear();
    m_builder->start();
}

void QemuBuildDialog::started()
{
    m_step->setText(tr("Starting…"));
    m_progress->setMaximum(0);
    m_progress->setValue(0);
    m_progress->show();
    showMissing();
    updateState();
}

void QemuBuildDialog::stepStarted(int step, int total, const QString &text)
{
    m_step->setText(tr("Step %1 of %2: %3").arg(step).arg(total).arg(text));
    /* busy until ninja tells how far it is */
    m_progress->setMaximum(0);
    m_progress->setValue(0);
}

void QemuBuildDialog::finished(const QString &error)
{
    const StackBuilder::Build build = StackBuilder::current();

    m_progress->hide();
    if (error.isEmpty()) {
        m_step->setText(tr("Done: %1.").arg(describe(build)));
        emit built();
        const QString qemu = Paths::qemuBinary();
        if (qemu != m_qemuBefore) {
            emit qemuChanged(qemu);
        }
    } else {
        m_step->setText(tr("%1: see the log below.").arg(error));
    }
    showMissing();
    updateState();
}

void QemuBuildDialog::showMissing()
{
    const QStringList missing = m_builder->missing();
    QString text;

    if (missing.isEmpty() || m_builder->isRunning()) {
        m_missing->hide();
        return;
    }
    text = tr("Missing for the build: %1.").arg(missing.join(", ").toHtmlEscaped());
    if (!m_builder->installCommand().isEmpty()) {
        text += "<br>" + tr("Install them with: <code>%1</code>")
                             .arg(m_builder->installCommand().toHtmlEscaped());
    }
    m_missing->setText(text);
    m_missing->show();
}

void QemuBuildDialog::updateState()
{
    const bool running = m_builder->isRunning();
    const StackBuilder::State state = StackBuilder::state();
    const StackBuilder::Build build = StackBuilder::current();
    QString status = "<b>" + describe(build).toHtmlEscaped() + "</b>";

    if (state != StackBuilder::State::NotBuilt) {
        status += "<br>" + explain(state, build).toHtmlEscaped();
    }
    setWindowTitle(title(state));
    m_status->setText(status);
    m_build->setText(state == StackBuilder::State::Outdated ? tr("Update") : tr("Build"));
    m_build->setEnabled(!running && state != StackBuilder::State::NoSources);
    m_cancel->setVisible(running);
    m_background->setVisible(running);
}
