// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/snapshots.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/guesttoolsbuildbanner.h"
#include "ui/icons.h"
#include "ui/widgets.h"

using GuestTools::Pending;
using GuestTools::State;

/*
 * How long the dialog waits for the guest to shut down before it gives up
 * restarting the VM (the install stays for its next start): a guest that
 * asks on its screen has the user's answer by then, or the user went
 * elsewhere, and should not see the VM start again whenever it stops later
 */
static int shutdownWaitMs = 5 * 60 * 1000;

void GuestToolsDialog::setShutdownWait(int ms)
{
    shutdownWaitMs = ms;
}

static std::function<void(Vm *)> &starter()
{
    static std::function<void(Vm *)> start;
    return start;
}

void GuestToolsDialog::setStarter(const std::function<void(Vm *)> &start)
{
    starter() = start;
}

GuestToolsDialog *GuestToolsDialog::of(Vm *vm)
{
    for (QWidget *top : QApplication::topLevelWidgets()) {
        auto *dialog = qobject_cast<GuestToolsDialog *>(top);
        if (dialog && vm && dialog->m_vm == vm) {
            return dialog;
        }
    }
    return nullptr;
}

void GuestToolsDialog::stayOff(Vm *vm)
{
    if (GuestToolsDialog *dialog = of(vm); dialog && dialog->m_step == Step::ShuttingDown) {
        dialog->stopWaiting(tr("The VM stays off: the guest tools install at its next start."));
    }
}

