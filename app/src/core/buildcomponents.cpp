// SPDX-License-Identifier: GPL-2.0-or-later
#include "buildcomponents.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/paths.h"
#include "core/stackbuilder.h"

namespace Builds {

/* What a folder of an older Vitrine builds, without its components.conf */
static const char kHostDefaults[] = R"(
[virglrenderer]
title=virglrenderer
url=VIRGL_URL
commit=VIRGL_COMMIT
patches=patches/virglrenderer
[qemu]
title=QEMU
url=QEMU_URL
commit=QEMU_COMMIT
patches=patches/qemu
deps=virglrenderer
)";
static const char kGuestDefaults[] = R"(
[tools]
title=vitrine-guest-tools
spec=tools/vitrine-guest-tools.spec
[mesa]
title=Mesa
spec=mesa/mesa.spec
patches=spec
heavy=1
[kwin]
title=KWin
spec=kwin/kwin.spec
patches=spec
heavy=1
[medium]
title=Guest tools medium
kind=medium
deps=tools mesa kwin
)";
/* What a patch's description is read from: its start */
static const qint64 kPatchHead = 64 * 1024;

static QString tr(const char *text, const char *disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("Builds", text, disambiguation, n);
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/* sha256sum's hash of the file; empty if it cannot be read */
static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);

    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
        return {};
    }
    return hash.result().toHex();
}

/* NAME=value lines outside any section, as build.sh reads versions.conf:
   the last of a name wins, trailing blanks go */
static QHash<QString, QString> plainValues(const QByteArray &conf)
{
    static const QRegularExpression line("^\\s*([A-Za-z_][A-Za-z0-9_]*)=(.*)$");
    QHash<QString, QString> values;

    for (const QByteArray &raw : conf.split('\n')) {
        const QRegularExpressionMatch m = line.match(QString::fromUtf8(raw));
        if (m.hasMatch()) {
            QString value = m.captured(2);
            while (!value.isEmpty() && value.back().isSpace()) {
                value.chop(1);
            }
            values.insert(m.captured(1), value);
        }
    }
    return values;
}

Places Places::defaults()
{
    return {StackBuilder::hostDir(), GuestToolsBuilder::guestDir(), Paths::stackDir(),
            GuestToolsBuilder::rpmsDir(), GuestToolsBuilder::mediumImage()};
}

QList<std::pair<QString, QHash<QString, QString>>> parseConf(const QByteArray &conf)
{
    static const QRegularExpression section("^\\s*\\[([^\\]]+)\\]\\s*$");
    static const QRegularExpression value("^\\s*([A-Za-z_][A-Za-z0-9_.-]*)\\s*=(.*)$");
    QList<std::pair<QString, QHash<QString, QString>>> sections;

    for (const QByteArray &raw : conf.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        const QString trimmed = line.trimmed();

        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }
        if (const QRegularExpressionMatch m = section.match(line); m.hasMatch()) {
            sections.append({m.captured(1).trimmed(), {}});
        } else if (const QRegularExpressionMatch m = value.match(line);
                   m.hasMatch() && !sections.isEmpty()) {
            sections.last().second.insert(m.captured(1), m.captured(2).trimmed());
        }
    }
    return sections;
}

QList<Component> load(const Places &places)
{
    QList<Component> list;
    const auto add = [&list](const QString &dir, bool host, const char *defaults) {
        QByteArray conf;

        if (dir.isEmpty()) {
            return;
        }
        conf = readFile(dir + "/components.conf");
        if (conf.isEmpty()) {
            conf = defaults;
        }
        for (const auto &[id, values] : parseConf(conf)) {
            Component c;
            bool ok = false;
            const int minutes = values.value("minutes").toInt(&ok);

            c.id = id;
            c.title = values.value("title", id);
            c.dir = dir;
            c.values = values;
            c.kind = host ? Kind::Host
                     : values.value("kind") == "medium" ? Kind::Medium
                                                         : Kind::Package;
            c.deps = values.value("deps").split(' ', Qt::SkipEmptyParts);
            c.minutes = ok && minutes > 0 ? minutes : 10;
            c.heavy = values.value("heavy") == "1";
            /* a package is its spec */
            if (c.kind == Kind::Package && !QFileInfo(dir + '/' + values.value("spec")).isFile()) {
                continue;
            }
            list << c;
        }
    };

    add(places.hostDir, true, kHostDefaults);
    add(places.guestDir, false, kGuestDefaults);
    return list;
}

