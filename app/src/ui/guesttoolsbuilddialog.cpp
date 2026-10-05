// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsbuilddialog.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/guesttools.h"
#include "core/stackbuilder.h"
#include "ui/banner.h"
#include "ui/icons.h"
#include "ui/widgets.h"

using State = GuestToolsBuilder::State;

/* The lines of the log a window shows: the builder keeps more */
static const int kLogLines = 5000;

/* "10.0" */
static QString gib(qint64 mib)
{
    return QLocale().toString(double(mib) / 1024, 'f', 1);
}

/* The end of @log: @lines lines at most */
static QString tail(const QString &log, int lines)
{
    qsizetype at = log.size();

    for (int i = 0; i <= lines; i++) {
        if (at <= 0 || (at = log.lastIndexOf('\n', at - 1)) < 0) {
            return log;
        }
    }
    return log.mid(at + 1);
}

/* The date of the medium, "5 October 2026"; empty if unknown */
static QString builtOn()
{
    const QDateTime built = GuestToolsBuilder::made(GuestToolsBuilder::mediumImage()).built;

    return built.isValid() ? QLocale().toString(built.toLocalTime().date(), "d MMMM yyyy")
                           : QString();
}

QString GuestToolsBuildDialog::title(State state)
{
    return state == State::Outdated ? tr("Update Guest Tools") : tr("Build Guest Tools");
}

QString GuestToolsBuildDialog::describe()
{
    const GuestTools::Medium medium = GuestTools::medium();
    const QString release = ".fc" + QString(GuestTools::kFedoraRelease);
    QString tools = medium.tools;
    QStringList parts;

    if (!medium.isValid()) {
        return tr("Not built yet");
    }
    if (tools.endsWith(release)) {
        tools.chop(release.size());
    }
    parts << "vitrine-guest-tools " + tools;
    if (!medium.mesa.isEmpty()) {
        parts << tr("Mesa %1").arg(medium.mesa.section('-', 0, 0));
    }
    if (!medium.kwin.isEmpty()) {
        parts << tr("KWin %1").arg(medium.kwin.section('-', 0, 0));
    }
    const QString date = builtOn();
    return date.isEmpty() ? tr("The medium of %1").arg(parts.join(", "))
                          : tr("Built on %1: %2").arg(date, parts.join(", "));
}

QString GuestToolsBuildDialog::explain(State state)
{
    switch (state) {
    case State::NoSources:
        return tr("This installation of Vitrine has no guest/ folder: it cannot build the guest "
                  "tools.");
    case State::NotBuilt:
        return tr("The guest tools are not built yet: Fedora guests need them for Vitrine's "
                  "graphics driver, Mesa with native context and KWin.");
    case State::Outdated:
        return tr("Out of date: this version of Vitrine has other guest tools sources than this "
                  "medium. VMs keep the guest tools they have until they are updated.");
    case State::UpToDate:
        return tr("Up to date.");
    }
    return {};
}

QString GuestToolsBuildDialog::progressText(const GuestToolsBuilder *builder, bool percent)
{
    QStringList detail;
    QString text;

    if (builder->step() < 1) {
        return tr("starting");
    }
    text = tr("step %1 of %2: %3")
               .arg(builder->step())
               .arg(builder->steps().size())
               .arg(builder->stepText());
    if (builder->isWaiting()) {
        detail << tr("waiting for memory");
    } else {
        if (!builder->phase().isEmpty()) {
            detail << builder->phase();
        }
        if (percent && builder->progressTotal() > 0) {
            detail << tr("%1%").arg(builder->progressDone() * 100 / builder->progressTotal());
        }
    }
    return detail.isEmpty() ? text : text + " (" + detail.join(", ") + ")";
}

void GuestToolsBuildDialog::present(QWidget *from)
{
    static QPointer<GuestToolsBuildDialog> open;
    QWidget *parent = from ? from->window() : nullptr;

    /* over the window that asks: one under a modal dialog would take no input */
    if (open && open->parentWidget() != parent) {
        open->close();
        open = nullptr;
    }
    if (!open) {
        open = new GuestToolsBuildDialog(parent);
        open->setAttribute(Qt::WA_DeleteOnClose);
    }
    open->show();
    open->raise();
    open->activateWindow();
}