void GuestToolsDialog::run(QWidget *parent, Vm *vm)
{
    if (!vm) {
        return;
    }
    if (GuestToolsDialog *open = of(vm)) {
        open->show();
        open->raise();
        open->activateWindow();
        return;
    }
    auto *dialog = new GuestToolsDialog(vm, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    /* not modal: the VM's screen takes its clicks meanwhile, the guest may
       be asking something there */
    dialog->setWindowModality(Qt::NonModal);
    dialog->show();
}

/* No medium: GuestToolsBuildBanner says so, with Build… */
/*
 * A build under way makes the medium again (the Builds window remakes it
 * after any of its packages): installing now would put in the tools it
 * replaces, and offer an update a few minutes later (the user saw Install
 * open during one)
 */
static bool mediumBeingBuilt()
{
    const BuildManager *builds = BuildManager::instance();
    const GuestToolsBuilder *builder = GuestToolsBuilder::instance();

    return (builds && builds->isRunning() &&
            (builds->current() == "medium" || builds->queue().contains("medium"))) ||
           (builder && builder->isRunning());
}

static QString notBuilt()
{
    return GuestToolsDialog::tr("The guest tools are not built yet: build them first.");
}

/* Why the tools cannot go into @vm now; empty if they can */
static QString obstacle(Vm *vm, const GuestTools::Medium &medium)
{
    const ArgsFile &args = vm->args();
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const GuestTools::System system = monitor->system();
    const QString qemu = VmConfig::qemuBinary(args);

    /* what the guest said it runs; else what the VM's settings say, if not
       Linux: before the medium, as a build would not change it */
    if (monitor->unsupported()) {
        return GuestToolsDialog::tr("This guest runs %1, which the guest tools are not for.")
            .arg(system.displayName());
    }
    if (const QString os = VmConfig::guest(args).os;
        !system.isKnown() && !os.isEmpty() && os != "linux") {
        return os == "windows"
                   ? GuestToolsDialog::tr("This VM is set up for Windows.")
                   : GuestToolsDialog::tr("This VM is set up for another system than Linux.");
    }
    if (!medium.isValid()) {
        return notBuilt();
    }
    if (!GuestTools::canBootstrap(args, qemu.isEmpty() ? "qemu-system-x86_64" : qemu)) {
        return GuestToolsDialog::tr("The guest tools are for x86-64 Fedora guests, on a "
                                    "machine with PCI.");
    }
    if (VmConfig::firmwareKind(args) == VmConfig::FirmwareKind::UefiSecureBoot ||
        monitor->report().secureBoot.value_or(false)) {
        return GuestToolsDialog::tr(
            "This VM boots with Secure Boot. The graphics driver of the guest tools is not "
            "signed, so the guest would be left with no graphics driver at all: turn Secure "
            "Boot off in the VM's firmware settings first.");
    }
    if (vm->runner()->state() == VmRunner::State::Paused) {
        return GuestToolsDialog::tr("The VM is paused: resume it first.");
    }
    if (vm->runner()->state() == VmRunner::State::Starting ||
        vm->runner()->state() == VmRunner::State::Stopping) {
        return GuestToolsDialog::tr("Wait until the VM has started or stopped.");
    }
    return {};
}

/* Whom the tools are for, and what @vm's guest runs, as far as it said */
static QString forWhom(Vm *vm, const GuestTools::Medium &medium)
{
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const QString text =
        GuestToolsDialog::tr("The guest tools are for %1 guests: they make them run well in "
                             "vitrine.")
            .arg(GuestTools::supportedSystems(medium).toHtmlEscaped());

    /* another system: the warning under it says so */
    if (monitor->unsupported()) {
        return text;
    }
    if (monitor->system().isKnown()) {
        return text + ' ' +
               GuestToolsDialog::tr("This guest runs %1.")
                   .arg(monitor->system().displayName().toHtmlEscaped());
    }
    return text + ' ' +
           GuestToolsDialog::tr("What this guest runs is not known yet: QEMU's guest agent "
                                "(qemu-guest-agent) tells vitrine while the VM runs.");
}

/* What the tools of @medium install in @vm, and how */
static QString what(Vm *vm, const GuestTools::Medium &medium)
{
    const bool running = vm->runner()->isActive();
    QString what;

    what = "<p>" + forWhom(vm, medium) + "</p>";
    what += GuestToolsDialog::tr("<p>They install:</p><ul>");
    what += GuestToolsDialog::tr("<li>vitrine's virtio-gpu driver, built again for each new "
                                 "kernel (DKMS), with its settings</li>");
    if (!medium.mesa.isEmpty()) {
        what += GuestToolsDialog::tr("<li>Mesa %1 with native context, in place of the guest's</li>")
                    .arg(medium.mesa.toHtmlEscaped());
    }
    const QString desktop = VmConfig::guest(vm->args()).desktop;
    if (!medium.kwin.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += GuestToolsDialog::tr("<li>KWin %1, if the guest's Plasma is %2</li>")
                    .arg(medium.kwin.toHtmlEscaped(), medium.kwin.section('-', 0, 0));
    }
    if (!medium.kpipewire.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += GuestToolsDialog::tr("<li>KPipeWire %1 (Plasma's window thumbnails), if the "
                                     "guest's Plasma is %2</li>")
                    .arg(medium.kpipewire.toHtmlEscaped(), medium.kpipewire.section('-', 0, 0));
    }
    if (!medium.proxy.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += GuestToolsDialog::tr("<li>the nested desktop, which the VM's Nested desktop "
                                     "setting turns on: Plasma as a window of this computer's "
                                     "desktop</li>");
    }
    what += GuestToolsDialog::tr("<li>the vitrine agent, which tells vitrine how the guest is "
                                 "doing</li></ul>");
    what += GuestToolsDialog::tr("<p>Mesa, KWin and KPipeWire then stay at these versions until "
                                 "the next guest tools. The guest needs the network: dkms and the "
                                 "kernel headers come from Fedora.</p>");
    what += running ? GuestToolsDialog::tr("<p><b>vitrine shuts the VM down and starts it again"
                                           "</b> with the tools medium. The guest installs them "
                                           "before its desktop starts, which takes a few "
                                           "minutes, then restarts once more.</p>")
                    : GuestToolsDialog::tr("<p>The VM starts with the tools medium. The guest "
                                           "installs them before its desktop starts, which takes "
                                           "a few minutes, then restarts once more.</p>");
    return what;
}

GuestToolsDialog::GuestToolsDialog(Vm *vm, QWidget *parent)
    : QDialog(parent), m_vm(vm), m_what(Widgets::note()), m_warning(new Banner(Banner::Warning)),
      m_progress(Widgets::note()),
      m_snapshot(new QCheckBox(tr("Take a snapshot of the disks first"))),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Cancel)), m_wait(new QTimer(this))
{
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const bool update = !monitor->report().tools.isEmpty();
    auto *layout = new QVBoxLayout(this);
    auto *build = new GuestToolsBuildBanner;

    setWindowTitle(update ? tr("Update Guest Tools") : tr("Install Guest Tools"));
    layout->addWidget(m_what);
    /* the medium: not built yet, being built, out of date, with Build… or Update… */
    layout->addWidget(build);
    /* what else keeps the tools from the VM (refresh()) */
    layout->addWidget(m_warning);

    /* the disks of a VM that writes to qcow2 files can go back to before */
    bool canSnapshot = false;
    for (const VmSnapshots::Drive &d : VmSnapshots::drives(vm->args(), vm->dir())) {
        canSnapshot |= d.canSnapshot() && !d.pflash;
    }
    m_snapshot->setChecked(canSnapshot);
    m_snapshot->setEnabled(canSnapshot);
    m_snapshot->setToolTip(canSnapshot ? tr("The VM can go back to it from its Snapshots tab")
                                       : tr("The VM's disks are not qcow2 files"));
    layout->addWidget(m_snapshot);
    layout->addWidget(m_progress);
    m_progress->hide();
    /* room to spare under the text, not between its lines */
    layout->addStretch();

    m_install = m_buttons->addButton(tr("&Install"), QDialogButtonBox::AcceptRole);
    m_mediumButton = m_buttons->addButton(tr("Attach &Medium Only"),
                                          QDialogButtonBox::ActionRole);
    m_mediumButton->setToolTip(
        tr("The VM starts with the tools medium only: install them by hand in the guest with\n"
           "sudo bash /run/media/$USER/%1/install")
            .arg(GuestTools::kLabel));
    Widgets::setButtonIcon(m_install, Icons::themed({"run-build-install", "system-software-install"},
                                                    QStyle::SP_DialogApplyButton));
    Widgets::setButtonIcon(m_mediumButton, Icons::themed({"media-optical", "drive-optical"},
                                                         QStyle::SP_DriveCDIcon));
    layout->addWidget(m_buttons);
    connect(m_install, &QPushButton::clicked, this, [this]() { go(false); });
    connect(m_mediumButton, &QPushButton::clicked, this, [this]() { go(true); });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    /* not modal: the VM may start, stop, change or go meanwhile */
    connect(vm->runner(), &VmRunner::stateChanged, this, [this](VmRunner::State state) {
        if (m_step != Step::ShuttingDown) {
            refresh();
        } else if (state == VmRunner::State::Stopped) {
            m_wait->stop();
            /* QEMU stopped by itself (a crash) is no shutdown to restart after */
            if (const QString error = m_vm->runner()->errorString(); !error.isEmpty()) {
                stopWaiting(tr("The VM stopped with an error: the guest tools install at its "
                               "next start."));
            } else {
                next();
            }
        }
    });
    connect(vm, &Vm::changed, this, &GuestToolsDialog::refresh);
    connect(vm, &QObject::destroyed, this, &QDialog::reject);
    m_wait->setSingleShot(true);
    connect(m_wait, &QTimer::timeout, this, [this]() {
        if (m_step == Step::ShuttingDown) {
            stopWaiting(tr("The guest did not shut down: the guest tools install at the VM's "
                           "next start."));
        }
    });
    /* a build made the medium meanwhile */
    connect(build, &GuestToolsBuildBanner::mediumChanged, this, &GuestToolsDialog::refresh);
    /* a build starting or ending: Install waits for it, or goes on once it is done */
    if (BuildManager *builds = BuildManager::instance()) {
        connect(builds, &BuildManager::runStarted, this, &GuestToolsDialog::refresh);
        connect(builds, &BuildManager::runFinished, this, &GuestToolsDialog::buildDone);
    }
    if (GuestToolsBuilder *builder = GuestToolsBuilder::instance()) {
        connect(builder, &GuestToolsBuilder::finished, this, &GuestToolsDialog::buildDone);
    }
    refresh();
    setMinimumWidth(Widgets::em(this) * 34);
    Widgets::resizeToWidth(this, sizeHint().width());
}

