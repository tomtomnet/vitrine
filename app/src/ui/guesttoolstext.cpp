// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolstext.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>

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
        QString runs = text + ' ' +
                       tr("This guest runs %1.")
                           .arg(monitor->system().displayName().toHtmlEscaped());
        /* vitrine's Mutter or Fedora's: what the user's own test of it needs to know */
        if (const QString mutter = runningMutter(monitor->report()); !mutter.isEmpty()) {
            runs += ' ' + mutter;
        }
        /* the locked builds Fedora has moved past: a newer vitrine ports them */
        if (const QString newer = fedoraNewer(monitor->report()); !newer.isEmpty()) {
            runs += ' ' + newer;
        }
        return runs;
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
    if (!medium.kwin.isEmpty() && !medium.kwayland.isEmpty() &&
        (desktop.isEmpty() || desktop == "kde")) {
        what += tr("<li>KWin %1 for Plasma %2, with KWayland %3, which it is built "
                                     "against; a guest with Fedora's older Plasma gets Fedora's "
                                     "Plasma %2 with them, in the same update</li>")
                    .arg(medium.kwin.toHtmlEscaped(), medium.kwin.section('-', 0, 0),
                         medium.kwayland.toHtmlEscaped());
    } else if (!medium.kwin.isEmpty() && (desktop.isEmpty() || desktop == "kde")) {
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
    /* GNOME's compositor, in place of the guest's while it has its version */
    const bool mutter = !medium.mutter.isEmpty() && (desktop.isEmpty() || desktop == "gnome");
    if (mutter) {
        what += tr("<li>Mutter %1, GNOME's compositor (the first seconds of motion after a new "
                   "screen mode no longer skip refreshes), if the guest's Mutter is %2</li>")
                    .arg(medium.mutter.toHtmlEscaped(), medium.mutter.section('-', 0, 0));
    }
    what += tr("<li>the vitrine agent, which tells vitrine how the guest is "
                                 "doing</li></ul>");
    /* what stays at its version: of what the medium has (Fedora 45's has no KDE packages) */
    const bool kdePackages = !medium.kwin.isEmpty() || !medium.kpipewire.isEmpty() ||
                             !medium.qtbase.isEmpty() || !medium.ksvg.isEmpty() ||
                             !medium.kirigami.isEmpty();
    if (kdePackages) {
        /* with vitrine's KWin, Qt Base, KSvg and Kirigami too, and the
           guest's Plasma and KDE Frameworks held at their versions */
        QStringList held;
        if (!medium.qtbase.isEmpty()) {
            held << tr("Qt Base");
        }
        if (!medium.ksvg.isEmpty()) {
            held << tr("KSvg");
        }
        if (!medium.kirigami.isEmpty()) {
            held << tr("Kirigami");
        }
        held << tr("the guest's Plasma");
        if (!medium.ksvg.isEmpty() || !medium.kirigami.isEmpty()) {
            held << tr("its KDE Frameworks");
        }
        const QString heldList = held.size() == 1
                                     ? held.first()
                                     : tr("%1 and %2").arg(held.mid(0, held.size() - 1).join(", "),
                                                           held.last());
        what += (medium.kwayland.isEmpty()
                     ? tr("<p>Mesa, KWin and KPipeWire then stay at these versions until the "
                          "next guest tools, and so do %1 with vitrine's KWin: Fedora's "
                          "updates of them wait for vitrine's next guest tools. ")
                     : tr("<p>Mesa, KWin, KWayland and KPipeWire then stay at these versions "
                          "until the next guest tools, and so do %1 with vitrine's KWin: "
                          "Fedora's updates of them wait for vitrine's next guest tools. "))
                    .arg(heldList);
        what += tr("Without vitrine's KWin, KSvg, Kirigami and Qt Base give way to Fedora's next "
                   "build of them. The guest needs the network: dkms, the kernel headers and a "
                   "newer Plasma come from Fedora.</p>");
    } else if (!medium.mesa.isEmpty() && mutter) {
        what += tr("<p>Mesa then stays at this version until the next guest tools; Mutter gives "
                   "way to Fedora's next build of it. The guest needs the network: dkms and the "
                   "kernel headers come from Fedora.</p>");
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

QString runningMutter(const GuestTools::Report &report)
{
    const QString mutter = report.packages.value("mutter");

    if (mutter.isEmpty()) {
        return {};
    }
    return report.vitrineBuilds.value("mutter")
               ? tr("Its Mutter is vitrine's build, %1.").arg(mutter.toHtmlEscaped())
               : tr("Its Mutter is %1, not vitrine's build.").arg(mutter.toHtmlEscaped());
}

QString fedoraNewer(const GuestTools::Report &report)
{
    QStringList said;

    for (const GuestTools::Held &h : report.fedoraNewer) {
        said << tr("vitrine's guest tools hold %1 %2; Fedora has %3: a newer vitrine brings "
                   "builds for it.")
                    .arg(h.what.toHtmlEscaped(), h.held.toHtmlEscaped(), h.fedora.toHtmlEscaped());
    }
    return said.join(' ');
}

/* The Qt app's GuestToolsBuildBanner's words, in its translation context */
static QString mediumTr(const char *text)
{
    return QCoreApplication::translate("GuestToolsBuildBanner", text);
}

/* The date of the medium of @release, "5 October 2026"; empty if unknown */
static QString builtOn(const QString &release)
{
    const QDateTime built = GuestToolsBuilder::made(GuestToolsBuilder::mediumImage(release)).built;

    return built.isValid() ? QLocale().toString(built.toLocalTime().date(), "d MMMM yyyy")
                           : QString();
}

QString mediumNote(const QString &release, QString *button, bool *warning)
{
    using State = GuestToolsBuilder::State;
    const GuestToolsBuilder *builder = GuestToolsBuilder::instance();
    bool isWarning = true;
    QString text;
    QString word;

    if (builder->isRunning()) {
        isWarning = false;
        text = mediumTr("The guest tools for Fedora %1 are being built: %2.")
                   .arg(builder->runRelease(), builder->progressText().toHtmlEscaped());
        word = mediumTr("Show");
    } else {
        const State state = GuestToolsBuilder::releaseState(release);
        const QString date = builtOn(release);

        if (!builder->error().isEmpty() && (state == State::NotBuilt || state == State::Outdated)) {
            text = mediumTr("The guest tools could not be built: %1.")
                       .arg(builder->error().toHtmlEscaped());
            word = mediumTr("Show…");
        } else if (state == State::NotBuilt) {
            /* KWin, Qt base and the rest of KDE's take longer than Mesa itself */
            const bool kde = GuestToolsBuilder::packages(GuestToolsBuilder::guestDir(), release)
                                 .contains("kwin");
            text = mediumTr("<b>The guest tools for Fedora %1 are not built yet.</b> Vitrine "
                            "builds them in a Fedora %1 container, which takes %2.")
                       .arg(release, kde ? mediumTr("40 to 70 minutes")
                                         : mediumTr("15 to 30 minutes"));
            word = mediumTr("Build…");
        } else if (state == State::Outdated) {
            isWarning = false;
            text = date.isEmpty()
                       ? mediumTr("<b>The guest tools medium for Fedora %1 is out of date:</b> "
                                  "this version of Vitrine has other guest tools sources.")
                             .arg(release)
                       : mediumTr("<b>The guest tools medium for Fedora %1 is out of date:</b> "
                                  "this version of Vitrine has other guest tools sources than "
                                  "the medium made on %2.")
                             .arg(release, date);
            word = mediumTr("Update…");
        } else if (state == State::NoSources && !GuestTools::medium(release).isValid()) {
            text = mediumTr("The guest tools are not built, and this installation of Vitrine "
                            "has no guest/ folder to build them from.");
        }
    }
    if (button) {
        *button = word;
    }
    if (warning) {
        *warning = isWarning;
    }
    return text;
}

}