GuestToolsBuildDialog::GuestToolsBuildDialog(QWidget *parent)
    : QDialog(parent), m_builder(GuestToolsBuilder::instance()), m_about(Widgets::note()),
      m_status(Widgets::note()), m_missing(Widgets::note()),
      m_memory(new Banner(Banner::Warning)), m_step(new QLabel), m_progress(new QProgressBar),
      m_background(Widgets::hint()), m_log(new QPlainTextEdit)
{
    auto *layout = new QVBoxLayout(this);
    auto *status = new QHBoxLayout;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    const QString guest = GuestToolsBuilder::guestDir();
    /* "Mesa 26.2.3", or "Mesa" if its spec has no plain version */
    const auto named = [&guest](const QString &name, const char *package) {
        const QString version = GuestToolsBuilder::version(guest, package);
        return (version.isEmpty() ? name : name + ' ' + version).toHtmlEscaped();
    };

    setObjectName("guestToolsBuild");
    if (!guest.isEmpty()) {
        m_about->setText(
            tr("Vitrine builds the guest tools from its sources: %1 (the graphics driver of "
               "the guest, its settings and the agent), %2 and %3 with Vitrine's patches, in a "
               "Fedora %4 container with podman, then the medium VMs install them from.")
                .arg(named("vitrine-guest-tools", "tools"), named("Mesa", "mesa"),
                     named("KWin", "kwin"), GuestTools::kFedoraRelease));
        m_about->setToolTip(tr("From %1, into %2").arg(guest, GuestTools::dataDir()));
    }
    layout->addWidget(m_about);
    layout->addWidget(m_status);
    layout->addWidget(Widgets::hint(
        tr("The first build takes 20 to 40 minutes and up to %1 GiB of memory, and needs an "
           "internet connection: it downloads Fedora's packages and the sources of Mesa and "
           "KWin. An update builds only the packages whose sources changed.")
            .arg(gib(GuestToolsBuilder::memoryCapMiB()))));
    m_missing->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_missing->hide();
    layout->addWidget(m_missing);
    m_memory->hide();
    layout->addWidget(m_memory);

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
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_log, 1);

    m_build = buttons->addButton(tr("Build"), QDialogButtonBox::ActionRole);
    m_cancel = buttons->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    m_build->setDefault(true);
    /* as QEMU's build window */
    Widgets::setButtonIcon(m_build, Icons::themed({"run-build", "run-build-install"},
                                                  QStyle::SP_BrowserReload));
    Widgets::setButtonIcon(m_cancel, Icons::themed({"process-stop"}, QStyle::SP_BrowserStop));
    m_memory->button()->setText(tr("Build Anyway"));
    m_memory->button()->show();
    m_memory->button()->setToolTip(tr("Start it now: the host may run short of memory, and "
                                      "the kernel stop a program to free some, a VM maybe"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_build, &QPushButton::clicked, this, &GuestToolsBuildDialog::build);
    connect(m_cancel, &QPushButton::clicked, m_builder, &GuestToolsBuilder::cancel);
    connect(m_memory->button(), &QPushButton::clicked, m_builder, &GuestToolsBuilder::proceed);
    connect(m_builder, &GuestToolsBuilder::started, this, &GuestToolsBuildDialog::started);
    connect(m_builder, &GuestToolsBuilder::stepStarted, this, &GuestToolsBuildDialog::showStep);
    connect(m_builder, &GuestToolsBuilder::phaseChanged, this, &GuestToolsBuildDialog::showStep);
    connect(m_builder, &GuestToolsBuilder::waitingChanged, this, [this]() {
        showMemory();
        showStep();
    });
    connect(m_builder, &GuestToolsBuilder::output, this, [this](const QString &text) {
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(text);
        m_log->moveCursor(QTextCursor::End);
    });
    /* busy (0) until told how far the phase is */
    connect(m_builder, &GuestToolsBuilder::progress, this, [this](int done, int total) {
        m_progress->setMaximum(total);
        m_progress->setValue(done);
    });
    connect(m_builder, &GuestToolsBuilder::finished, this, &GuestToolsBuildDialog::finished);
    /* not while QEMU builds */
    connect(StackBuilder::instance(), &StackBuilder::started, this,
            &GuestToolsBuildDialog::updateState);
    connect(StackBuilder::instance(), &StackBuilder::finished, this,
            &GuestToolsBuildDialog::updateState);

    /* a build running already, started from another window: where it is */
    m_log->setPlainText(tail(m_builder->log(), kLogLines));
    m_log->moveCursor(QTextCursor::End);
    if (m_builder->isRunning()) {
        m_progress->setMaximum(m_builder->progressTotal());
        m_progress->setValue(m_builder->progressDone());
        m_progress->show();
        showStep();
    }
    showMissing();
    showMemory();
    updateState();
    resize(760, 620);
}

