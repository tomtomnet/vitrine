// SPDX-License-Identifier: GPL-2.0-or-later
#include "buildtexts.h"

#include <QDateTime>
#include <QLocale>
#include <QStringList>

#include "core/guesttools.h"

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
