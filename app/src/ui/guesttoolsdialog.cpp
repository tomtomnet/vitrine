// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsdialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "core/guesttools.h"
#include "core/snapshots.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/widgets.h"

using GuestTools::Pending;
using GuestTools::State;

/* The guest's shutdown, before vitrine offers to force the VM off */
static const int kShutdownMs = 120000;

static std::function<void(Vm *)> &starter()
{
    static std::function<void(Vm *)> start;
    return start;
}

void GuestToolsDialog::setStarter(const std::function<void(Vm *)> &start)
{
    starter() = start;
}

void GuestToolsDialog::run(QWidget *parent, Vm *vm)
{
    if (!vm) {
        return;
    }
    auto *dialog = new GuestToolsDialog(vm, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
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
    : QDialog(parent), m_vm(vm), m_progress(Widgets::note()),
      m_snapshot(new QCheckBox(tr("Take a snapshot of the disks first"))),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Cancel)), m_timeout(new QTimer(this))
{
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const GuestTools::Medium medium = GuestTools::medium();
    const bool update = !monitor->report().tools.isEmpty();
    const bool running = vm->runner()->isActive();
    const QString problem = obstacle(vm, medium);
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
    what += running ? tr("<p><b>vitrine shuts the VM down and starts it again</b> with the "
                         "tools medium. The guest installs them before its desktop starts, "
                         "which takes a few minutes, then restarts once more.</p>")
                    : tr("<p>The VM starts with the tools medium. The guest installs them "
                         "before its desktop starts, which takes a few minutes, then restarts "
                         "once more.</p>");
    layout->addWidget(Widgets::note(what));

    if (!problem.isEmpty()) {
        auto *warning = new Banner(Banner::Warning);
        warning->setText(problem.toHtmlEscaped());
        layout->addWidget(warning);
    }

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

    m_install = m_buttons->addButton(running ? tr("&Restart and Install") : tr("&Install"),
                                     QDialogButtonBox::AcceptRole);
    m_mediumButton = m_buttons->addButton(tr("Attach &Medium Only"),
                                          QDialogButtonBox::ActionRole);
    m_mediumButton->setToolTip(
        tr("The VM starts with the tools medium only: install them by hand in the guest with\n"
           "sudo bash /run/media/$USER/%1/install")
            .arg(GuestTools::kLabel));
    m_install->setEnabled(problem.isEmpty());
    m_mediumButton->setEnabled(problem.isEmpty());
    layout->addWidget(m_buttons);
    connect(m_install, &QPushButton::clicked, this, [this]() { go(false); });
    connect(m_mediumButton, &QPushButton::clicked, this, [this]() { go(true); });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        if (!m_vm || m_step != Step::ShuttingDown) {
            return;
        }
        if (Widgets::confirm(this, QMessageBox::Warning, tr("Force Off %1?").arg(m_vm->name()),
                             tr("The guest has not shut down. Force the VM off? The guest "
                                "loses its unsaved work."),
                             tr("&Force Off"))) {
            m_vm->runner()->forceOff();
        } else {
            m_timeout->start(kShutdownMs);
        }
    });
    setMinimumWidth(Widgets::em(this) * 34);
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
    m_progress->show();
    GuestToolsMonitor::of(m_vm)->setPending(medium ? Pending::Medium : Pending::Bootstrap);
    if (m_vm->runner()->isActive()) {
        m_step = Step::ShuttingDown;
        m_progress->setText(tr("Shutting the guest down…"));
        connect(m_vm->runner(), &VmRunner::stateChanged, this, [this](VmRunner::State s) {
            if (s == VmRunner::State::Stopped && m_step == Step::ShuttingDown) {
                m_timeout->stop();
                next();
            }
        });
        /* through the agent if the guest has one (VmRunner's shutdown handler) */
        m_vm->runner()->powerdown();
        m_timeout->start(kShutdownMs);
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
        m_progress->setText(tr("Taking the snapshot “%1”…").arg(name.toHtmlEscaped()));
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
    m_progress->setText(tr("Starting the VM…"));
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
    m_progress->setText(
        QString("<b>%1</b><br>%2")
            .arg(error.toHtmlEscaped(),
                 tr("The VM is off; the guest tools install at its next start.").toHtmlEscaped()));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("&Close"));
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