void GuestToolsDialog::refresh()
{
    if (!m_vm || m_step != Step::Idle) {
        return;
    }
    const bool running = m_vm->runner()->isActive();
    const GuestTools::Medium medium = GuestTools::medium();
    const QString problem = obstacle(m_vm, medium);

    m_what->setText(what(m_vm, medium));
    /* the medium missing: the build banner tells it */
    m_warning->setText(problem.toHtmlEscaped());
    m_warning->setVisible(!problem.isEmpty() && problem != notBuilt());
    const bool building = mediumBeingBuilt();

    if (building) {
        m_install->setText(running ? tr("&Restart and Install When Built")
                                   : tr("&Install When Built"));
    } else {
        m_install->setText(running ? tr("&Restart and Install") : tr("&Install"));
    }
    /* no medium yet, but one on its way: Install waits for it too */
    m_install->setEnabled(problem.isEmpty() || (building && problem == notBuilt()));
    m_mediumButton->setEnabled(problem.isEmpty() && !building);
    if (isVisible()) {
        fit();
    }
}

void GuestToolsDialog::buildDone(const QString &error)
{
    if (!m_vm) {
        return;
    }
    if (m_step != Step::WaitingForBuild) {
        refresh();
        return;
    }
    if (!error.isEmpty()) {
        stopWaiting(tr("The build did not end well (%1): nothing was installed.").arg(error));
        return;
    }
    /* a run of the host only, or a part of the run: the medium is still to come */
    if (mediumBeingBuilt()) {
        return;
    }
    m_step = Step::Idle;
    if (const QString problem = obstacle(m_vm, GuestTools::medium()); !problem.isEmpty()) {
        stopWaiting(problem);
        return;
    }
    refresh();
    go(m_mediumOnly);
}

