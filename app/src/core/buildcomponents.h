// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

/*
 * What Vitrine builds: the components host/components.conf and
 * guest/components.conf describe (virglrenderer, rutabaga_gfx and QEMU;
 * the guest tools' packages and the medium made of them), each with its
 * source, its patches and what it is built against; and whether what is
 * built is what the sources make now, and if not, why.
 *
 * The scripts build them (host/build.sh, guest/build-rpms.sh,
 * guest/build-medium.sh) and record with each build the lines of inputs
 * its key sums up; this computes the same lines, to tell without running
 * them, and compares them with those recorded to say what changed.
 */
namespace Builds {

enum class Kind {
    Host,       // host/build.sh: fetched at a pinned commit, patched, built
    Package,    // guest/build-rpms.sh: a spec and the files beside it
    Medium,     // guest/build-medium.sh: the medium of the packages
};

struct Component {
    QString id;                     // virglrenderer, rutabaga_gfx, qemu, tools, mesa, kwin, medium
    QString title;                  // "QEMU"
    Kind kind = Kind::Host;
    QString dir;                    // the host/ or guest/ folder it is described in
    QHash<QString, QString> values; // its NAME=value lines
    QStringList deps;
    bool heavy = false;             // takes up to the container's memory cap

    bool isValid() const { return !id.isEmpty(); }
};

struct Patch {
    QString path;                   // in the component's folder: patches/qemu/fork/0001-x.patch
    QString origin;                 // where it comes from: "Research"
    QString note;                   // a spec's comment over its Patch line
    QString number;                 // a spec's: "100" for Patch100

    QString name() const { return path.section('/', -1); }
};

/* Where things are; the tests choose */
struct Places {
    QString hostDir;                // StackBuilder::hostDir()
    QString guestDir;               // GuestToolsBuilder::guestDir()
    QString stackDir;               // Paths::stackDir()
    QString rpmsDir;                // GuestToolsBuilder::rpmsDir()
    QString mediumImage;            // GuestToolsBuilder::mediumImage()

    static Places defaults();
};

/* Where a component comes from, for the window */
struct Source {
    QString url;                    // its repository, or its project's page
    QString revision;               // the pinned commit; a package's Version-Release
    QString archive;                // a package's: kwin-6.7.5.tar.xz
    QString archiveHost;            // download.kde.org
};

/* An input that changed since what is built was */
struct Change {
    enum class What { Source, Script, Spec, Patch, File, Dependency, Unknown };
    enum class How { Changed, Added, Removed };
    What what = What::Unknown;
    How how = How::Changed;
    QString name;                   // fork/0001-x.patch, a dependency's title
    QString was;                    // Source: the commit or URL before
    QString now;
};

/* What is built of a component */
struct Status {
    enum class State { NoSources, NotBuilt, Outdated, UpToDate };
    State state = State::NoSources;
    QList<Change> changes;          // why it is out of date
    QDateTime built;                // of what is built, if anything
    QString output;                 // its folder, or the medium's image
    QString version;                // of what is built: "11.1.50"
};

/* The [sections] of a components.conf, in order, with their NAME=value lines */
QList<std::pair<QString, QHash<QString, QString>>> parseConf(const QByteArray &conf);

/* The components of @places' host/ and guest/, in build order, host/'s first */
QList<Component> load(const Places &places);
/* Of @list: the one called @id, or none */
Component find(const QList<Component> &list, const QString &id);

/*
 * The patches of folder @folder of @dir, in the order build.sh applies
 * them: those its series file names, else its *.patch files and those of
 * its folders, in the C order of their paths
 */
QStringList patchPaths(const QString &dir, const QString &folder);

Source source(const Component &component);
/* Its patches, in the order they apply */
QList<Patch> patches(const Component &component);
/* What @patch does, in a line: its Subject, else the first line of its
   description, else the spec's comment over it, else its name */
QString subject(const Component &component, const Patch &patch);
QString text(const Component &component, const Patch &patch);

/*
 * The lines its key sums up, as its script prints them (build.sh
 * --print-inputs, build-rpms.sh's inputs_hash); empty if an input cannot be
 * read.  The medium's: its packages' keys and build-medium.sh.
 */
QByteArray inputLines(const Component &component, const QList<Component> &all);
/* The sha256 of those lines; empty without them */
QString key(const Component &component, const QList<Component> &all);

Status status(const Component &component, const QList<Component> &all, const Places &places);
/* What changed from the input lines @was to @now: none if they are the
   same, Unknown if @was is empty (a build that did not record them) */
QList<Change> changes(const Component &component, const QList<Component> &all,
                      const QByteArray &was, const QByteArray &now);
/* "Patch fork/0001-x.patch changed" */
QString describe(const Change &change);
/* "2 patches changed, build script changed": the changes in a few words */
QString summary(const QList<Change> &changes);

}
