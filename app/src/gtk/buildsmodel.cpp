// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/buildsmodel.h"

#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QUrl>

#include <sys/stat.h>

#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/hostkde.h"
#include "core/paths.h"
#include "core/stackbuilder.h"
#include "ui/buildtexts.h"
#include "ui/hostkdetext.h"

using Builds::Component;
using Builds::Kind;
using State = BuildManager::State;
using StatusState = Builds::Status::State;

namespace BuildsModel {

namespace {

QString capitalized(const QString &text)
{
    return text.isEmpty() ? text : text.left(1).toUpper() + text.mid(1);
}

/* The repository's page, from its git URL */
QString webPage(const QString &url)
{
    return url.endsWith(".git") ? url.chopped(4) : url;
}

/* "gitlab.com/qemu-project/qemu" */
QString shortUrl(const QString &url)
{
    const QUrl u(webPage(url));
    return u.isValid() && !u.host().isEmpty() ? u.host() + u.path() : url;
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

QString code(const QString &text)
{
    return "<code>" + text.toHtmlEscaped() + "</code>";
}

/* "5 October 2026, 14:05, in 18 min": when it was built, and how long it took */
QString builtWhen(const BuildManager &m, const QString &id, const Builds::Status &status)
{
    const QDateTime when = status.built.toLocalTime();
    QString built = QLocale().toString(when.date(), "d MMMM yyyy") + ", " +
                    QLocale().toString(when.time(), QLocale::ShortFormat);
    const QList<qint64> took = m.durations(id);

    if (!took.isEmpty()) {
        built = QString("%1, in %2").arg(built, BuildTexts::duration(took.last()));
    }
    return built;
}

QString outputValue(const QString &output, qint64 used)
{
    return code(output) + (used >= 0 ? " — " + escaped(bytes(used)) : QString());
}

/* A host package's details: what it is, the system's state (the Qt window's systemSource()) */
Details systemDetails(const BuildManager &m, const Component &c,
                      const std::function<qint64(const QString &)> &size)
{
    using Offer = HostKde::Status::Offer;
    using Owner = HostKde::Status::Owner;
    const HostKde::Status host = m.systemStatus(c.id);
    const HostKde::Target &target = host.target;
    const QString title = c.title;
    const HostKde::System system = HostKdeMonitor::instance()->system();
    const Builds::Status status = m.status(c.id);
    const BuildManager::Progress p = m.progress(c.id);
    Details d;
    QString why;

    d.paragraphs << "<b>" + escaped(title) + "</b> — " + escaped(host.summary()) + ".";
    d.paragraphs << escaped(BuildTexts::systemAbout(target) + ' ' +
                            "Optional: Vitrine builds it only when asked, and installs it only "
                            "with the administrator's password.");
    if (p.state == State::Failed) {
        d.paragraphs << escaped(QString("Its last build failed: %1").arg(p.error));
    } else if (p.state == State::Building) {
        d.paragraphs << escaped(QString("Building: %1.").arg(p.phase.isEmpty() ? QString("starting")
                                                                              : p.phase));
    }

    /* what stands in the way, or wants doing */
    if (host.offer == Offer::Unsupported) {
        d.paragraphs << escaped(host.why);
    } else if (host.offer == Offer::NoSet) {
        QStringList sets;
        for (const HostKde::Set &set : HostKde::sets(c.dir, target.package)) {
            QStringList versions;
            for (const QString &pattern : set.versions) {
                versions << HostKde::versionText(pattern);
            }
            sets << QString("%1 for %2 %3").arg(set.series, title, versions.join(", "));
        }
        d.paragraphs << escaped(QString("The research has no host set for %1 %2. Its sets: %3.")
                                    .arg(title, host.version,
                                         sets.isEmpty() ? QString("none") : sets.join(", ")));
    }
    if (host.isOutdated()) {
        QStringList moved;
        for (const QString &line : host.outdated) {
            const QStringList f = line.split(' ');
            moved << QString("%1 %2, built for %3")
                         .arg(HostKde::packageTitle(f.value(0)), f.value(2), f.value(1));
        }
        d.paragraphs
            << "<b>" +
                   escaped(target.package == "kwin"
                               ? QString("Vitrine's KWin no longer matches this computer (%1). At "
                                         "the next login the desktop would run it with the new "
                                         "libraries, and might not start: build and install the "
                                         "matching KWin, or revert, before you log out.")
                                     .arg(moved.join(", "))
                               : QString("Vitrine's %1 no longer matches this computer (%2): "
                                         "programs use it with the newer libraries, which upstream "
                                         "does not support. Build and install the matching one, "
                                         "or revert.")
                                     .arg(title, moved.join(", "))) +
                   "</b>";
    } else if (host.owner == Owner::Foreign) {
        d.paragraphs << escaped(
            host.installed.isLocal()
                ? QString("The %1 installed, %2, is not Vitrine's: it was installed from a file. "
                          "Vitrine leaves it alone: Install… replaces it only after a warning, "
                          "and Revert… is not offered for it.")
                      .arg(title, host.installed.evr())
                : QString("%1 is version-locked at %2, not by Vitrine. Install… replaces it and "
                          "its lock only after a warning.")
                      .arg(title, host.lockedAt.isEmpty() ? host.installed.evr() : host.lockedAt));
    }
    if (host.offer == Offer::Offered && host.build.isValid() && !host.canInstall(&why) &&
        !host.differs.isEmpty()) {
        d.paragraphs << escaped(why);
    }
    if (host.owner == Owner::Vitrine && !host.isOutdated() && host.build.isValid() &&
        host.build.rpm(target.package).digest != host.installed.digest && host.differs.isEmpty()) {
        d.paragraphs << escaped(
            QString("Another build than the one installed is ready (%1 %2, built %3): Install… "
                    "replaces it.")
                .arg(title, host.build.evr,
                     QLocale().toString(host.build.built.toLocalTime().date(), "d MMMM yyyy")));
    }
    if (!host.heldBack.isEmpty()) {
        d.paragraphs << escaped(
            (target.package == "qt6-qtbase"
                 ? QString("Updates wait: the repositories have Qt base %1, and its lock keeps "
                           "this computer at %2, with the rest of Qt (its modules need the same "
                           "version).")
                       .arg(host.heldBack, host.installed.evr())
                 : QString("An update waits: the repositories have %1 %2, and its lock keeps "
                           "this computer at %3.")
                       .arg(title, host.heldBack, host.installed.evr())) +
            ' ' +
            (host.offer == Offer::Offered
                 ? QString("Building it again takes the newer one.")
                 : QString("The research has no set for it yet: Revert… lets the update in.")));
    }
    if (host.nextLogin) {
        d.paragraphs << escaped(target.package == "kwin"
                                    ? QString("The running desktop still runs the KWin it started "
                                              "with: the one installed now runs from the next "
                                              "login.")
                                    : QString("Programs started before it was installed or "
                                              "reverted keep what they loaded; the desktop gets "
                                              "it at the next login."));
    }

    /* where it comes from, and what this computer has */
    const QString from = host.plasmaFrom == "beta"      ? QString("the KDE SIG's beta COPR")
                         : host.plasmaFrom == "testing" ? QString("Fedora's updates-testing")
                                                        : QString("Fedora's repositories");
    d.properties << Property{"Source",
                             escaped(target.package == "kwin"
                                         ? QString("KWin's source RPM from %1, the version of "
                                                   "this computer's Plasma")
                                               .arg(from)
                                         : QString("the source RPM of %1 from %2, its newest "
                                                   "version there")
                                               .arg(target.package, from)),
                             {}, {}};
    d.properties << Property{"Recipe",
                             QString("the research's %1 at commit %2, in a Fedora container")
                                 .arg(code("kde-rpms.sh"),
                                      code(c.values.value("research").left(12))),
                             {}, {}};
    if (!host.series.isEmpty()) {
        int count = 0;
        for (const Builds::Patch &patch : Builds::patches(c)) {
            count += !patch.left && patch.origin.startsWith(host.series + ',');
        }
        d.properties << Property{"Set",
                                 escaped(count == 1 ? QString("%1, 1 patch (the Patches view)")
                                                          .arg(host.series)
                                                    : QString("%1, %2 patches (the Patches view)")
                                                          .arg(host.series)
                                                          .arg(count)),
                                 {}, {}};
    }
    QStringList computer;
    if (!system.fedora.isEmpty()) {
        computer << QString("Fedora %1").arg(system.fedora);
    }
    if (!host.plasma.isEmpty()) {
        computer << QString("Plasma %1").arg(host.plasma);
    }
    if (const HostKde::Package kf = system.package("kf6-kcoreaddons"); kf.isValid()) {
        computer << QString("KDE Frameworks %1").arg(kf.version);
    }
    if (const HostKde::Package qt = system.package("qt6-qtbase"); qt.isValid()) {
        computer << QString("Qt %1").arg(qt.version);
    }
    if (!computer.isEmpty()) {
        d.properties << Property{"This computer", escaped(computer.join(", ")), {}, {}};
    }
    if (host.installed.isValid()) {
        QString installed;
        switch (host.owner) {
        case Owner::Vitrine:
            installed = host.locked ? QString("Vitrine's %1 %2, version-locked")
                                          .arg(title, host.installed.evr())
                                    : QString("Vitrine's %1 %2, not version-locked")
                                          .arg(title, host.installed.evr());
            break;
        case Owner::Foreign:
            installed = host.installed.isLocal()
                            ? QString("%1 %2, not Vitrine's: installed from a file")
                                  .arg(title, host.installed.evr())
                            : QString("%1 %2, from %3")
                                  .arg(title, host.installed.evr(), host.installed.repo);
            if (host.locked) {
                installed += ", version-locked";
            }
            break;
        case Owner::Distribution:
            installed = QString("the distribution's %1 %2").arg(title, host.installed.evr());
            break;
        }
        if (host.names.size() > 1) {
            installed += QString(" (%1 packages: %2)")
                             .arg(host.names.size())
                             .arg(host.names.join(", "));
        }
        d.properties << Property{"Installed", escaped(installed), {}, {}};
    }
    if (status.built.isValid()) {
        QString built = builtWhen(m, c.id, status);
        const HostKde::Build &b = host.build.isValid() ? host.build : host.other;
        if (b.isValid()) {
            built = QString("%1 %2, %3").arg(title, b.evr, built);
        }
        d.properties << Property{"Built", escaped(built), {}, {}};
        if (b.isValid() && target.package != "qt6-qtbase" && b.buildroot.contains("qt6-qtbase")) {
            d.properties << Property{
                "Built against",
                escaped(QString("Qt %1").arg(b.buildroot.value("qt6-qtbase").section('-', 0, 0))),
                {}, {}};
        }
    }
    if (!status.output.isEmpty()) {
        d.properties << Property{"Output", outputValue(status.output, size(status.output)), {},
                                 status.output};
    }
    d.properties << Property{
        "A build needs",
        escaped(target.heavy ? QString("up to %1 GiB of memory, in its container")
                                   .arg(GuestToolsBuilder::memoryCapMiB() / 1024)
                             : QString("a GiB or two of memory, in its container")),
        {}, {}};
    d.after << escaped(
        target.package == "kwin"
            ? QString("Installing replaces the compositor, as root, version-locked: after each "
                      "Plasma or Qt update it must be built and installed again, or reverted to "
                      "the distribution's KWin. It takes effect at the next login.")
        : target.package == "qt6-qtbase"
            ? QString("Installing replaces all of Qt base's packages, as root, version-locked: Qt "
                      "updates wait until it is built and installed again for them, or reverted. "
                      "Programs started after it use it; the desktop, from the next login.")
            : QString("Installing replaces it, as root, version-locked: when the KDE Frameworks "
                      "move to a new release, it must be built and installed again, or reverted. "
                      "Programs started after it use it; the desktop, from the next login."));
    return d;
}

}

Group groupOf(Kind kind)
{
    switch (kind) {
    case Kind::Host:
        return Group::Host;
    case Kind::System:
        return Group::System;
    default:
        return Group::Guest;
    }
}

QString groupId(Group group)
{
    return group == Group::Host ? QString("host") : group == Group::System ? QString("system")
                                                                            : QString("guest");
}

bool isGroupId(const QString &id)
{
    return id == "host" || id == "guest" || id == "system";
}

Group groupNamed(const QString &id)
{
    return id == "host" ? Group::Host : id == "system" ? Group::System : Group::Guest;
}

QString groupTitle(Group group, const QString &guestRelease)
{
    switch (group) {
    case Group::Host:
        return "Vitrine's QEMU";
    case Group::System:
        return "Host Desktop";
    case Group::Guest:
        break;
    }
    return guestRelease.isEmpty() ? QString("Guest Tools")
                                  : QString("Guest Tools for Fedora %1").arg(guestRelease);
}

QList<Group> groups(const BuildManager &m)
{
    QList<Group> list;

    for (const Component &c : m.components()) {
        if (!list.contains(groupOf(c.kind))) {
            list << groupOf(c.kind);
        }
    }
    return list;
}

QStringList componentsOf(const BuildManager &m, Group group)
{
    QStringList ids;

    for (const Component &c : m.components()) {
        if (groupOf(c.kind) == group) {
            ids << c.id;
        }
    }
    return ids;
}

QString title(const BuildManager &m, const QString &id)
{
    const Component c = m.component(id);
    QString version = m.status(id).version;

    if (c.kind == Kind::Package && version.isEmpty()) {
        version = Builds::source(c).revision.section('-', 0, 0);
    }
    /* the Mesa for Flatpak apps: one for each runtime, in its Source */
    return c.kind == Kind::Medium || c.kind == Kind::Flatpak || version.isEmpty()
               ? c.title
               : c.title + ' ' + version;
}

Status status(const BuildManager &m, const QString &id)
{
    const Component c = m.component(id);
    const Builds::Status built = m.status(id);
    const BuildManager::Progress p = m.progress(id);
    const bool running = m.isRunning();
    Status s;

    if (!c.isValid()) {
        return s;
    }
    /* how the run goes for it, else what is built */
    switch (p.state) {
    case State::Queued:
        s.text = "Queued";
        s.look = Look::Waiting;
        break;
    case State::Waiting:
        s.text = "Waiting for memory";
        s.look = Look::Waiting;
        break;
    case State::Building:
        s.text = p.phase.isEmpty() ? QString("Starting") : capitalized(p.phase);
        if (p.total > 0) {
            s.text = QString("%1, %2%").arg(s.text).arg(int(qint64(p.done) * 100 / p.total));
            s.fraction = double(p.done) / p.total;
        }
        s.look = Look::Building;
        break;
    case State::Failed:
        s.text = QString("Failed: %1").arg(p.error);
        s.look = Look::Failed;
        break;
    case State::Stopped:
        s.text = "Stopped";
        s.look = Look::Stopped;
        break;
    case State::Skipped:
        s.text = "Not built: a build before it failed";
        s.look = Look::Stopped;
        break;
    case State::Built:
    case State::UpToDate:
    case State::Restored:
        if (running) {
            s.text = p.state == State::Built      ? QString("Built")
                     : p.state == State::Restored ? QString("Its build before, brought back")
                                                  : QString("Up to date");
        }
        break;
    case State::Idle:
        break;
    }
    if (s.text.isEmpty() && c.kind == Kind::System) {
        s.text = m.systemStatus(id).summary();
    }
    if (s.text.isEmpty()) {
        switch (built.state) {
        case StatusState::NoSources:
            s.text = "No sources to build it from";
            break;
        case StatusState::NotBuilt:
            s.text = "Not built";
            break;
        case StatusState::Outdated: {
            const QString why = Builds::summary(built.changes);
            s.text = why.isEmpty() ? QString("Out of date") : QString("Out of date: %1").arg(why);
            break;
        }
        case StatusState::UpToDate:
            s.text = "Up to date";
            break;
        }
    }
    if (s.look == Look::None) {
        s.look = built.state == StatusState::UpToDate ? Look::UpToDate
                 : built.state == StatusState::NoSources ? Look::Warning
                                                         : Look::ToBuild;
    }
    /* a host package: what it is to the user, rather than to a build */
    if (c.kind == Kind::System && p.state == State::Idle) {
        using Owner = HostKde::Status::Owner;
        using Offer = HostKde::Status::Offer;
        const HostKde::Status host = m.systemStatus(id);
        if (host.isOutdated() || host.owner == Owner::Foreign ||
            (host.owner == Owner::Vitrine && !host.locked)) {
            s.look = Look::Warning;
        } else if (host.owner == Owner::Vitrine) {
            s.look = Look::UpToDate;
        } else if (host.offer != Offer::Offered) {
            s.look = Look::Information;
        }
    }
    return s;
}

/* A short date, or a long one with the time; empty if none */
static QString whenText(const QDateTime &when, bool longFormat)
{
    if (!when.isValid()) {
        return {};
    }
    return longFormat ? QLocale().toString(when.toLocalTime(), QLocale::LongFormat)
                      : QLocale().toString(when.toLocalTime().date(), QLocale::ShortFormat);
}

QString built(const BuildManager &m, const QString &id, bool longFormat)
{
    return whenText(m.status(id).built, longFormat);
}

QString releaseBuilt(const BuildManager &m, const QString &release, bool longFormat)
{
    return whenText(m.guestToolsBuilt(release), longFormat);
}

QString releaseId(const QString &release)
{
    return "release:" + release;
}

QString releaseOf(const QString &id)
{
    return id.startsWith("release:") ? id.mid(8) : QString();
}

Status releaseStatus(const BuildManager &m, const QString &release)
{
    const GuestToolsBuilder::State state = m.guestToolsState(release);
    Status s;

    s.text = BuildTexts::releaseState(state);
    switch (state) {
    case GuestToolsBuilder::State::UpToDate:
        s.look = Look::UpToDate;
        break;
    case GuestToolsBuilder::State::NoSources:
        s.look = Look::Warning;
        break;
    default:
        s.look = Look::ToBuild;
        break;
    }
    return s;
}

QString groupStatus(const BuildManager &m, Group group)
{
    const QStringList ids = componentsOf(m, group);
    int outdated = 0;
    int building = 0;
    int failed = 0;
    bool all = true;

    for (const QString &id : ids) {
        const StatusState state = m.status(id).state;
        const BuildManager::Progress p = m.progress(id);
        outdated += state != StatusState::UpToDate;
        all = all && state == StatusState::NotBuilt;
        building += p.state == State::Building || p.state == State::Waiting ||
                    p.state == State::Queued;
        failed += p.state == State::Failed;
    }
    /* optional, and about the system rather than builds: what is installed */
    if (group == Group::System && building == 0 && failed == 0) {
        QStringList installed;
        bool behind = false;
        for (const QString &id : ids) {
            const HostKde::Status s = m.systemStatus(id);
            if (s.owner == HostKde::Status::Owner::Vitrine) {
                installed << s.target.title;
            }
            behind = behind || s.isOutdated();
        }
        return behind                 ? QString("Out of date")
               : !installed.isEmpty() ? QString("Installed: %1").arg(installed.join(", "))
                                      : QString("Optional");
    }
    return building > 0     ? QString("Building")
           : failed > 0     ? QString("Failed")
           : outdated == 0  ? QString("Up to date")
           : all            ? QString("Not built")
                            : QString("Out of date");
}

QStringList selection(const BuildManager &m, const QString &id)
{
    if (isGroupId(id)) {
        QStringList ids;
        for (const QString &child : componentsOf(m, groupNamed(id))) {
            /* of the host packages, those the research has a set for: building one
               installs nothing (Install… does, with the administrator's password),
               and the group's Build is how they are all built at once */
            if (id != "system" || m.systemStatus(child).canBuild()) {
                ids << child;
            }
        }
        return ids;
    }
    return id.isEmpty() || !m.component(id).isValid() ? QStringList() : QStringList{id};
}

QStringList toBuild(const BuildManager &m, const QStringList &ids)
{
    QStringList todo;

    for (const QString &id : ids) {
        const StatusState state = m.status(id).state;
        if (state == StatusState::NotBuilt || state == StatusState::Outdated) {
            todo << id;
        }
    }
    return todo;
}

QStringList installable(const BuildManager &m)
{
    QStringList ids;

    for (const QString &id : componentsOf(m, Group::System)) {
        if (m.systemStatus(id).canInstall()) {
            ids << id;
        }
    }
    return ids;
}

Action build(const BuildManager &m, const QString &id)
{
    const QStringList ids = selection(m, id);
    const QStringList todo = toBuild(m, ids);
    bool built = false;
    Action a;

    for (const QString &c : ids) {
        const StatusState state = m.status(c).state;
        built = built || state == StatusState::Outdated || state == StatusState::UpToDate;
    }
    /* "Update QEMU", "Build Guest Tools" */
    const QString title = isGroupId(id) ? (id == "host"     ? QString("Vitrine's QEMU")
                                           : id == "system" ? QString("Host Desktop")
                                                            : QString("Guest Tools"))
                                        : m.component(id).title;
    a.label = title.isEmpty()            ? QString("_Build")
              : built && !todo.isEmpty() ? QString("_Update %1").arg(title)
                                         : QString("_Build %1").arg(title);
    a.enabled = !m.isRunning() && !todo.isEmpty();
    const QStringList plan = todo.isEmpty() ? QStringList() : m.plan(todo);
    QStringList titles;
    for (const QString &c : plan) {
        titles << m.component(c).title;
    }
    a.tooltip = plan.isEmpty() ? QString() : QString("Builds %1").arg(titles.join(", "));
    /* a host package the research has no set for, or this host does not build */
    QString why;
    if (m.component(id).kind == Kind::System && !m.isRunning() &&
        !m.systemStatus(id).canBuild(&why)) {
        a.tooltip = why;
    }
    return a;
}

Action buildAll(const BuildManager &m)
{
    const QStringList outdated = m.outdated();
    QStringList titles;
    bool anyBuilt = false;
    Action a;

    for (const QString &c : outdated) {
        titles << m.component(c).title;
    }
    for (const Component &c : m.components()) {
        const StatusState state = m.status(c.id).state;
        anyBuilt = anyBuilt || state == StatusState::UpToDate || state == StatusState::Outdated;
    }
    a.label = anyBuilt ? QString("Update _All") : QString("Build _All");
    a.enabled = !m.isRunning() && !outdated.isEmpty();
    a.tooltip = outdated.isEmpty() ? QString("All is up to date")
                                   : QString("Builds %1").arg(titles.join(", "));
    return a;
}

Action install(const BuildManager &m, const QString &id)
{
    const HostKdeMonitor *monitor = HostKdeMonitor::instance();
    const bool running = m.isRunning();
    Action a;

    /* the Host Desktop: all those ready at once, one password */
    if (id == "system") {
        const QStringList ready = installable(m);
        QStringList titles;
        for (const QString &c : ready) {
            titles << m.systemStatus(c).target.title;
        }
        a.label = "_Install All…";
        a.enabled = !(monitor->isBusy() || (running && ready.contains(m.current()))) &&
                    !ready.isEmpty();
        a.tooltip = ready.isEmpty() ? QString("No build is waiting to be installed")
                                    : QString("Install %1 on this computer at once, with the "
                                              "administrator's password")
                                          .arg(HostKdeDialog::names(titles));
        return a;
    }
    const HostKde::Status status = m.systemStatus(id);
    QString why;
    a.label = "_Install…";
    a.enabled = !(monitor->isBusy() || (running && m.current() == id)) && status.canInstall(&why);
    a.tooltip = !why.isEmpty() ? why
                               : QString("Install it on this computer, with the administrator's "
                                         "password");
    return a;
}

Action revert(const BuildManager &m, const QString &id)
{
    const HostKdeMonitor *monitor = HostKdeMonitor::instance();
    const HostKde::Status status = m.systemStatus(id);
    QString why;
    Action a;

    a.label = "Re_vert…";
    a.enabled = !(monitor->isBusy() || (m.isRunning() && m.current() == id)) &&
                status.canRevert(&why);
    a.tooltip = !why.isEmpty() ? why
                               : QString("Put the distribution's %1 back").arg(status.target.title);
    return a;
}

QString runText(const BuildManager &m)
{
    QString text;

    if (m.isRunning()) {
        const QString id = m.current();
        const QStringList queue = m.queue();
        const QString title = m.component(id.isEmpty() ? queue.value(0) : id).title;
        text = queue.size() > 1 ? QString("Building %1 (%2 of %3)")
                                      .arg(title)
                                      .arg(qMax<qsizetype>(1, queue.indexOf(id) + 1))
                                      .arg(queue.size())
                                : QString("Building %1").arg(title);
        return m.isWaiting() ? QString("%1: waiting for memory.").arg(text) : text + '.';
    }
    const QStringList outdated = m.outdated();
    if (m.components().isEmpty()) {
        text = "This installation of Vitrine has nothing to build: no host/ or guest/ folder.";
    } else if (outdated.isEmpty()) {
        text = "All is built from this version's sources.";
    } else {
        text = outdated.size() == 1 ? QString("1 component to build.")
                                    : QString("%1 components to build.").arg(outdated.size());
    }
    if (!m.error().isEmpty()) {
        text = QString("The last build failed: %1.").arg(m.error()) + ' ' + text;
    }
    return text;
}

QString missingText(const BuildManager &m)
{
    const StackBuilder *stack = StackBuilder::instance();
    QStringList guest;
    QString text;

    if (m.isRunning()) {
        return {};
    }
    if (!stack->missing().isEmpty()) {
        text = QString("Missing for the build of Vitrine's QEMU: %1.")
                   .arg(escaped(stack->missing().join(", ")));
        if (!stack->installCommand().isEmpty()) {
            text += "<br>" + QString("Install them with: %1").arg(code(stack->installCommand()));
        }
    }
    /* what this host lacks now for the guest tools' */
    const QList<GuestToolsBuilder::Requirement> needed = GuestToolsBuilder::missing();
    for (const GuestToolsBuilder::Requirement &r : needed) {
        guest << (r.command == "podman" ? QString("podman, which runs the build in a Fedora "
                                                  "container")
                                        : escaped(r.command));
    }
    if (!guest.isEmpty()) {
        if (!text.isEmpty()) {
            text += "<br>";
        }
        text += QString("Missing for the build of the guest tools: %1.").arg(guest.join(", "));
        text += "<br>" + QString("Install them with: %1")
                             .arg(code(GuestToolsBuilder::installCommand(needed)));
    }
    return text;
}

QString memoryText(const BuildManager &m)
{
    const auto gib = [](qint64 mib) {
        return QLocale().toString(double(mib) / 1024, 'f', mib % 1024 == 0 ? 0 : 1);
    };

    if (!m.isWaiting()) {
        return {};
    }
    return QString("<b>Waiting for memory.</b> Building %1 takes up to %2 GiB, and %3 GiB is free: "
                   "close VMs or other programs. The build goes on by itself once %4 GiB is free, "
                   "what the host keeps free included.")
        .arg(escaped(m.component(m.current()).title), gib(GuestToolsBuilder::memoryCapMiB()),
             gib(m.availableMiB()), gib(m.neededMiB()));
}

Details details(const BuildManager &m, const QString &id,
                const std::function<qint64(const QString &)> &size)
{
    const Component c = m.component(id);
    const Builds::Status status = m.status(id);
    const BuildManager::Progress p = m.progress(id);
    Details d;

    if (!c.isValid()) {
        return d;
    }
    if (c.kind == Kind::System) {
        return systemDetails(m, c, size);
    }

    /* what it is, and how it stands */
    QString state;
    switch (status.state) {
    case StatusState::UpToDate:
        state = "up to date: built from these sources.";
        break;
    case StatusState::NotBuilt:
        state = "not built yet.";
        break;
    case StatusState::NoSources:
        state = "no sources to build it from.";
        break;
    case StatusState::Outdated:
        state = "out of date. Changed since its build:";
        break;
    }
    d.paragraphs << "<b>" + escaped(c.title) + "</b> — " + escaped(state);
    if (status.state == StatusState::Outdated) {
        for (const Builds::Change &change : status.changes) {
            d.changes << Builds::describe(change);
        }
    }
    if (p.state == State::Failed) {
        d.paragraphs << escaped(QString("Its last build failed: %1").arg(p.error));
    } else if (p.state == State::Building) {
        d.paragraphs << escaped(QString("Building: %1.").arg(p.phase.isEmpty() ? QString("starting")
                                                                              : p.phase));
    }

    /* where it comes from */
    const Builds::Source source = Builds::source(c);
    switch (c.kind) {
    case Kind::Host:
        d.properties << Property{"Source",
                                 QString("%1, commit %2")
                                     .arg(escaped(shortUrl(source.url)),
                                          code(source.revision.left(12))),
                                 webPage(source.url), {}};
        break;
    case Kind::Package:
        d.properties << Property{
            "Source",
            c.id == "tools"
                ? QString("its folder %1 in Vitrine's guest/, version %2")
                      .arg(code(c.values.value("spec").section('/', 0, 0) + '/'),
                           escaped(source.revision))
                : QString("%1 from %2, built with Fedora's spec %3, version %4")
                      .arg(code(source.archive), escaped(source.archiveHost),
                           code(c.values.value("spec").section('/', -1)), escaped(source.revision)),
            {}, {}};
        if (!source.url.isEmpty()) {
            d.properties << Property{"Project", escaped(shortUrl(source.url)), source.url, {}};
        }
        break;
    case Kind::Flatpak: {
        QStringList parts;
        for (const Builds::FlatpakBranch &b : Builds::flatpakBranches(c)) {
            parts << QString("Mesa %1 for runtime %2 (commit %3)")
                         .arg(escaped(b.mesa), escaped(b.branch), code(b.commit.left(12)));
        }
        d.properties << Property{"Source",
                                 QString("%1, the recipe of Flathub's: %2")
                                     .arg(escaped(shortUrl(source.url)), parts.join(", ")),
                                 source.url, {}};
        break;
    }
    case Kind::Medium: {
        QStringList parts;
        QStringList others;
        for (const QString &dep : c.deps) {
            const Component dc = m.component(dep);
            if (dc.isValid()) {
                (dc.kind == Kind::Flatpak ? others : parts) << escaped(dc.title);
            }
        }
        d.properties << Property{
            "Made of",
            others.isEmpty()
                ? QString("the packages %1, and the installer").arg(parts.join(", "))
                : QString("the packages %1; %2 and the installer")
                      .arg(parts.join(", "), others.join(", ")),
            {}, {}};
        break;
    }
    case Kind::System:
        break;
    }
    /* its patches, in a word: their number by origin */
    const QList<Builds::Patch> patches = Builds::patches(c);
    if (!patches.isEmpty()) {
        QStringList origins;
        QHash<QString, int> count;
        for (const Builds::Patch &patch : patches) {
            /* not built: the Patches view shows them apart */
            if (patch.left) {
                continue;
            }
            if (!count.contains(patch.origin)) {
                origins << patch.origin;
            }
            count[patch.origin]++;
        }
        QStringList parts;
        for (const QString &origin : std::as_const(origins)) {
            parts << escaped(origin.isEmpty()
                                 ? QString::number(count.value(origin))
                                 : QString("%1 from %2").arg(count.value(origin)).arg(origin));
        }
        d.properties << Property{"Patches", parts.join(", "), {}, {}};
    }
    /* a library of QEMU's, or a package's (KWin's KWayland) */
    if ((c.kind == Kind::Host || c.kind == Kind::Package) && !c.deps.isEmpty()) {
        QStringList titles;
        for (const QString &dep : c.deps) {
            titles << escaped(m.component(dep).title);
        }
        d.properties << Property{"Built against", titles.join(", "), {}, {}};
    }
    /* QEMU and passt, which it starts for a VM's network */
    if (c.kind == Kind::Host && !c.runs.isEmpty()) {
        QStringList titles;
        for (const QString &dep : c.runs) {
            titles << escaped(m.component(dep).title);
        }
        d.properties << Property{"Runs with", titles.join(", "), {}, {}};
    }
    if (status.built.isValid()) {
        d.properties << Property{"Built", escaped(builtWhen(m, id, status)), {}, {}};
    }
    if (!status.output.isEmpty()) {
        d.properties << Property{"Output", outputValue(status.output, size(status.output)), {},
                                 status.output};
    }
    if (c.id == "qemu" && !Paths::customQemuBinary().isEmpty()) {
        d.properties << Property{"In use",
                                 escaped(QString("no: the preferences choose another QEMU, %1")
                                             .arg(Paths::customQemuBinary())),
                                 {}, {}};
    }
    return d;
}

QList<PatchGroup> patchGroups(const BuildManager &m, const QString &id)
{
    const Component c = m.component(id);
    QList<PatchGroup> groups;
    QString origin;
    int number = 0;

    for (const Builds::Patch &patch : Builds::patches(c)) {
        /* their origins, in the order they apply */
        if (groups.isEmpty() || patch.origin != origin) {
            origin = patch.origin;
            /* the host's KWin's are sets, one of them for this computer */
            if (c.kind == Kind::System) {
                number = 0;
            }
            groups << PatchGroup{origin, {}, {}};
        }
        PatchGroup &group = groups.last();
        group.patches << patch;
        group.subjects << QString("%1. %2").arg(++number).arg(Builds::subject(c, patch));
    }
    for (PatchGroup &group : groups) {
        const QString count = group.patches.size() == 1
                                  ? QString("1 patch")
                                  : QString("%1 patches").arg(group.patches.size());
        group.title = group.title.isEmpty() ? count : group.title + " · " + count;
    }
    return groups;
}

QString bytes(qint64 size)
{
    return QLocale().formattedDataSize(size, 1, QLocale::DataSizeTraditionalFormat);
}

qint64 diskUsage(const QString &path)
{
    struct stat st;
    qint64 total = 0;

    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return qint64(st.st_blocks) * 512;
    }
    QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (::lstat(QFile::encodeName(it.next()).constData(), &st) == 0) {
            total += qint64(st.st_blocks) * 512;
        }
    }
    return total;
}

}