void GuestToolsDialog::fit()
{
    Widgets::resizeToWidth(this, width());
}

void GuestToolsDialog::reject()
{
    switch (m_step) {
    case Step::WaitingForBuild:
        /* nothing asked of the VM yet */
        m_step = Step::Idle;
        break;
    case Step::ShuttingDown:
        /* Cancel, Escape, the window's X or the banner's Cancel: no install
           after all, nor restart (the guest was asked already) */
        m_step = Step::Idle;
        m_wait->stop();
        if (m_vm) {
            GuestToolsMonitor::of(m_vm)->setPending(Pending::None);
        }
        break;
    case Step::Snapshot:
        /* not while qemu-img writes it: the VM stays off once it is written */
        m_cancelled = true;
        if (m_vm) {
            GuestToolsMonitor::of(m_vm)->setPending(Pending::None);
        }
        setProgress(tr("The snapshot is being written; then the VM stays off."));
        return;
    case Step::Starting:
        return;
    case Step::Idle:
    case Step::Done:
        break;
    }
    QDialog::reject();
}

void GuestToolsDialog::stopWaiting(const QString &why)
{
    m_step = Step::Done;
    m_wait->stop();
    if (isHidden()) {
        /* out of sight all along: nothing to say, the banner says what is pending */
        QDialog::reject();
        return;
    }
    setProgress(why);
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("&Close"));
}

void GuestToolsDialog::setProgress(const QString &text)
{
    m_progress->setText(text);
    m_progress->show();
    /* the line it adds, not taken from the notes above */
    if (isVisible()) {
        fit();
    }
}

void GuestToolsDialog::go(bool medium)
{
    if (!m_vm) {
        return;
    }
    m_mediumOnly = medium;
    m_install->setEnabled(false);
    m_mediumButton->setEnabled(false);
    m_snapshot->setEnabled(false);
    if (mediumBeingBuilt()) {
        m_step = Step::WaitingForBuild;
        setProgress(tr("The guest tools install as soon as the build under way is done."));
        return;
    }
    GuestToolsMonitor::of(m_vm)->setPending(medium ? Pending::Medium : Pending::Bootstrap);
    if (m_vm->runner()->isActive()) {
        m_step = Step::ShuttingDown;
        setProgress(tr("Waiting for the guest to shut down. If it asks what to do, answer it "
                       "on its screen: the VM then starts again with the tools medium."));
        m_wait->start(shutdownWaitMs);
        /*
         * Out of the way of the VM's screen, where the guest may ask what
         * to do (Plasma answers the power button with its logout screen),
         * for as long as it takes: the status bar says how the guest was
         * asked, and Install Guest Tools brings this back.  The gentlest
         * way the guest takes (VmRunner::powerdown()).
         */
        hide();
        m_vm->runner()->powerdown();
        return;
    }
    next();
}

void GuestToolsDialog::next()
{
    if (!m_vm) {
        reject();
        return;
    }
    if (m_step < Step::Snapshot && m_snapshot->isChecked()) {
        const QString name =
            tr("Before guest tools %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm"));

        m_step = Step::Snapshot;
        setProgress(tr("Taking the snapshot “%1”…").arg(name.toHtmlEscaped()));
        m_snapshots = new VmSnapshots(m_vm->runner(), this);
        m_snapshots->setVm(m_vm->args(), m_vm->dir());
        connect(m_snapshots, &VmSnapshots::finished, this, [this](const QString &error) {
            if (m_cancelled) {
                m_step = Step::Done;
                QDialog::reject();
                return;
            }
            if (!error.isEmpty()) {
                fail(tr("The snapshot failed: %1").arg(VmSnapshots::explain(error)));
                return;
            }
            next();
        });
        m_snapshots->take(name);
        return;
    }
    m_step = Step::Starting;
    setProgress(tr("Starting the VM…"));
    if (starter()) {
        starter()(m_vm);
    } else {
        m_vm->runner()->start(m_vm->args());
    }
    accept();
}

