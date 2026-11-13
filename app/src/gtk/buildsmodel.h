// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "core/buildcomponents.h"

class BuildManager;

/*
 * What the GTK app's Builds window shows, as the Qt app's tells it, from
 * the core's BuildManager (its components, their state, the run) and
 * HostKdeMonitor (the host's packages): words, states and what the
 * buttons do, without a widget (Qt Core: the tests read it with no
 * display).  The words that both apps share are BuildTexts' and
 * HostKdeDialog's; these are the Qt window's own, in another shape: rows
 * and paragraphs rather than a tree and a page of HTML.
 */
namespace BuildsModel {

/* The groups of components, by what they make */
enum class Group { Host, Guest, System };
Group groupOf(Builds::Kind kind);
/* "host", "guest", "system": what the window selects a group by */
QString groupId(Group group);
bool isGroupId(const QString &id);
Group groupNamed(const QString &id);
/* "Vitrine's QEMU", "Guest Tools for Fedora 45", "Host Desktop" */
QString groupTitle(Group group, const QString &guestRelease);
/* The groups that have components, in build order */
QList<Group> groups(const BuildManager &m);
/*
 * The guest tools of another release than the one whose components show
 * (BuildManager::otherGuestReleases()), by their medium: the id of its item
 * ("release:44"), and back ("44"; empty for any other id); how it stands,
 * when it was made
 */
QString releaseId(const QString &release);
QString releaseOf(const QString &id);
QStringList componentsOf(const BuildManager &m, Group group);

/* "QEMU 11.1.50", "Mesa 26.2.3": what is built, else what its sources make; the medium's title */
QString title(const BuildManager &m, const QString &id);

/* How a component stands, for its icon */
enum class Look {
    None,
    Waiting,        // queued, or waiting for memory
    Building,
    Failed,
    Stopped,        // stopped, or not built as one before it failed
    UpToDate,
    ToBuild,        // not built, or out of date
    Warning,        // no sources; a host package to see to
    Information,    // a host package not offered here
};
struct Status {
    QString text;           // "Out of date: 2 patches changed", "Compiling QEMU, 45%"
    Look look = Look::None;
    double fraction = -1;   // while it builds and tells how far, 0 to 1
};
Status status(const BuildManager &m, const QString &id);
Status releaseStatus(const BuildManager &m, const QString &release);
/* When what is built was: a short date, and a long one with the time; empty if none */
QString built(const BuildManager &m, const QString &id, bool longFormat = false);
QString releaseBuilt(const BuildManager &m, const QString &release, bool longFormat = false);
/* A group's: Building, Failed, Up to date, Not built, Out of date; the Host Desktop's
   what Vitrine installed of it, else Optional */
QString groupStatus(const BuildManager &m, Group group);

/* What a Build of @id builds: the component, or a group's (of the host packages, those
   the research has a set for) */
QStringList selection(const BuildManager &m, const QString &id);
/* Of @ids, those not built from their sources now */
QStringList toBuild(const BuildManager &m, const QStringList &ids);
/* The Host Desktop's packages that can be installed now: Install All… */
QStringList installable(const BuildManager &m);

/* A button: its words, whether it can act, and its tooltip */
struct Action {
    QString label;
    bool enabled = false;
    QString tooltip;
};
/* "Update QEMU", "Build Guest Tools", "Build": the Build of @id */
Action build(const BuildManager &m, const QString &id);
/* "Update All", "Build All" */
Action buildAll(const BuildManager &m);
/* A host package's Install… and Revert…, the Host Desktop's Install All… (@id "system") */
Action install(const BuildManager &m, const QString &id);
Action revert(const BuildManager &m, const QString &id);

/* The run, or what there is to build: "Building QEMU (2 of 4).", "3 components to build." */
QString runText(const BuildManager &m);
/* What this host lacks for the builds, and the commands that install it; rich text, empty if
   nothing (nor during a run) */
QString missingText(const BuildManager &m);
/* While a heavy package waits for memory: what it needs and what is free; rich text */
QString memoryText(const BuildManager &m);

/* A component's Overview: what it is and how it stands, where it comes from */
struct Property {
    QString label;          // "Source"
    QString value;          // rich text
    QString link;           // a web page to open, if any
    QString path;           // a folder or file to open, if any
};
struct Details {
    /* rich text, in order: what it is and its state, why it changed, what failed or builds */
    QStringList paragraphs;
    /* Outdated: what changed since its build ("Patch fork/0001-x.patch changed") */
    QStringList changes;
    QList<Property> properties;
    /* after the properties: what an install of a host package does */
    QStringList after;
};
/* @size: what a folder or file takes on disk, -1 while it is not known yet */
Details details(const BuildManager &m, const QString &id,
                const std::function<qint64(const QString &path)> &size);

/* Its patches by origin, in the order they apply: "Research · 7 patches" */
struct PatchGroup {
    QString title;
    QList<Builds::Patch> patches;
    /* "1. What it does", in the order they apply */
    QStringList subjects;
};
QList<PatchGroup> patchGroups(const BuildManager &m, const QString &id);

/* "1.4 GB", as the size of files goes */
QString bytes(qint64 size);
/* What @path takes on disk, its folders' files too, links not followed (in any thread) */
qint64 diskUsage(const QString &path);

}
