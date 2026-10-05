// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/guesttools.h"
#include "core/snapshots.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/icons.h"
#include "ui/widgets.h"

using GuestTools::Pending;
using GuestTools::State;

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

/* Why the tools cannot go into @vm now; empty if they can */
static QString obstacle(Vm *vm, const GuestTools::Medium &medium)
{
    const ArgsFile &args = vm->args();
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const QString qemu = VmConfig::qemuBinary(args);

    if (!medium.isValid()) {
        return GuestToolsDialog::tr(
            "The guest tools are not built yet. Build them in vitrine's sources with "
            "guest/build-rpms.sh, then guest/build-medium.sh (about 35 minutes).");
    }
    if (const QString os = VmConfig::guest(args).os; !os.isEmpty() && os != "linux") {
        return GuestToolsDialog::tr("The guest tools are for Fedora Linux guests.");
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

GuestToolsDialog::GuestToolsDialog(Vm *vm, QWidget *parent)
    : QDialog(parent), m_vm(vm), m_restart(Widgets::note()), m_warning(new Banner(Banner::Warning)),
      m_progress(Widgets::note()),
      m_snapshot(new QCheckBox(tr("Take a snapshot of the disks first"))),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Cancel))
{
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const GuestTools::Medium medium = GuestTools::medium();
    const bool update = !monitor->report().tools.isEmpty();
    auto *layout = new QVBoxLayout(this);
    QString what;

    setWindowTitle(update ? tr("Update Guest Tools") : tr("Install Guest Tools"));
    what = tr("<p>The guest tools make a Fedora %1 guest run well in vitrine. They "
              "install:</p><ul>")
               .arg(GuestTools::kFedoraRelease);
    what += tr("<li>vitrine's virtio-gpu driver, built again for each new kernel (DKMS), with "
               "its settings</li>");
    if (!medium.mesa.isEmpty()) {
        what += tr("<li>Mesa %1 with native context, in place of the guest's</li>")
                    .arg(medium.mesa.toHtmlEscaped());
    }
    const QString desktop = VmConfig::guest(vm->args()).desktop;
    if (!medium.kwin.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += tr("<li>KWin %1, if the guest's Plasma is %2</li>")
                    .arg(medium.kwin.toHtmlEscaped(), medium.kwin.section('-', 0, 0));
    }
    what += tr("<li>the vitrine agent, which tells vitrine how the guest is doing</li></ul>");
    what += tr("<p>Mesa and KWin then stay at these versions until the next guest tools. "
               "The guest needs the network: dkms and the kernel headers come from Fedora.</p>");
    layout->addWidget(Widgets::note(what));
    /* what the VM goes through, and what keeps it from it (refresh()) */
    layout->addWidget(m_restart);
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
    /* Cancel, Escape or the window's X while the guest shuts down: no
       install after all, nor restart (the guest was asked already) */
    connect(this, &QDialog::rejected, this, [this]() {
        if (m_vm && m_step == Step::ShuttingDown) {
            m_step = Step::Idle;
            GuestToolsMonitor::of(m_vm)->setPending(Pending::None);
        }
    });

    /* not modal: the VM may start, stop, change or go meanwhile */
    connect(vm->runner(), &VmRunner::stateChanged, this, &GuestToolsDialog::refresh);
    connect(vm, &Vm::changed, this, &GuestToolsDialog::refresh);
    connect(vm, &QObject::destroyed, this, &QDialog::reject);
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
    const QString problem = obstacle(m_vm, GuestTools::medium());

    m_restart->setText(running ? tr("<b>vitrine shuts the VM down and starts it again</b> "
                                    "with the tools medium. The guest installs them before its "
                                    "desktop starts, which takes a few minutes, then restarts "
                                    "once more.")
                               : tr("The VM starts with the tools medium. The guest installs "
                                    "them before its desktop starts, which takes a few minutes, "
                                    "then restarts once more."));
    m_warning->setText(problem.toHtmlEscaped());
    m_warning->setVisible(!problem.isEmpty());
    m_install->setText(running ? tr("&Restart and Install") : tr("&Install"));
    m_install->setEnabled(problem.isEmpty());
    m_mediumButton->setEnabled(problem.isEmpty());
    if (isVisible()) {
        fit();
    }
}

void GuestToolsDialog::fit()
{
    Widgets::resizeToWidth(this, width());
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
    GuestToolsMonitor::of(m_vm)->setPending(medium ? Pending::Medium : Pending::Bootstrap);
    if (m_vm->runner()->isActive()) {
        m_step = Step::ShuttingDown;
        setProgress(tr("Waiting for the guest to shut down. If it asks what to do, answer it "
                       "on its screen: the VM then starts again with the tools medium."));
        connect(m_vm->runner(), &VmRunner::stateChanged, this, [this](VmRunner::State s) {
            if (s == VmRunner::State::Stopped && m_step == Step::ShuttingDown) {
                next();
            }
        });
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
    /* as chosen here: a report of the guest's tools while it shut down
       may have taken it back (GuestToolsMonitor, for tools already in) */
    GuestToolsMonitor::of(m_vm)->setPending(m_mediumOnly ? Pending::Medium : Pending::Bootstrap);
    if (starter()) {
        starter()(m_vm);
    } else {
        m_vm->runner()->start(m_vm->args());
    }
    accept();
}

void GuestToolsDialog::fail(const QString &error)
{
    m_step = Step::Idle;
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("&Close"));
    /* hidden since its guest shut down; the VM is off, its screen gone:
       back, without taking the keys from where they go */
    if (isHidden()) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        show();
        setAttribute(Qt::WA_ShowWithoutActivating, false);
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
    }
    m_vm = vm;
    if (vm) {
        connect(GuestToolsMonitor::of(vm), &GuestToolsMonitor::changed, this,
                &GuestToolsBanner::update);
    }
    update();
}

/* A guest the tools are for, as far as its settings tell: Linux (the
   #guest directive) with an accelerated virtio GPU; without the directive,
   not Windows (Hyper-V enlightenments) */
static bool toolsGuest(const ArgsFile &args)
{
    const QString os = VmConfig::guest(args).os;
    const QString text = args.toText();

    if (VmConfig::graphics(args).kind != VmConfig::Graphics::Accelerated) {
        return false;
    }
    return os.isEmpty() ? !text.contains("hv-relaxed") && !text.contains("hv_relaxed")
                        : os == "linux";
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
    QString button;
    bool warning = false;

    switch (state) {
    case State::NotInstalled:
        button = tr("Install Guest Tools…");
        break;
    case State::Pending:
        button = tr("Cancel");
        break;
    case State::Failed:
        button = tr("Try Again…");
        warning = true;
        break;
    case State::RebootNeeded:
        button = monitor->hasAgent() ? tr("Restart Guest") : QString();
        break;
    case State::DriverNotActive:
    case State::MesaNotActive:
        button = tr("Install Again…");
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
    if (text.isEmpty() || (state == State::NotInstalled && !toolsGuest(m_vm->args()))) {
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