void GuestToolsDialog::fail(const QString &error)
{
    m_step = Step::Done;
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("&Close"));
    /* hidden since its guest shut down: back with the error, the VM off and
       its screen gone (KWin activates it, WA_ShowWithoutActivating or not) */
    if (isHidden()) {
        show();
    }
    setProgress(
        QString("<b>%1</b><br>%2")
            .arg(error.toHtmlEscaped(),
                 tr("The VM is off; the guest tools install at its next start.").toHtmlEscaped()));
}

GuestToolsBanner::GuestToolsBanner(QWidget *parent)
    : QWidget(parent), m_info(new Banner(Banner::Information)),
      m_warning(new Banner(Banner::Warning))
{
    auto *layout = new QVBoxLayout(this);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_info);
    layout->addWidget(m_warning);
    for (Banner *b : {m_info, m_warning}) {
        connect(b->button(), &QPushButton::clicked, this, [this]() {
            if (!m_vm) {
                return;
            }
            GuestToolsMonitor *monitor = GuestToolsMonitor::of(m_vm);
            switch (monitor->state()) {
            case State::Pending:
                monitor->setPending(Pending::None);
                /* nor the restart its dialog waits to make, out of sight */
                if (GuestToolsDialog *waiting = GuestToolsDialog::of(m_vm)) {
                    waiting->reject();
                }
                break;
            case State::RebootNeeded:
                monitor->requestReboot();
                break;
            default:
                GuestToolsDialog::run(window(), m_vm);
            }
        });
    }
    hide();
}

void GuestToolsBanner::setVm(Vm *vm)
{
    if (m_vm == vm) {
        return;
    }
    if (m_vm) {
        disconnect(GuestToolsMonitor::of(m_vm), nullptr, this, nullptr);
        disconnect(m_vm, nullptr, this, nullptr);
    }
    m_vm = vm;
    if (vm) {
        connect(GuestToolsMonitor::of(vm), &GuestToolsMonitor::changed, this,
                &GuestToolsBanner::update);
        /* its settings: the 3D card the tools' driver is for (offered()) */
        connect(vm, &Vm::changed, this, &GuestToolsBanner::update);
    }
    update();
}

/*
 * The tools are offered to @vm: its guest is known to run a system they are
 * for (it said so, through an agent, when it last ran), with the 3D card
 * their driver is for.  Not while what it runs is not known.
 */
static bool offered(Vm *vm)
{
    return GuestToolsMonitor::of(vm)->supported() &&
           VmConfig::graphics(vm->args()).kind == VmConfig::Graphics::Accelerated;
}

void GuestToolsBanner::update()
{
    if (!m_vm) {
        hide();
        return;
    }
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(m_vm);
    const State state = monitor->state();
    const QString text = monitor->text();
    const GuestTools::Inputs in = monitor->inputs();
    /* a guest known to run a system the tools are not for: nothing to install */
    const bool other = monitor->unsupported();
    /* the install asked for at this start did not happen: said, offered or not */
    const bool asked = state == State::NotInstalled && in.running && in.bootstrapRun;
    QString button;
    bool warning = false;

    switch (state) {
    case State::NotInstalled:
        button = other ? QString() : tr("Install Guest Tools…");
        warning = asked && other;
        break;
    case State::Pending:
        button = tr("Cancel");
        break;
    case State::Failed:
        button = other ? QString() : tr("Try Again…");
        warning = true;
        break;
    case State::RebootNeeded:
        button = monitor->hasAgent() ? tr("Restart Guest") : QString();
        break;
    case State::DriverNotActive:
    case State::MesaNotActive:
        button = other ? QString() : tr("Install Again…");
        warning = true;
        break;
    case State::UpdateAvailable:
        button = tr("Update…");
        break;
    case State::Unsupported:
        warning = true;
        break;
    default:
        break;
    }
    /* the offers: to install them, to update them */
    if (text.isEmpty() || (state == State::NotInstalled && !asked && !offered(m_vm)) ||
        (state == State::UpdateAvailable && other)) {
        hide();
        return;
    }
    Banner *shown = warning ? m_warning : m_info;
    shown->setText(text);
    shown->button()->setText(button);
    shown->button()->setVisible(!button.isEmpty());
    m_info->setVisible(!warning);
    m_warning->setVisible(warning);
    show();
}