Component find(const QList<Component> &list, const QString &id)
{
    const auto it = std::find_if(list.begin(), list.end(),
                                 [&id](const Component &c) { return c.id == id; });
    return it == list.end() ? Component() : *it;
}

/*
 * As build.sh's patches_of: the names of FOLDER/series, else the *.patch
 * files of FOLDER and of its folders (not deeper), in the C order of their
 * paths.  Matched with case, as bash's glob is: a .PATCH file is none; and
 * neither matches hidden files or folders.
 */
QStringList patchPaths(const QString &dir, const QString &folder)
{
    const QDir top(dir + '/' + folder);
    QStringList list;
    const auto add = [&list](const QDir &in, const QString &prefix) {
        const QStringList names = in.entryList({"*.patch"}, QDir::Files | QDir::CaseSensitive,
                                               QDir::NoSort);
        for (const QString &name : names) {
            list << prefix + name;
        }
    };

    if (dir.isEmpty() || folder.isEmpty()) {
        return {};
    }
    if (QFileInfo(top.filePath("series")).isFile()) {
        for (const QByteArray &raw : readFile(top.filePath("series")).split('\n')) {
            const QString name = QString::fromUtf8(raw).section('#', 0, 0).trimmed();
            if (!name.isEmpty()) {
                list << folder + '/' + name;
            }
        }
        return list;
    }
    add(top, folder + '/');
    for (const QString &sub : top.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort)) {
        add(QDir(top.filePath(sub)), folder + '/' + sub + '/');
    }
    /* sort's C order: by bytes */
    std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
        return a.toUtf8() < b.toUtf8();
    });
    return list;
}

/* The *.patch files of @folder, in the order @order names them, the others
   after them by name */
static QStringList orderedPatchPaths(const QString &dir, const QString &folder,
                                     const QString &order)
{
    const QString text = QString::fromUtf8(readFile(dir + '/' + order));
    QStringList names = QDir(dir + '/' + folder)
                            .entryList({"*.patch"}, QDir::Files | QDir::CaseSensitive, QDir::Name);
    QStringList list;

    std::stable_sort(names.begin(), names.end(), [&text](const QString &a, const QString &b) {
        const qsizetype i = text.indexOf(a);
        const qsizetype j = text.indexOf(b);
        if ((i < 0) != (j < 0)) {
            return i >= 0;
        }
        return i >= 0 && i < j;
    });
    for (const QString &name : std::as_const(names)) {
        list << folder + '/' + name;
    }
    return list;
}

