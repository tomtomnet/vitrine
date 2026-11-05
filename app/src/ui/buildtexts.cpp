// SPDX-License-Identifier: GPL-2.0-or-later
#include "buildtexts.h"

#include <QDateTime>
#include <QLocale>
#include <QStringList>

#include "core/guesttools.h"
#include "core/paths.h"

QString BuildTexts::describe(const StackBuilder::Build &build)
{
    if (!build.isValid()) {
        return tr("Not built yet");
    }
    return tr("Vitrine's build of %1, %2")
        .arg(QLocale().toString(build.built.toLocalTime().date(), "d MMMM yyyy"),
             build.summary());
}

QString BuildTexts::explain(StackBuilder::State state, const StackBuilder::Build &build)
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

QString BuildTexts::describeGuestTools(const QString &fedora)
{
    const GuestTools::Medium medium = GuestTools::medium(fedora);
    const QString release = ".fc" + fedora;
    const QDateTime built = GuestToolsBuilder::made(GuestToolsBuilder::mediumImage(fedora)).built;
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
    return built.isValid()
               ? tr("Built on %1: %2")
                     .arg(QLocale().toString(built.toLocalTime().date(), "d MMMM yyyy"),
                          parts.join(", "))
               : tr("The medium of %1").arg(parts.join(", "));
}

QString BuildTexts::explain(GuestToolsBuilder::State state)
{
    switch (state) {
    case GuestToolsBuilder::State::NoSources:
        return tr("This installation of Vitrine has no guest/ folder: it cannot build the guest "
                  "tools.");
    case GuestToolsBuilder::State::NotBuilt:
        return tr("The guest tools are not built yet: Fedora guests need them for Vitrine's "
                  "graphics driver, Mesa with native context and KWin.");
    case GuestToolsBuilder::State::Outdated:
        return tr("Out of date: this version of Vitrine has other guest tools sources than this "
                  "medium. VMs keep the guest tools they have until they are updated.");
    case GuestToolsBuilder::State::UpToDate:
        return tr("Up to date.");
    }
    return {};
}

QString BuildTexts::duration(qint64 ms)
{
    const qint64 minutes = (ms + 30000) / 60000;

    if (ms < 59500) {
        return tr("%1 s").arg(qMax<qint64>(1, (ms + 500) / 1000));
    }
    if (minutes < 60) {
        return tr("%1 min").arg(minutes);
    }
    return tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

QString BuildTexts::groupAbout(const QString &group, const QString &guestRelease)
{
    return group == "host"
           ? tr("<p><b>Vitrine's QEMU</b>: upstream QEMU, virglrenderer, rutabaga_gfx "
                "and passt at pinned commits, with Vitrine's patches, for the display in "
                "Vitrine's window, 3D acceleration by the host GPU's own driver "
                "(rutabaga_gfx: QEMU's virtio-gpu-rutabaga device) and the VMs' NAT "
                "network (passt). Built into %1.</p>")
                 .arg(Paths::stackDir().toHtmlEscaped())
           : group == "system"
           ? tr("<p><b>Host desktop</b>, optional: this computer's own compositor, KWin, "
                "and libraries the desktop draws with, KSvg, Kirigami and Qt base, with "
                "the research's host sets: steadier frames and quicker input in VMs, the "
                "pointer grab inside Vitrine's Desktop view, popups and resizes that cost "
                "less. Vitrine builds each only when asked, and installs it only with the "
                "administrator's password.</p>")
           : tr("<p><b>The guest tools for Fedora %1</b>: the packages Fedora %1 guests "
                "install from the medium, built in a Fedora %1 container with podman, "
                "and the medium itself. Each Fedora release has its own: %2.</p>")
                 .arg(guestRelease, GuestTools::supportedSystems());
}

/* What a host package is, and what the research's set changes and measured */
QString BuildTexts::systemAbout(const HostKde::Target &target)
{
    if (target.package == "kwin") {
        return tr("This computer's own compositor with the research's host set: "
                  "steadier frames and quicker input in VMs, and the pointer grab "
                  "(Ctrl+Alt+G) inside Vitrine's Desktop view.");
    }
    if (target.package == "kf6-ksvg") {
        return tr("The KDE framework that draws Plasma's themed frames, with the "
                  "research's host set: Plasma's popups stop uploading their frames "
                  "again each time they open. Measured by the research with "
                  "Kirigami's set, on an Arc Pro B50 at 4K and 120 Hz: the launcher "
                  "opens in 61-69 ms instead of 72-78, and Super pressed 8 times a "
                  "second repeats 0.2 frames a second instead of 6-9.");
    }
    if (target.package == "kf6-kirigami") {
        return tr("The KDE framework of the controls Plasma and many programs are made "
                  "of, with the research's host set: their items react to a change of "
                  "the frame contrast only when it changes (a popup's items reacted "
                  "some 1,600 times per opening and closing). Measured by the research "
                  "with KSvg's set, on an Arc Pro B50 at 4K and 120 Hz: the launcher "
                  "opens in 61-69 ms instead of 72-78.");
    }
    if (target.package == "qt6-qtbase") {
        return tr("Qt's base libraries, which every Qt program uses, with the "
                  "research's host set: a window being resized reuses its shared "
                  "memory for the next sizes instead of new memory for each. Measured "
                  "by the research, bare 4K resizes: Konsole shows 111-114 sizes a "
                  "second instead of 85-88, Dolphin 99-102 instead of 62.");
    }
    return tr("A package of this computer's with the research's host set.");
}

/* The Qt app's StackBanner's words, in its translation context */
static QString stackTr(const char *text)
{
    return QCoreApplication::translate("StackBanner", text);
}

QString BuildTexts::stackNote(const StackBuilder *builder, int percent, const QString &error,
                              QString *button, bool *note)
{
    const StackBuilder::State state = StackBuilder::state();
    bool isNote = false;
    QString text;
    QString word;

    if (builder->isRunning()) {
        isNote = true;
        text = stackTr("Building Vitrine's QEMU");
        if (builder->step() > 0) {
            text += stackTr(": step %1 of %2, %3").arg(builder->step()).arg(builder->steps())
                        .arg(builder->stepText().toHtmlEscaped());
            if (percent >= 0) {
                text += stackTr(" (%1%)").arg(percent);
            }
        }
        text += '.';
        word = stackTr("Show");
    } else if (!Paths::customQemuBinary().isEmpty() ||
               state == StackBuilder::State::UpToDate) {
        /* the user's QEMU, or nothing to do */
    } else if (!error.isEmpty()) {
        text = stackTr("Vitrine's QEMU could not be built. %1.").arg(error.toHtmlEscaped());
        word = stackTr("Show…");
    } else if (state == StackBuilder::State::NotBuilt) {
        text = stackTr("<b>Vitrine's QEMU is not built yet.</b> VMs need it for their display "
                       "in this window and for 3D acceleration, and new Linux VMs need it to "
                       "start. Building it takes a few minutes.");
        word = stackTr("Build…");
    } else if (state == StackBuilder::State::Outdated) {
        text = stackTr("<b>Vitrine's QEMU is out of date:</b> this version of Vitrine builds "
                       "it from other sources or patches. VMs running keep the QEMU they "
                       "started with.");
        word = stackTr("Update…");
    } else {
        text = explain(state, {}).toHtmlEscaped();
    }

    if (button) {
        *button = word;
    }
    if (note) {
        *note = isNote;
    }
    return text;
}
