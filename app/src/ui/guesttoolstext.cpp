// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolstext.h"

#include <QCoreApplication>

#include "core/buildmanager.h"
#include "core/guesttoolsbuilder.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

namespace GuestToolsText {

/* the dialog's words, as before */
static QString tr(const char *text)
{
    return QCoreApplication::translate("GuestToolsDialog", text);
}

/* No medium: GuestToolsBuildBanner says so, with Build… */
/*
 * A build under way makes the medium again (the Builds window remakes it
 * after any of its packages): installing now would put in the tools it
 * replaces, and offer an update a few minutes later (the user saw Install
 * open during one)
 */
bool mediumBeingBuilt(const QString &release)
{
    const BuildManager *builds = BuildManager::instance();
    const GuestToolsBuilder *builder = GuestToolsBuilder::instance();

    /* of the guest's release: another release's medium is not the one it gets */
    return (builds && builds->isRunning() && builds->guestRelease() == release &&
            (builds->current() == "medium" || builds->queue().contains("medium"))) ||
           (builder && builder->isRunning() && builder->runRelease() == release);
}

/* The release whose tools @vm gets, and its medium if built */
QString toolsRelease(Vm *vm)
{
    return GuestToolsMonitor::of(vm)->release();
}

GuestTools::Medium mediumOf(Vm *vm)
{
    const QString release = toolsRelease(vm);
    return release.isEmpty() ? GuestTools::Medium() : GuestTools::medium(release);
}

QString notBuilt()
{
    return tr("The guest tools are not built yet: build them first.");
}

/* Why the tools cannot go into @vm now; empty if they can */
QString obstacle(Vm *vm, const GuestTools::Medium &medium)
{
    const ArgsFile &args = vm->args();
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const GuestTools::System system = monitor->system();
    const QString qemu = VmConfig::qemuBinary(args);

    /* what the guest said it runs; else what the VM's settings say, if not
       Linux: before the medium, as a build would not change it */
    if (monitor->unsupported()) {
        return tr("This guest runs %1, which the guest tools are not for.")
            .arg(system.displayName());
    }
    if (const QString os = VmConfig::guest(args).os;
        !system.isKnown() && !os.isEmpty() && os != "linux") {
        return os == "windows"
                   ? tr("This VM is set up for Windows.")
                   : tr("This VM is set up for another system than Linux.");
    }
    if (!medium.isValid()) {
        return notBuilt();
    }
    if (!GuestTools::canBootstrap(args, qemu.isEmpty() ? "qemu-system-x86_64" : qemu)) {
        return tr("The guest tools are for x86-64 Fedora guests, on a "
                                    "machine with PCI.");
    }
    if (VmConfig::firmwareKind(args) == VmConfig::FirmwareKind::UefiSecureBoot ||
        monitor->report().secureBoot.value_or(false)) {
        return tr(
            "This VM boots with Secure Boot. The graphics driver of the guest tools is not "
            "signed, so the guest would be left with no graphics driver at all: turn Secure "
            "Boot off in the VM's firmware settings first.");
    }
    if (vm->runner()->state() == VmRunner::State::Paused) {
        return tr("The VM is paused: resume it first.");
    }
    if (vm->runner()->state() == VmRunner::State::Starting ||
        vm->runner()->state() == VmRunner::State::Stopping) {
        return tr("Wait until the VM has started or stopped.");
    }
    return {};
}

/* Whom the tools are for, and what @vm's guest runs, as far as it said */
static QString forWhom(Vm *vm)
{
    const GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
    const QString text =
        tr("The guest tools are for %1 guests: they make them run well in "
                             "vitrine.")
            .arg(GuestTools::supportedSystems().toHtmlEscaped());

    /* another system: the warning under it says so */
    if (monitor->unsupported()) {
        return text;
    }
    if (monitor->system().isKnown()) {
        return text + ' ' +
               tr("This guest runs %1.")
                   .arg(monitor->system().displayName().toHtmlEscaped());
    }
    return text + ' ' +
           tr("What this guest runs is not known yet: QEMU's guest agent "
                                "(qemu-guest-agent) tells vitrine while the VM runs.");
}

/* What the tools of @medium install in @vm, and how */
QString what(Vm *vm, const GuestTools::Medium &medium)
{
    const bool running = vm->runner()->isActive();
    QString what;

    what = "<p>" + forWhom(vm) + "</p>";
    what += tr("<p>They install:</p><ul>");
    what += tr("<li>vitrine's virtio-gpu driver, built again for each new "
                                 "kernel (DKMS), with its settings</li>");
    if (!medium.mesa.isEmpty()) {
        what += tr("<li>Mesa %1 with native context, in place of the guest's</li>")
                    .arg(medium.mesa.toHtmlEscaped());
    }
    const QString desktop = VmConfig::guest(vm->args()).desktop;
    if (!medium.kwin.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += tr("<li>KWin %1 for Plasma %2; a guest with Fedora's older "
                                     "Plasma gets Fedora's Plasma %2 with it, in the same "
                                     "update</li>")
                    .arg(medium.kwin.toHtmlEscaped(), medium.kwin.section('-', 0, 0));
    }
    if (!medium.kpipewire.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += tr("<li>KPipeWire %1 (Plasma's window thumbnails) for "
                                     "Plasma %2</li>")
                    .arg(medium.kpipewire.toHtmlEscaped(), medium.kpipewire.section('-', 0, 0));
    }
    if (!medium.proxy.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += tr("<li>the nested desktop, which the VM's Nested desktop "
                                     "setting turns on: Plasma as a window of this computer's "
                                     "desktop</li>");
    }
    /* in place of the guest's while it has their versions */
    if (!medium.qtbase.isEmpty()) {
        what += tr("<li>Qt Base %1 (Wayland windows being resized reuse their "
                                     "memory), if the guest's Qt is %2</li>")
                    .arg(medium.qtbase.toHtmlEscaped(), medium.qtbase.section('-', 0, 0));
    }
    QStringList frameworks;
    if (!medium.ksvg.isEmpty()) {
        frameworks << tr("KSvg %1").arg(medium.ksvg.toHtmlEscaped());
    }
    if (!medium.kirigami.isEmpty()) {
        frameworks << tr("Kirigami %1").arg(medium.kirigami.toHtmlEscaped());
    }
    if (!frameworks.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
        what += tr("<li>%1 (Plasma's popups open without uploading their "
                                     "frames again), if the guest's frameworks are those "
                                     "versions</li>")
                    .arg(frameworks.join(tr(" and ")));
    }
    what += tr("<li>the vitrine agent, which tells vitrine how the guest is "
                                 "doing</li></ul>");
    /* what stays at its version: of what the medium has (Fedora 45's has no KDE packages) */
    const bool kdePackages = !medium.kwin.isEmpty() || !medium.kpipewire.isEmpty() ||
                             !medium.qtbase.isEmpty() || !medium.ksvg.isEmpty() ||
                             !medium.kirigami.isEmpty();
    if (kdePackages) {
        what += tr("<p>Mesa, KWin and KPipeWire then stay at these versions "
                                     "until the next guest tools, and so do Qt Base and the "
                                     "guest's Plasma with vitrine's KWin. "
                                     "KSvg, Kirigami and Qt Base otherwise give way to Fedora's "
                                     "next build of them. The guest needs the network: dkms, the "
                                     "kernel headers and a newer Plasma come from Fedora.</p>");
    } else if (!medium.mesa.isEmpty()) {
        what += tr("<p>Mesa then stays at this version until the next guest "
                                     "tools. The guest needs the network: dkms and the kernel "
                                     "headers come from Fedora.</p>");
    } else {
        what += tr("<p>The guest needs the network: dkms and the kernel headers "
                                     "come from Fedora.</p>");
    }
    what += running ? tr("<p><b>vitrine shuts the VM down and starts it again"
                                           "</b> with the tools medium. The guest installs them "
                                           "before its desktop starts, which takes a few "
                                           "minutes, then restarts once more.</p>")
                    : tr("<p>The VM starts with the tools medium. The guest "
                                           "installs them before its desktop starts, which takes "
                                           "a few minutes, then restarts once more.</p>");
    return what;
}

}