/* A spec's NAME: value line ("Version"); empty if none */
static QString specValue(const QString &spec, const char *name)
{
    const QRegularExpression line(QString("^%1:\\s*(.*?)\\s*$").arg(name),
                                  QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = line.match(spec);

    return m.hasMatch() ? m.captured(1) : QString();
}

/* The macros of a spec a source's name uses most */
static QString expand(QString text, const QString &spec)
{
    const QString version = specValue(spec, "Version");

    text.replace("%{name}", specValue(spec, "Name"));
    text.replace("%{version}", version);
    /* Mesa's: the version with - for ~ */
    text.replace("%{ver}", QString(version).replace('~', '-'));
    text.remove("%{?dist}");
    return text;
}

Source source(const Component &c)
{
    Source s;

    if (c.kind == Kind::Host) {
        const QHash<QString, QString> versions = plainValues(readFile(c.dir + "/versions.conf"));
        s.url = versions.value(c.values.value("url"));
        s.revision = versions.value(c.values.value("commit"));
    } else if (c.kind == Kind::Package) {
        const QString spec = QString::fromUtf8(readFile(c.dir + '/' + c.values.value("spec")));
        const QString release = expand(specValue(spec, "Release"), spec);
        const QString source0 = specValue(spec, "Source0");
        QString archive = source0.section('/', -1);

        s.url = specValue(spec, "URL");
        s.revision = specValue(spec, "Version");
        if (!release.isEmpty()) {
            s.revision += '-' + release;
        }
        /* crates' "download#/name-1.0.tar.gz" */
        if (source0.contains("#/")) {
            archive = source0.section("#/", -1);
        }
        s.archive = expand(archive, spec);
        s.archiveHost = QUrl(source0).host();
    }
    return s;
}

/* A spec's Patch lines, in their order, with the comment over each */
static QList<Patch> specPatches(const Component &c)
{
    static const QRegularExpression patchLine("^Patch(\\d*)\\s*:\\s*(\\S+)\\s*$");
    const QString specPath = c.values.value("spec");
    const QString folder = specPath.section('/', 0, -2);
    const QString spec = QString::fromUtf8(readFile(c.dir + '/' + specPath));
    QStringList comment;
    QList<Patch> list;

    for (const QString &line : spec.split('\n')) {
        const QString trimmed = line.trimmed();
        /* "## proposed patches": a heading, not about the next patch */
        if (trimmed.startsWith("##")) {
            comment.clear();
            continue;
        }
        if (trimmed.startsWith('#')) {
            comment << trimmed.mid(1).trimmed();
            continue;
        }
        if (const QRegularExpressionMatch m = patchLine.match(trimmed); m.hasMatch()) {
            Patch p;
            p.path = (folder.isEmpty() ? QString() : folder + '/') + expand(m.captured(2), spec);
            p.origin = c.values.value("origin");
            p.number = m.captured(1);
            p.note = comment.join(' ').trimmed();
            list << p;
        }
        comment.clear();
    }
    return list;
}

QList<Patch> patches(const Component &c)
{
    const QString where = c.values.value("patches");
    QStringList paths;
    QList<Patch> list;

    if (where.isEmpty() || c.kind == Kind::Medium) {
        return {};
    }
    if (where == "spec") {
        return specPatches(c);
    }
    paths = c.values.contains("order") ? orderedPatchPaths(c.dir, where, c.values.value("order"))
                                       : patchPaths(c.dir, where);
    for (const QString &path : std::as_const(paths)) {
        const QString inside = path.mid(where.size() + 1);
        const QString sub = inside.contains('/') ? inside.section('/', 0, 0) : QString();
        Patch p;

        p.path = path;
        p.origin = sub.isEmpty() ? c.values.value("origin")
                                 : c.values.value("origin." + sub, sub);
        list << p;
    }
    return list;
}

/* Where a diff starts, after a patch's description */
static bool diffStarts(const QString &line)
{
    return line.startsWith("diff ") || line.startsWith("--- ") || line.startsWith("+++ ") ||
           line.startsWith("Index: ") || line.startsWith("@@ ");
}

QString subject(const Component &c, const Patch &p)
{
    static const QRegularExpression tag("^\\[[^\\]]*\\]\\s*");
    /* git format-patch's first line */
    static const QRegularExpression mbox("^From [0-9a-f]{40} ");
    QFile file(c.dir + '/' + p.path);

    if (file.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(file.read(kPatchHead)).split('\n');

        for (qsizetype i = 0; i < lines.size() && !diffStarts(lines[i]); i++) {
            if (!lines[i].startsWith("Subject:")) {
                continue;
            }
            QString text = lines[i].mid(8).trimmed();
            /* a long one goes on, indented */
            while (i + 1 < lines.size() && !lines[i + 1].isEmpty() &&
                   (lines[i + 1].front() == ' ' || lines[i + 1].front() == '\t')) {
                text += ' ' + lines[++i].trimmed();
            }
            text.remove(tag);
            if (!text.isEmpty()) {
                return text;
            }
        }
        /* a description without mail headers: its first line */
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (diffStarts(line)) {
                break;
            }
            if (trimmed.isEmpty() || trimmed == "---" || mbox.match(line).hasMatch() ||
                line.startsWith("From:") || line.startsWith("Date:")) {
                continue;
            }
            return trimmed;
        }
    }
    if (!p.note.isEmpty()) {
        return p.note;
    }
    QString name = p.name();
    return name.endsWith(".patch") ? name.chopped(6) : name;
}