void GuestToolsBuildDialog::build()
{
    m_log->clear();
    m_step->clear();
    m_builder->start();
    /* refused (what is missing, QEMU's build): told by finished() */
}

void GuestToolsBuildDialog::started()
{
    m_step->setText(tr("Starting…"));
    m_progress->setMaximum(0);
    m_progress->setValue(0);
    m_progress->show();
    showMissing();
    updateState();
}

void GuestToolsBuildDialog::showStep()
{
    if (!m_builder->isRunning()) {
        return;
    }
    /* the bar beside it tells how far */
    QString text = progressText(m_builder, false);
    text[0] = text[0].toUpper();
    m_step->setText(text);
    m_step->show();
}

void GuestToolsBuildDialog::finished(const QString &error)
{
    m_progress->hide();
    if (error.isEmpty()) {
        m_step->setText(tr("Done: %1.").arg(describe()));
    } else if (m_builder->wasStopped()) {
        m_step->setText(tr("Stopped."));
    } else {
        m_step->setText(tr("%1: see the log below.").arg(error));
    }
    showMissing();
    showMemory();
    updateState();
}

void GuestToolsBuildDialog::showMissing()
{
    QStringList missing = m_builder->missingCommands();
    QString install = m_builder->missingInstall();
    QString text;

    if (m_builder->isRunning()) {
        m_missing->hide();
        return;
    }
    /* before a build: what this host lacks now */
    if (missing.isEmpty()) {
        const QList<GuestToolsBuilder::Requirement> needed = GuestToolsBuilder::missing();
        for (const GuestToolsBuilder::Requirement &r : needed) {
            missing << r.command;
        }
        install = GuestToolsBuilder::installCommand(needed);
    }
    if (missing.isEmpty()) {
        m_missing->hide();
        return;
    }
    for (QString &command : missing) {
        command = command == "podman" ? tr("podman, which runs the build in a Fedora container")
                                      : command.toHtmlEscaped();
    }
    text = tr("Missing for the build: %1.").arg(missing.join(", "));
    if (!install.isEmpty()) {
        text += "<br>" + tr("Install with: <code>%1</code>").arg(install.toHtmlEscaped());
    }
    m_missing->setText(text);
    m_missing->show();
}

void GuestToolsBuildDialog::showMemory()
{
    const QList<GuestToolsBuilder::Step> steps = m_builder->steps();
    const QString package = steps.value(m_builder->step() - 1).name == "kwin" ? QStringLiteral("KWin")
                                                                              : QStringLiteral("Mesa");

    if (!m_builder->isWaiting()) {
        m_memory->hide();
        m_progress->setVisible(m_builder->isRunning());
        return;
    }
    /* nothing moves meanwhile */
    m_progress->hide();
    m_memory->setText(tr("<b>Waiting for memory.</b> Building %1 takes up to %2 GiB, and %3 GiB "
                         "is free: close VMs or other programs. The build goes on by itself once "
                         "%4 GiB is free, what the host keeps free included.")
                          .arg(package, gib(GuestToolsBuilder::memoryCapMiB()),
                               gib(m_builder->availableMiB()), gib(m_builder->neededMiB())));
    m_memory->show();
}

void GuestToolsBuildDialog::updateState()
{
    const bool running = m_builder->isRunning();
    const bool qemu = StackBuilder::instance()->isRunning();
    const State state = GuestToolsBuilder::state();
    QString status = "<b>" + describe().toHtmlEscaped() + "</b>";

    if (state != State::NotBuilt) {
        status += "<br>" + explain(state).toHtmlEscaped();
    }
    if (qemu && !running) {
        status += "<br>" + tr("Vitrine's QEMU is being built: the guest tools can be built once "
                              "it is done, as both take a lot of memory.")
                               .toHtmlEscaped();
    }
    setWindowTitle(title(state));
    m_status->setText(status);
    m_build->setText(state == State::Outdated ? tr("Update") : tr("Build"));
    m_build->setEnabled(!running && !qemu && state != State::NoSources);
    m_cancel->setVisible(running);
    m_background->setVisible(running);
    /* no empty line before a build: the step's, under way or done */
    m_step->setVisible(!m_step->text().isEmpty());
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
                [this]() { GuestToolsBuildDialog::present(this); });
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
                   .arg(GuestToolsBuildDialog::progressText(builder).toHtmlEscaped());
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