QString text(const Component &c, const Patch &p)
{
    return QString::fromUtf8(readFile(c.dir + '/' + p.path));
}

QByteArray inputLines(const Component &c, const QList<Component> &all)
{
    QByteArray lines;

    switch (c.kind) {
    case Kind::Host: {
        const QHash<QString, QString> versions = plainValues(readFile(c.dir + "/versions.conf"));
        const QString url = versions.value(c.values.value("url"));
        const QString commit = versions.value(c.values.value("commit"));
        const QStringList files =
            QStringList{"build.sh"} + patchPaths(c.dir, c.values.value("patches"));

        if (url.isEmpty() || commit.isEmpty()) {
            return {};
        }
        lines = "source " + url.toUtf8() + ' ' + commit.toUtf8() + '\n';
        for (const QString &file : files) {
            const QByteArray hash = fileHash(c.dir + '/' + file);
            if (hash.isEmpty()) {
                return {};
            }
            lines += hash + "  " + file.toUtf8() + '\n';
        }
        for (const QString &id : c.deps) {
            const QString depKey = key(find(all, id), all);
            if (depKey.isEmpty()) {
                return {};
            }
            lines += "dep " + id.toUtf8() + ' ' + depKey.toLatin1() + '\n';
        }
        return lines;
    }
    case Kind::Package:
        return GuestToolsBuilder::inputLines(c.dir, c.id);
    case Kind::Medium: {
        const QByteArray script = fileHash(c.dir + "/build-medium.sh");

        /* the packages there are: a medium has those built */
        for (const QString &id : c.deps) {
            const Component package = find(all, id);
            if (package.isValid()) {
                const QString packageKey = key(package, all);
                if (packageKey.isEmpty()) {
                    return {};
                }
                lines += "dep " + id.toUtf8() + ' ' + packageKey.toLatin1() + '\n';
            }
        }
        if (script.isEmpty()) {
            return {};
        }
        return lines + script + "  build-medium.sh\n";
    }
    }
    return {};
}

QString key(const Component &c, const QList<Component> &all)
{
    const QByteArray lines = c.isValid() ? inputLines(c, all) : QByteArray();

    return lines.isEmpty() ? QString()
                           : QString::fromLatin1(
                                 QCryptographicHash::hash(lines, QCryptographicHash::Sha256).toHex());
}

/* The input lines of a build, parsed */
struct Lines {
    QString source;                             // "URL COMMIT"
    QList<std::pair<QString, QString>> files;   // path, hash, in their order
    QList<std::pair<QString, QString>> deps;    // id, key

    static Lines parse(const QByteArray &text)
    {
        static const QRegularExpression file("^([0-9a-f]{64})  (.+)$");
        Lines lines;

        for (const QByteArray &raw : text.split('\n')) {
            const QString line = QString::fromUtf8(raw);
            if (line.startsWith("source ")) {
                lines.source = line.mid(7);
            } else if (line.startsWith("dep ")) {
                lines.deps.append({line.section(' ', 1, 1), line.section(' ', 2, 2)});
            } else if (const QRegularExpressionMatch m = file.match(line); m.hasMatch()) {
                lines.files.append({m.captured(2), m.captured(1)});
            }
        }
        return lines;
    }

    QString fileHash(const QString &path) const
    {
        for (const auto &[name, hash] : files) {
            if (name == path) {
                return hash;
            }
        }
        return {};
    }
};

/* What a file input of @c is: a patch, its spec, its build script... */
static Change fileChange(const Component &c, const QString &path, Change::How how)
{
    Change change;
    const QString patchFolder = c.values.value("patches");

    change.how = how;
    change.name = path;
    if (path == "build.sh" || path == "rpm-build-inside.sh" || path == "build-medium.sh") {
        change.what = Change::What::Script;
    } else if (path.endsWith(".spec")) {
        change.what = Change::What::Spec;
    } else if (path.endsWith(".patch")) {
        change.what = Change::What::Patch;
        /* by its path in the component's patches: fork/0001-x.patch */
        if (c.kind == Kind::Host && path.startsWith(patchFolder + '/')) {
            change.name = path.mid(patchFolder.size() + 1);
        } else {
            change.name = path.section('/', -1);
        }
    } else {
        change.what = Change::What::File;
    }
    return change;
}

QList<Change> changes(const Component &c, const QList<Component> &all, const QByteArray &was,
                      const QByteArray &now)
{
    const Lines before = Lines::parse(was);
    const Lines after = Lines::parse(now);
    QList<Change> list;

    if (was.trimmed().isEmpty()) {
        return {Change()};
    }
    if (before.source != after.source) {
        Change change;
        const QString url = after.source.section(' ', 0, 0);
        const QString commit = after.source.section(' ', 1);
        change.what = Change::What::Source;
        /* moved to another commit, or fetched from elsewhere */
        if (before.source.section(' ', 0, 0) == url) {
            change.was = before.source.section(' ', 1);
            change.now = commit;
        } else {
            change.was = before.source.section(' ', 0, 0);
            change.now = url;
            change.name = QStringLiteral("url");
        }
        list << change;
    }
    for (const auto &[path, hash] : after.files) {
        const QString old = before.fileHash(path);
        if (old.isEmpty()) {
            list << fileChange(c, path, Change::How::Added);
        } else if (old != hash) {
            list << fileChange(c, path, Change::How::Changed);
        }
    }
    for (const auto &[path, hash] : before.files) {
        if (after.fileHash(path).isEmpty()) {
            list << fileChange(c, path, Change::How::Removed);
        }
    }
    for (const auto &[id, depKey] : after.deps) {
        const auto old = std::find_if(before.deps.begin(), before.deps.end(),
                                      [&id](const auto &dep) { return dep.first == id; });
        if (old == before.deps.end() || old->second != depKey) {
            Change change;
            const Component dep = find(all, id);
            change.what = Change::What::Dependency;
            change.how = old == before.deps.end() ? Change::How::Added : Change::How::Changed;
            change.name = dep.isValid() ? dep.title : id;
            list << change;
        }
    }
    /* the same files in another order: the patches apply in another one */
    if (list.isEmpty() && before.files != after.files) {
        Change change;
        change.what = Change::What::Patch;
        change.name = QStringLiteral("order");
        list << change;
    }
    return list;
}

QString describe(const Change &change)
{
    switch (change.what) {
    case Change::What::Source:
        return change.name == "url"
                   ? tr("Fetched from %1 now, %2 before").arg(change.now, change.was)
                   : tr("Pinned to another commit: %1, %2 before")
                         .arg(change.now.left(12), change.was.left(12));
    case Change::What::Script:
        return tr("Its build script %1 changed").arg(change.name);
    case Change::What::Spec:
        return change.how == Change::How::Changed ? tr("Its spec %1 changed").arg(change.name)
                                                  : tr("New spec %1").arg(change.name);
    case Change::What::Patch:
        if (change.name == "order") {
            return tr("Its patches apply in another order");
        }
        switch (change.how) {
        case Change::How::Changed:
            return tr("Patch %1 changed").arg(change.name);
        case Change::How::Added:
            return tr("New patch %1").arg(change.name);
        case Change::How::Removed:
            return tr("Patch %1 removed").arg(change.name);
        }
        break;
    case Change::What::File:
        switch (change.how) {
        case Change::How::Changed:
            return tr("%1 changed").arg(change.name);
        case Change::How::Added:
            return tr("New file %1").arg(change.name);
        case Change::How::Removed:
            return tr("%1 removed").arg(change.name);
        }
        break;
    case Change::What::Dependency:
        /* one it has now, which what is built was made without (a package
           not built yet, which the medium has not) */
        return change.how == Change::How::Added ? tr("Built without %1").arg(change.name)
                                                : tr("%1 changed").arg(change.name);
    case Change::What::Unknown:
        break;
    }
    return tr("Built from other sources");
}

QString summary(const QList<Change> &changes)
{
    int patches = 0;
    int added = 0;
    int removed = 0;
    int files = 0;
    QStringList parts;

    for (const Change &change : changes) {
        switch (change.what) {
        case Change::What::Source:
            parts.prepend(change.name == "url" ? tr("another repository")
                                               : tr("another commit"));
            break;
        case Change::What::Patch:
            if (change.name == "order") {
                parts << tr("patches reordered");
            } else {
                (change.how == Change::How::Added     ? added
                 : change.how == Change::How::Removed ? removed
                                                      : patches)++;
            }
            break;
        case Change::What::Spec:
            parts << tr("spec changed");
            break;
        case Change::What::Script:
            parts << tr("build script changed");
            break;
        case Change::What::File:
            files++;
            break;
        case Change::What::Dependency:
            parts << (change.how == Change::How::Added ? tr("without %1").arg(change.name)
                                                       : tr("%1 changed").arg(change.name));
            break;
        case Change::What::Unknown:
            parts << tr("built from other sources");
            break;
        }
    }
    if (patches > 0) {
        parts << (patches == 1 ? tr("1 patch changed") : tr("%1 patches changed").arg(patches));
    }
    if (added > 0) {
        parts << (added == 1 ? tr("1 new patch") : tr("%1 new patches").arg(added));
    }
    if (removed > 0) {
        parts << (removed == 1 ? tr("1 patch removed") : tr("%1 patches removed").arg(removed));
    }
    if (files > 0) {
        parts << (files == 1 ? tr("1 file changed") : tr("%1 files changed").arg(files));
    }
    parts.removeDuplicates();
    return parts.join(", ");
}

/* The version of the packages in @dir: of an RPM of them, NAME-VERSION-RELEASE.ARCH.rpm */
static QString packageVersion(const QString &dir)
{
    static const QRegularExpression rpm("^(.+)-([^-]+)-([^-]+)\\.([^.]+)\\.rpm$");

    for (const QString &name : QDir(dir).entryList({"*.rpm"}, QDir::Files, QDir::Name)) {
        if (const QRegularExpressionMatch m = rpm.match(name);
            m.hasMatch() && !m.captured(1).endsWith("-debuginfo") &&
            !m.captured(1).endsWith("-debugsource")) {
            return m.captured(2);
        }
    }
    return {};
}

static Status hostStatus(const Component &c, const QList<Component> &all, const Places &places)
{
    const QByteArray now = inputLines(c, all);
    const QString componentKey = key(c, all);
    const StackBuilder::Build current = StackBuilder::current(places.stackDir);
    Status s;

    if (componentKey.isEmpty()) {
        return s;
    }
    s.state = Status::State::NotBuilt;
    if (c.id == "qemu") {
        if (!current.isValid()) {
            return s;
        }
        s.built = current.built;
        s.output = current.prefix;
        s.version = current.qemuVersion;
        if (current.stamp == componentKey) {
            s.state = Status::State::UpToDate;
            return s;
        }
        s.state = Status::State::Outdated;
        s.changes = changes(c, all, readFile(current.prefix + "/share/vitrine/qemu.inputs"), now);
        return s;
    }
    /* a library of the stack: its folder for these inputs, else the one the
       current QEMU is built against */
    const QString folder = places.stackDir + '/' + c.id + '/' + componentKey.left(16);
    const QString conf = "/share/vitrine/" + c.id + ".conf";
    QHash<QString, QString> built = plainValues(readFile(folder + conf));
    if (built.value("KEY") == componentKey) {
        s.state = Status::State::UpToDate;
        s.output = folder;
        s.built = QDateTime::fromString(built.value("BUILT"), Qt::ISODate);
        s.version = built.value("VERSION");
        return s;
    }
    if (!current.isValid()) {
        return s;
    }
    s.state = Status::State::Outdated;
    if (c.id == "virglrenderer" && !current.virglPrefix.isEmpty()) {
        built = plainValues(readFile(current.virglPrefix + conf));
        s.output = current.virglPrefix;
        s.built = QDateTime::fromString(built.value("BUILT"), Qt::ISODate);
        s.version = built.value("VERSION");
        s.changes = changes(c, all,
                            readFile(current.virglPrefix + "/share/vitrine/" + c.id + ".inputs"),
                            now);
        return s;
    }
    /* built into QEMU's own folder, before each had its own: what changed is not known */
    s.output = current.prefix + "/lib64";
    s.built = current.built;
    s.version = c.id == "virglrenderer" ? current.virglVersion : QString();
    s.changes = {Change()};
    return s;
}

static Status packageStatus(const Component &c, const QList<Component> &all,
                            const Places &places)
{
    const QByteArray now = inputLines(c, all);
    const QString dir = places.rpmsDir + '/' + c.id;
    const QString built = GuestToolsBuilder::builtInputs(places.rpmsDir, c.id);
    Status s;

    if (now.isEmpty()) {
        return s;
    }
    s.state = Status::State::NotBuilt;
    if (built.isEmpty()) {
        return s;
    }
    s.output = dir;
    s.built = QFileInfo(dir + "/.inputs").lastModified();
    s.version = packageVersion(dir);
    if (built == key(c, all)) {
        s.state = Status::State::UpToDate;
        return s;
    }
    s.state = Status::State::Outdated;
    s.changes = changes(c, all, readFile(dir + "/.inputs.list"), now);
    return s;
}

static Status mediumStatus(const Component &c, const QList<Component> &all, const Places &places)
{
    const QByteArray now = inputLines(c, all);
    const QByteArray manifest = readFile(GuestToolsBuilder::manifestOf(places.mediumImage));
    const GuestTools::Medium medium = GuestTools::parseManifest(manifest, places.mediumImage);
    Status s;
    QByteArray was;

    if (now.isEmpty()) {
        return s;
    }
    s.state = Status::State::NotBuilt;
    if (!QFileInfo::exists(places.mediumImage) || !medium.isValid()) {
        return s;
    }
    const GuestToolsBuilder::Made made = GuestToolsBuilder::made(places.mediumImage);
    s.output = places.mediumImage;
    s.built = made.built;
    s.version = medium.tools;
    /* an older Vitrine's, which did not say what it was made of */
    if (made.inputs.isEmpty()) {
        s.state = Status::State::Outdated;
        s.changes = {Change()};
        return s;
    }
    /* what it was made of, as lines: its packages' keys, then the script's */
    for (const QString &id : c.deps) {
        if (made.inputs.contains(id)) {
            was += "dep " + id.toUtf8() + ' ' + made.inputs.value(id).toLatin1() + '\n';
        }
    }
    was += made.inputs.value("medium").toLatin1() + "  build-medium.sh\n";
    s.changes = changes(c, all, was, now);
    s.state = s.changes.isEmpty() ? Status::State::UpToDate : Status::State::Outdated;
    return s;
}

Status status(const Component &c, const QList<Component> &all, const Places &places)
{
    switch (c.kind) {
    case Kind::Host:
        return hostStatus(c, all, places);
    case Kind::Package:
        return packageStatus(c, all, places);
    case Kind::Medium:
        return mediumStatus(c, all, places);
    }
    return {};
}

}
