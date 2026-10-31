// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkde.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>
#include <QtConcurrent>

#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#include "core/paths.h"
#include "core/stackbuilder.h"

/* What a query may take: dnf loads its configuration first */
static const int kQueryMs = 30000;

static QString tr(const char *text)
{
    return QCoreApplication::translate("HostKde", text);
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/* @program's output; its status in @status (-1: it did not run) */
static QByteArray run(const QString &program, const QStringList &args, int *status = nullptr)
{
    QProcess p;

    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args);
    if (!p.waitForStarted() || !p.waitForFinished(kQueryMs)) {
        p.kill();
        p.waitForFinished(1000);
        if (status) {
            *status = -1;
        }
        return {};
    }
    if (status) {
        *status = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    }
    return p.readAllStandardOutput();
}

/* sha256sum's hash of a file; empty if it cannot be read */
static QString fileHash(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);

    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

/* "6.11.2" of "6.11.2-3.fc44" or "1:6.11.2-3.fc44" */
static QString versionOfEvr(const QString &evr)
{
    QString v = evr.section(':', -1);
    return v.section('-', 0, 0);
}

/* NAME=value lines, the last of a name winning, those that repeat (rpm=,
   digest=) in @lists */
static QHash<QString, QString> confValues(const QByteArray &conf,
                                          QHash<QString, QStringList> *lists = nullptr)
{
    QHash<QString, QString> values;

    for (const QByteArray &raw : conf.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        const qsizetype eq = line.indexOf('=');
        if (line.isEmpty() || line.startsWith('#') || eq <= 0) {
            continue;
        }
        const QString name = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        values.insert(name, value);
        if (lists) {
            (*lists)[name] << value;
        }
    }
    return values;
}

/* Debug information, which an install leaves alone and whose repository is seldom enabled */
static bool isDebug(const QString &name)
{
    return name.endsWith("-debuginfo") || name.endsWith("-debugsource");
}

/* rpm's rpmvercmp: the segments of digits and of letters compared in turn,
   ~ before anything, ^ after the end of the other */
static int rpmvercmp(const QByteArray &a, const QByteArray &b)
{
    const auto alnum = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const char *one = a.constData();
    const char *two = b.constData();

    if (a == b) {
        return 0;
    }
    while (*one || *two) {
        while (*one && !alnum(*one) && *one != '~' && *one != '^') {
            one++;
        }
        while (*two && !alnum(*two) && *two != '~' && *two != '^') {
            two++;
        }
        if (*one == '~' || *two == '~') {
            if (*one != '~') {
                return 1;
            }
            if (*two != '~') {
                return -1;
            }
            one++;
            two++;
            continue;
        }
        if (*one == '^' || *two == '^') {
            if (!*one) {
                return -1;
            }
            if (!*two) {
                return 1;
            }
            if (*one != '^') {
                return 1;
            }
            if (*two != '^') {
                return -1;
            }
            one++;
            two++;
            continue;
        }
        if (!(*one && *two)) {
            break;
        }
        const char *end1 = one;
        const char *end2 = two;
        const bool number = digit(*end1);
        if (number) {
            while (*end1 && digit(*end1)) {
                end1++;
            }
            while (*end2 && digit(*end2)) {
                end2++;
            }
        } else {
            while (*end1 && alpha(*end1)) {
                end1++;
            }
            while (*end2 && alpha(*end2)) {
                end2++;
            }
        }
        /* a number against letters: the number is newer */
        if (two == end2) {
            return number ? 1 : -1;
        }
        if (number) {
            while (*one == '0') {
                one++;
            }
            while (*two == '0') {
                two++;
            }
            if (end1 - one != end2 - two) {
                return end1 - one > end2 - two ? 1 : -1;
            }
        }
        const int c = std::string(one, end1).compare(std::string(two, end2));
        if (c != 0) {
            return c < 0 ? -1 : 1;
        }
        one = end1;
        two = end2;
    }
    if (!*one && !*two) {
        return 0;
    }
    return *one ? 1 : -1;
}

namespace HostKde {

static bool isSourceDir(const QString &dir)
{
    return !dir.isEmpty() && QFileInfo(dir + "/build.sh").isFile() &&
           QFileInfo(dir + "/install.sh").isFile() && QFileInfo(dir + "/components.conf").isFile();
}

QString sourceDir()
{
    const QString chosen = qEnvironmentVariable("VITRINE_HOST_KDE_DIR");

    if (!chosen.isEmpty()) {
        return isSourceDir(chosen) ? QFileInfo(chosen).canonicalFilePath() : QString();
    }
    const QString host = StackBuilder::hostDir();
    return isSourceDir(host + "/kde") ? QFileInfo(host + "/kde").canonicalFilePath() : QString();
}

QString dataDir()
{
    return Paths::dataDir();
}

QString workDir()
{
    return Paths::cacheDir() + "/host-kde-build";
}

QString oldWorkDir()
{
    return Paths::cacheDir() + "/host-kwin-build";
}

QList<Target> Target::parse(const QByteArray &conf)
{
    static const QRegularExpression section("^\\s*\\[([^\\]]+)\\]\\s*$");
    QList<std::pair<QString, QHash<QString, QString>>> sections;
    QList<Target> list;

    for (const QByteArray &raw : conf.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        const QString trimmed = line.trimmed();
        const qsizetype eq = trimmed.indexOf('=');
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }
        if (const QRegularExpressionMatch m = section.match(line); m.hasMatch()) {
            sections.append({m.captured(1).trimmed(), {}});
        } else if (eq > 0 && !sections.isEmpty()) {
            sections.last().second.insert(trimmed.left(eq).trimmed(), trimmed.mid(eq + 1).trimmed());
        }
    }
    for (const auto &[id, values] : sections) {
        const QStringList follows = values.value("follows").split(' ', Qt::SkipEmptyParts);
        Target t;
        if (values.value("kind") != "system" || values.value("package").isEmpty()) {
            continue;
        }
        t.id = id;
        t.title = values.value("title", id);
        t.package = values.value("package");
        t.follows = follows.value(0);
        t.parts = qMax(1, follows.value(1, "3").toInt());
        t.against = values.value("against").split(' ', Qt::SkipEmptyParts);
        t.patches = values.value("patches");
        t.heavy = values.value("heavy") == "1";
        list << t;
    }
    return list;
}

QList<Target> Target::read(const QString &sourceDir)
{
    return sourceDir.isEmpty() ? QList<Target>() : parse(readFile(sourceDir + "/components.conf"));
}

Target target(const QList<Target> &targets, const QString &id)
{
    const auto it = std::find_if(targets.begin(), targets.end(),
                                 [&id](const Target &t) { return t.id == id; });
    return it == targets.end() ? Target() : *it;
}

QString release(const QString &version, int parts)
{
    return version.section('.', 0, qMax(1, parts) - 1);
}

int compareEvr(const QString &a, const QString &b)
{
    const auto epoch = [](const QString &evr) {
        return evr.contains(':') ? evr.section(':', 0, 0).toLongLong() : 0;
    };
    const QString av = a.section(':', -1);
    const QString bv = b.section(':', -1);

    if (epoch(a) != epoch(b)) {
        return epoch(a) > epoch(b) ? 1 : -1;
    }
    /* VERSION-RELEASE: the release counts only where both have one */
    const bool releases = av.contains('-') && bv.contains('-');
    const int versions = rpmvercmp(releases ? av.section('-', 0, -2).toUtf8() : av.toUtf8(),
                                   releases ? bv.section('-', 0, -2).toUtf8() : bv.toUtf8());
    if (versions != 0 || !releases) {
        return versions;
    }
    return rpmvercmp(av.section('-', -1).toUtf8(), bv.section('-', -1).toUtf8());
}

QString packageTitle(const QString &name)
{
    static const QHash<QString, QString> titles = {
        {"plasma-workspace", "Plasma"},       {"qt6-qtbase", "Qt"},
        {"qt6-qtdeclarative", "Qt Declarative"}, {"kdecoration", "KDecoration"},
        {"kf6-kcoreaddons", "KDE Frameworks"}, {"kf6-ksvg", "KSvg"},
        {"kf6-kirigami", "Kirigami"},         {"kwin", "KWin"},
    };
    return titles.value(name, name);
}

QString versionText(const QString &pattern)
{
    return QString(pattern).replace('*', 'x');
}

QList<Set> sets(const QString &sourceDir, const QString &package)
{
    /* host/kwin/6.7.91) echo "hl39 hl39" ;;   # a comment */
    const QString prefix = "host/" + package + '/';
    const QRegularExpression line("^\\s*(host/" + QRegularExpression::escape(package) +
                                  "/[^)]*)\\)\\s*echo\\s+\"([^\"]*)\"");
    /* ${VGD_HOST_KWIN_SET:-hl39y}: the research's override, which build.sh leaves unset */
    static const QRegularExpression fallback("\\$\\{[A-Za-z_][A-Za-z0-9_]*:-([^}]*)\\}");
    QList<Set> list;

    for (const QByteArray &raw : readFile(sourceDir + "/research/setup/versions.sh").split('\n')) {
        const QRegularExpressionMatch m = line.match(QString::fromUtf8(raw));
        if (!m.hasMatch()) {
            continue;
        }
        const QStringList set =
            m.captured(2).replace(fallback, "\\1").split(' ', Qt::SkipEmptyParts);
        QStringList versions;
        for (const QString &pattern : m.captured(1).split('|')) {
            if (pattern.trimmed().startsWith(prefix)) {
                versions << pattern.trimmed().mid(prefix.size());
            }
        }
        const auto it = std::find_if(list.begin(), list.end(),
                                     [&set](const Set &s) { return s.series == set.value(0); });
        if (it != list.end()) {
            it->versions << versions;
        } else if (!set.isEmpty()) {
            list << Set{set.value(0), set.value(1), versions};
        }
    }
    return list;
}

QString Package::evr() const
{
    return version + '-' + release;
}

QString Package::nevra() const
{
    return name + '-' + evr() + '.' + arch;
}

bool Package::isLocal() const
{
    return repo == "@commandline" || repo == "<unknown>";
}

QString Package::source() const
{
    QString base = sourceRpm;
    for (const char *suffix : {".src.rpm", ".nosrc.rpm"}) {
        if (base.endsWith(QLatin1String(suffix))) {
            base.chop(int(qstrlen(suffix)));
            break;
        }
    }
    return base.count('-') >= 2 ? base.section('-', 0, -3) : QString();
}

Package System::package(const QString &name) const
{
    for (const Package &p : packages) {
        if (p.name == name) {
            return p;
        }
    }
    return {};
}

QList<Package> System::of(const QString &source) const
{
    QList<Package> list;

    for (const Package &p : packages) {
        if (p.source() == source && !isDebug(p.name)) {
            list << p;
        }
    }
    return list;
}

QString System::plasma() const
{
    return package("plasma-workspace").version;
}

bool System::isPlasmaHost() const
{
    return rpm && package("plasma-workspace").isValid();
}

QString rpmFormat()
{
    return QStringLiteral("%{NAME}\\t%{VERSION}\\t%{RELEASE}\\t%{ARCH}\\t%{SHA256HEADER}\\t%{VENDOR}"
                          "\\t%{SOURCERPM}\\t%{INSTALLTIME}\\n");
}

QList<Package> System::parsePackages(const QByteArray &rpm, const QByteArray &repos)
{
    QHash<QString, QString> from;
    QList<Package> list;

    for (const QByteArray &raw : repos.split('\n')) {
        const QStringList fields = QString::fromUtf8(raw).simplified().split(' ');
        if (fields.size() == 2 && !from.contains(fields[0])) {
            from.insert(fields[0], fields[1]);
        }
    }
    for (const QByteArray &raw : rpm.split('\n')) {
        /* "package x is not installed": no tabs */
        const QStringList f = QString::fromUtf8(raw).split('\t');
        if (f.size() != 8 || f[0].isEmpty()) {
            continue;
        }
        Package p;
        p.name = f[0];
        p.version = f[1];
        p.release = f[2];
        p.arch = f[3];
        p.digest = f[4] == "(none)" ? QString() : f[4];
        p.vendor = f[5] == "(none)" ? QString() : f[5];
        p.sourceRpm = f[6];
        p.repo = from.value(p.name);
        bool ok = false;
        const qint64 when = f[7].toLongLong(&ok);
        if (ok && when > 0) {
            p.installed = QDateTime::fromSecsSinceEpoch(when, QTimeZone::UTC);
        }
        list << p;
    }
    return list;
}

QHash<QString, QString> System::parseLocks(const QByteArray &list)
{
    static const QRegularExpression name("^Package name:\\s*(\\S+)\\s*$");
    static const QRegularExpression evr("^evr\\s*=\\s*(\\S+)\\s*$");
    QHash<QString, QString> locks;
    QString current;

    for (const QByteArray &raw : list.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (const QRegularExpressionMatch m = name.match(line); m.hasMatch()) {
            current = m.captured(1);
            if (!locks.contains(current)) {
                /* an entry of another comparator than = locks it too */
                locks.insert(current, QString());
            }
        } else if (const QRegularExpressionMatch m = evr.match(line);
                   m.hasMatch() && !current.isEmpty()) {
            /* dnf5 writes some with their epoch, 0 too */
            QString value = m.captured(1);
            if (value.startsWith("0:")) {
                value.remove(0, 2);
            }
            locks.insert(current, value);
        }
    }
    return locks;
}

QHash<QString, QString> System::parseNewest(const QByteArray &list, const QList<Package> &installed)
{
    QHash<QString, QString> newest;

    for (const QByteArray &raw : list.split('\n')) {
        const QStringList f = QString::fromUtf8(raw).simplified().split(' ');
        if (f.size() != 3) {
            continue;
        }
        /* of the arch installed (the repositories have i686 builds too) */
        const auto it = std::find_if(installed.begin(), installed.end(),
                                     [&f](const Package &p) { return p.name == f[0]; });
        if (it == installed.end() || (it->arch != f[1] && f[1] != "noarch")) {
            continue;
        }
        if (!newest.contains(f[0]) || compareEvr(f[2], newest.value(f[0])) > 0) {
            newest.insert(f[0], f[2]);
        }
    }
    return newest;
}

/* os-release's VERSION_ID, on Fedora only */
static QString fedoraRelease(const QString &path)
{
    QString id;
    QString version;

    for (const QByteArray &raw : readFile(path).split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        QString value = line.section('=', 1);
        if (value.size() >= 2 && (value.startsWith('"') || value.startsWith('\''))) {
            value = value.mid(1, value.size() - 2);
        }
        if (line.startsWith("ID=")) {
            id = value;
        } else if (line.startsWith("VERSION_ID=")) {
            version = value;
        }
    }
    return id == "fedora" ? version : QString();
}

QDateTime sessionStart(const QString &proc)
{
    const long ticks = ::sysconf(_SC_CLK_TCK);
    qint64 boot = -1;
    qint64 newest = -1;

    for (const QByteArray &raw : readFile(proc + "/stat").split('\n')) {
        if (raw.startsWith("btime ")) {
            boot = raw.mid(6).trimmed().toLongLong();
        }
    }
    if (boot < 0 || ticks <= 0) {
        return {};
    }
    const QStringList pids = QDir(proc).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        struct stat st;
        bool number = false;
        pid.toLongLong(&number);
        if (!number || readFile(proc + '/' + pid + "/comm").trimmed() != "kwin_wayland" ||
            ::stat(QFile::encodeName(proc + '/' + pid).constData(), &st) != 0 ||
            st.st_uid != ::getuid()) {
            continue;
        }
        /* after the name in parentheses, which may hold anything: field 22, the start */
        const QByteArray line = readFile(proc + '/' + pid + "/stat");
        const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 1).simplified().split(' ');
        if (fields.size() > 19) {
            newest = qMax(newest, fields[19].toLongLong());
        }
    }
    return newest < 0 ? QDateTime() : QDateTime::fromSecsSinceEpoch(boot + newest / ticks, QTimeZone::UTC);
}

System System::read(const QList<Target> &targets, const QString &osRelease, const QString &proc)
{
    const QString rpm = QStandardPaths::findExecutable("rpm");
    const QString dnf = QStandardPaths::findExecutable("dnf");
    QStringList patterns;
    QStringList sources;
    QStringList others = {"plasma-workspace"};
    QStringList mains;
    System s;
    int status = -1;

    s.pkexec = !QStandardPaths::findExecutable("pkexec").isEmpty();
    s.fedora = fedoraRelease(osRelease);
    if (rpm.isEmpty() || dnf.isEmpty() || !run(dnf, {"--version"}).startsWith("dnf5")) {
        return s;
    }
    s.rpm = true;
    for (const Target &t : targets) {
        /* a source package's packages are named after it (Fedora's specs) */
        patterns << t.package + '*';
        sources << t.package;
        mains << t.package;
        others << t.follows << t.against;
    }
    others.removeAll(QString());
    others.removeDuplicates();
    const QByteArray packages = run(rpm, QStringList{"-qa", "--qf", rpmFormat()} + patterns + others);
    /* the local cache only: nothing downloaded, nothing written */
    const QByteArray repos = run(dnf, QStringList{"-q", "-C", "repoquery", "--installed", "--qf",
                                                  "%{name} %{from_repo}\\n", "plasma-workspace"} +
                                          mains);
    const QByteArray locks = run(dnf, {"versionlock", "list"}, &status);
    s.versionlock = status == 0;
    for (const Package &p : parsePackages(packages, repos)) {
        if ((sources.contains(p.source()) && !isDebug(p.name)) || others.contains(p.name)) {
            if (!std::any_of(s.packages.begin(), s.packages.end(),
                             [&p](const Package &q) { return q.name == p.name; })) {
                s.packages << p;
            }
        }
    }
    s.locks = parseLocks(locks);
    /* what the repositories have, even what a lock keeps out (versionlock does
       not filter a query); a cache never filled says nothing */
    if (!mains.isEmpty()) {
        s.newest = parseNewest(run(dnf, QStringList{"-q", "-C", "repoquery", "--latest-limit=1", "--qf",
                                                    "%{name} %{arch} %{version}-%{release}\\n"} +
                                            mains),
                               s.packages);
    }
    s.session = sessionStart(proc);
    return s;
}

Recipe Recipe::parse(const QByteArray &output)
{
    const QHash<QString, QString> values = confValues(output);
    Recipe r;

    r.fedora = values.value("fedora");
    r.copr = values.value("copr");
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (it.key().startsWith("set.")) {
            r.sets.insert(it.key().mid(4), it.value().split(' ', Qt::SkipEmptyParts));
        } else if (it.key().startsWith("key.") && !it.value().isEmpty()) {
            r.keys.insert(it.key().mid(4), it.value());
        }
    }
    return r;
}

Recipe Recipe::read(const QString &sourceDir, const QString &fedora,
                    const QHash<QString, QString> &versions)
{
    const QString bash = QStandardPaths::findExecutable("bash");
    QStringList args = {sourceDir + "/build.sh", "--print-recipe", fedora};
    QStringList packages = versions.keys();
    int status = -1;

    if (sourceDir.isEmpty() || fedora.isEmpty() || versions.isEmpty()) {
        return {};
    }
    std::sort(packages.begin(), packages.end());
    for (const QString &package : std::as_const(packages)) {
        if (!versions.value(package).isEmpty()) {
            args << package + '=' + versions.value(package);
        }
    }
    const QByteArray out = run(bash.isEmpty() ? QStringLiteral("/bin/bash") : bash, args, &status);
    return status == 0 ? parse(out) : Recipe();
}

Build::Rpm Build::rpm(const QString &name) const
{
    for (const Rpm &r : rpms) {
        if (r.name == name) {
            return r;
        }
    }
    return {};
}

/* @evr without the set's tag in its release: what the source package had */
static QString untagged(const QString &evr, const QString &tag)
{
    const QString version = evr.section('-', 0, -2);
    QString rel = evr.section('-', -1);

    if (tag.isEmpty() || !evr.contains('-')) {
        return evr;
    }
    const QStringList parts = rel.split('.');
    const qsizetype at = parts.indexOf(tag, 1);
    if (at > 0) {
        QStringList kept = parts;
        kept.removeAt(at);
        rel = kept.join('.');
    }
    return version + '-' + rel;
}

QString Build::sourceEvr() const
{
    return untagged(evr, tag);
}

Build Build::parse(const QString &dir, const QByteArray &conf, const QByteArray &buildroot)
{
    QHash<QString, QStringList> lists;
    const QHash<QString, QString> values = confValues(conf, &lists);
    Build b;

    b.dir = dir;
    /* an older Vitrine built KWin only, and said nothing */
    b.package = values.value("package", "kwin");
    b.evr = values.value("evr");
    b.version = values.value("version");
    b.series = values.value("set");
    b.tag = values.value("tag");
    b.plasma = values.value("plasma");
    b.fedora = values.value("fedora");
    b.key = values.value("key");
    b.research = values.value("research");
    b.built = QDateTime::fromString(values.value("built"), Qt::ISODate);
    for (const QString &line : lists.value("rpm")) {
        const QStringList f = line.split(' ', Qt::SkipEmptyParts);
        if (f.size() == 4) {
            b.rpms << Rpm{f[0], f[1], f[2], f[3]};
        }
    }
    for (const QByteArray &raw : buildroot.split('\n')) {
        const QStringList f = QString::fromUtf8(raw).simplified().split(' ');
        if (f.size() == 2) {
            b.buildroot.insert(f[0], f[1]);
        }
    }
    return b;
}

Build Build::read(const QString &dir)
{
    const QByteArray conf = readFile(dir + "/build.conf");
    const QString package = confValues(conf).value("package", "kwin");
    const QString folder = dir + '/' + package;
    const QStringList roots = QDir(folder).entryList({"buildroot-*.txt"}, QDir::Files, QDir::Name);
    return parse(dir, conf, roots.isEmpty() ? QByteArray() : readFile(folder + '/' + roots.first()));
}

QList<Build> builds(const QString &out)
{
    QList<Build> list;

    if (out.isEmpty()) {
        return list;
    }
    /* the hidden ones are a build going on, or one being replaced */
    for (const QString &name : QDir(out).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const Build b = Build::read(out + '/' + name);
        if (b.isValid()) {
            list << b;
        }
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const Build &a, const Build &b) { return a.built > b.built; });
    return list;
}

QByteArray Record::serialize() const
{
    QByteArray out = "# What Vitrine installed of the host's packages (host/kde/install.sh)\n";
    QStringList names = digests.keys();
    QStringList roots = buildroot.keys();

    std::sort(names.begin(), names.end());
    std::sort(roots.begin(), roots.end());
    out += "evr=" + evr.toUtf8() + '\n';
    out += "series=" + series.toUtf8() + '\n';
    out += "tag=" + tag.toUtf8() + '\n';
    out += "when=" + when.toUTC().toString(Qt::ISODate).toUtf8() + '\n';
    for (const QString &name : std::as_const(names)) {
        out += "digest=" + name.toUtf8() + ' ' + digests.value(name).toUtf8() + '\n';
    }
    for (const QString &name : std::as_const(roots)) {
        out += "buildroot=" + name.toUtf8() + ' ' + buildroot.value(name).toUtf8() + '\n';
    }
    return out;
}

Record Record::parse(const QByteArray &conf)
{
    QHash<QString, QStringList> lists;
    const QHash<QString, QString> values = confValues(conf, &lists);
    Record r;

    r.evr = values.value("evr");
    r.series = values.value("series");
    r.tag = values.value("tag");
    r.when = QDateTime::fromString(values.value("when"), Qt::ISODate);
    for (const QString &line : lists.value("digest")) {
        const QStringList f = line.split(' ', Qt::SkipEmptyParts);
        if (f.size() == 2) {
            r.digests.insert(f[0], f[1]);
        }
    }
    for (const QString &line : lists.value("buildroot")) {
        const QStringList f = line.split(' ', Qt::SkipEmptyParts);
        if (f.size() == 2) {
            r.buildroot.insert(f[0], f[1]);
        }
    }
    return r;
}

Record Record::read(const QString &path)
{
    return parse(readFile(path));
}

bool Record::write(const QString &path) const
{
    QSaveFile file(path);

    QDir().mkpath(QFileInfo(path).absolutePath());
    return file.open(QIODevice::WriteOnly) && file.write(serialize()) >= 0 && file.commit();
}

QString Record::path(const QString &out)
{
    return out + "/installed.conf";
}

Record Record::of(const Build &build, const QDateTime &when, const QStringList &against)
{
    Record r;

    r.evr = build.evr;
    r.series = build.series;
    r.tag = build.tag;
    r.when = when;
    for (const Build::Rpm &rpm : build.rpms) {
        r.digests.insert(rpm.name, rpm.digest);
    }
    for (const QString &name : against) {
        if (build.buildroot.contains(name)) {
            r.buildroot.insert(name, versionOfEvr(build.buildroot.value(name)));
        }
    }
    return r;
}

QStringList digests(const Target &target, const QList<Build> &builds, const Record &record)
{
    QStringList list;

    if (const QString d = record.digests.value(target.package); !d.isEmpty()) {
        list << d;
    }
    for (const Build &b : builds) {
        if (const QString d = b.rpm(target.package).digest; !d.isEmpty() && !list.contains(d)) {
            list << d;
        }
    }
    return list;
}

/* Whose @target is: Vitrine's by its digest alone; someone else's when
   installed from a file or locked by them */
static Status::Owner ownerOf(const Target &target, const System &system, const QStringList &mine)
{
    const Package installed = system.package(target.package);

    if (!installed.digest.isEmpty() && mine.contains(installed.digest)) {
        return Status::Owner::Vitrine;
    }
    if (installed.isLocal()) {
        return Status::Owner::Foreign;
    }
    for (const Package &p : system.of(target.package)) {
        if (system.locks.contains(p.name)) {
            return Status::Owner::Foreign;
        }
    }
    return Status::Owner::Distribution;
}

QString versionOf(const Target &target, const System &system, const QStringList &mine)
{
    const Package installed = system.package(target.package);
    QString version = installed.version;

    if (ownerOf(target, system, mine) != Status::Owner::Distribution && !target.follows.isEmpty() &&
        system.package(target.follows).isValid()) {
        version = system.package(target.follows).version;
    }
    /* the newest source package is what a build takes */
    if (const QString newest = versionOfEvr(system.newest.value(target.package));
        !newest.isEmpty() && (version.isEmpty() || compareEvr(newest, version) > 0)) {
        version = newest;
    }
    return version;
}

Status status(const Target &target, const System &system, const Recipe &recipe,
              const QList<Build> &builds, const Record &record)
{
    const Package plasma = system.package("plasma-workspace");
    const Package reference = system.package(target.reference());
    Status s;

    s.target = target;
    s.installed = system.package(target.package);
    s.plasma = plasma.version;
    s.versionlock = system.versionlock;
    s.pkexec = system.pkexec;
    for (const Package &p : system.of(target.package)) {
        s.names << p.name;
    }
    std::sort(s.names.begin(), s.names.end());
    if (!system.isPlasmaHost() || system.fedora.isEmpty() || !s.installed.isValid()) {
        return s;
    }

    /* whose it is: Vitrine's by its digest alone */
    const QStringList mine = digests(target, builds, record);
    QHash<QString, QString> builtAgainst = record.buildroot;
    QString installedTag = record.tag.isEmpty() ? record.series : record.tag;
    if (record.digests.value(target.package) != s.installed.digest) {
        for (const Build &b : builds) {
            if (b.rpm(target.package).digest == s.installed.digest) {
                builtAgainst.clear();
                for (const QString &name : target.against) {
                    if (b.buildroot.contains(name)) {
                        builtAgainst.insert(name, versionOfEvr(b.buildroot.value(name)));
                    }
                }
                installedTag = b.tag;
                break;
            }
        }
    }
    s.locked = system.locks.contains(target.package);
    s.lockedAt = system.locks.value(target.package);
    s.owner = ownerOf(target, system, mine);
    s.nextLogin = system.session.isValid() && s.installed.installed.isValid() &&
                  s.installed.installed > system.session;
    s.version = versionOf(target, system, mine);
    if (const QString newest = versionOfEvr(system.newest.value(target.package));
        !newest.isEmpty() && s.version == newest &&
        newest != (s.owner == Status::Owner::Distribution || target.follows.isEmpty()
                       ? s.installed.version
                       : reference.version)) {
        s.basis = newest;
        s.basisInRepositories = true;
    } else if (!target.follows.isEmpty() && reference.isValid()) {
        s.basis = packageTitle(target.follows) + ' ' + reference.version;
    } else {
        s.basis = packageTitle(target.package) + ' ' + s.version;
    }
    s.series = recipe.series(target.package);
    s.tag = recipe.tag(target.package);

    /* where Plasma comes from, which gives the container its repositories:
       Fedora's have their names, and the installation's medium a hash of its own */
    if (!recipe.copr.isEmpty() && plasma.repo == recipe.copr) {
        s.plasmaFrom = "beta";
    } else if (plasma.repo == "updates-testing") {
        s.plasmaFrom = "testing";
    } else if (!plasma.repo.isEmpty() && !plasma.repo.startsWith("copr:") && !plasma.isLocal()) {
        s.plasmaFrom = "fedora";
    }
    if (!recipe.isValid()) {
        s.offer = Status::Offer::Unsupported;
        s.why = tr("Vitrine cannot read the research's recipe in host/kde.");
    } else if (recipe.fedora != system.fedora) {
        s.offer = Status::Offer::Unsupported;
        s.why = tr("The research's recipe builds %1 for Fedora %2, and this computer runs Fedora %3.")
                    .arg(target.title, recipe.fedora, system.fedora);
    } else if (s.plasmaFrom.isEmpty()) {
        s.offer = Status::Offer::Unsupported;
        s.why = tr("This computer's Plasma comes from %1: Vitrine builds the host's packages for "
                   "Plasma from Fedora's repositories or from the KDE SIG's beta COPR only.")
                    .arg(plasma.repo.isEmpty() ? tr("an unknown place") : plasma.repo);
    } else if (s.series.isEmpty()) {
        s.offer = Status::Offer::NoSet;
    } else {
        s.offer = Status::Offer::Offered;
    }

    /* the newest build for this version, on this Fedora, from the same repositories */
    for (const Build &b : builds) {
        if (release(b.version, target.parts) == release(s.version, target.parts) &&
            b.fedora == system.fedora && b.plasma == s.plasmaFrom) {
            if (!s.build.isValid()) {
                s.build = b;
            }
        } else if (!s.other.isValid()) {
            s.other = b;
        }
    }
    if (s.build.isValid()) {
        s.recipeChanged = s.build.key != recipe.key(target.package);
        /* its release, which the system must have first (KWin's Plasma) */
        if (!target.follows.isEmpty() && reference.isValid() &&
            release(s.build.version, target.parts) != release(reference.version, target.parts)) {
            s.differs << target.follows + ' ' + s.build.version + ' ' + reference.version;
        }
        for (const QString &name : target.against) {
            const QString built = versionOfEvr(s.build.buildroot.value(name));
            const QString now = system.package(name).version;
            if (!built.isEmpty() && !now.isEmpty() && built != now) {
                s.differs << name + ' ' + built + ' ' + now;
            }
        }
    }

    /* Vitrine's against what the system has now */
    if (s.owner == Status::Owner::Vitrine) {
        if (!target.follows.isEmpty() && reference.isValid() &&
            release(s.installed.version, target.parts) != release(reference.version, target.parts)) {
            s.outdated << target.follows + ' ' + s.installed.version + ' ' + reference.version;
        }
        for (const QString &name : target.against) {
            const QString built = builtAgainst.value(name);
            const QString now = system.package(name).version;
            if (!built.isEmpty() && !now.isEmpty() && built != now) {
                s.outdated << name + ' ' + built + ' ' + now;
            }
        }
        /* an update the lock keeps out */
        const QString newest = system.newest.value(target.package);
        if (!newest.isEmpty() && compareEvr(newest, untagged(s.installed.evr(), installedTag)) > 0) {
            s.heldBack = newest;
        }
    }
    return s;
}

/* "Qt 6.11.3" of "qt6-qtbase 6.11.2 6.11.3": what the system has now */
static QString nameNow(const QString &line)
{
    const QStringList f = line.split(' ');
    return packageTitle(f.value(0)) + ' ' + f.value(2);
}

QString Status::summary() const
{
    if (offer == Offer::Hidden) {
        return {};
    }
    /* Vitrine's own first: out of date, it matters whatever else holds */
    if (owner == Owner::Vitrine) {
        if (!outdated.isEmpty()) {
            return tr("Out of date: %1 is installed").arg(nameNow(outdated.first()));
        }
        if (!locked) {
            return tr("Installed, not version-locked");
        }
        if (build.isValid() && build.rpm(target.package).digest != installed.digest &&
            differs.isEmpty()) {
            return tr("Installed and locked; another build is ready");
        }
        if (!heldBack.isEmpty()) {
            return tr("Installed and locked; the update to %1 waits").arg(heldBack);
        }
        return nextLogin ? tr("Installed and locked, from the next login")
                         : tr("Installed and locked");
    }
    switch (offer) {
    case Offer::Hidden:
    case Offer::Offered:
        break;
    case Offer::Unsupported:
        return tr("Not available for this computer");
    case Offer::NoSet:
        return tr("No host set for %1 %2").arg(target.title, version);
    }
    switch (owner) {
    case Owner::Vitrine:
        break;
    case Owner::Foreign:
        return installed.isLocal() ? tr("Another %1 is installed: %2").arg(target.title, installed.evr())
                                   : tr("%1 is version-locked, not by Vitrine").arg(target.title);
    case Owner::Distribution:
        break;
    }
    if (build.isValid()) {
        if (differs.isEmpty()) {
            return tr("Built, not installed");
        }
        const QStringList f = differs.first().split(' ');
        return (f.value(0) == target.follows ? tr("Built for %1 %2 (this computer has %3)")
                                             : tr("Built against %1 %2 (this computer has %3)"))
            .arg(packageTitle(f.value(0)), f.value(1), f.value(2));
    }
    if (other.isValid()) {
        return (basisInRepositories ? tr("Built for %1 %2; the repositories have %3")
                                    : tr("Built for %1 %2; this computer has %3"))
            .arg(target.title, other.version, basis);
    }
    return tr("Not built");
}

bool Status::canBuild(QString *why) const
{
    QString reason;

    switch (offer) {
    case Offer::Hidden:
        reason = tr("Vitrine builds the host's %1 on Fedora with KDE Plasma only.").arg(target.title);
        break;
    case Offer::Unsupported:
        reason = this->why;
        break;
    case Offer::NoSet:
        reason = tr("The research has no host set for %1 %2.").arg(target.title, version);
        break;
    case Offer::Offered:
        break;
    }
    if (why) {
        *why = reason;
    }
    return reason.isEmpty();
}

bool Status::canInstall(QString *why) const
{
    QString reason;

    if (!canBuild(&reason)) {
        /* reason given */
    } else if (!build.isValid()) {
        reason = !other.isValid() ? tr("It is not built yet.")
                 : basisInRepositories
                     ? tr("The build there is for %1 %2, and the repositories have %3: build it again.")
                           .arg(target.title, other.version, basis)
                     : tr("The build there is for %1 %2, and this computer has %3: build it again.")
                           .arg(target.title, other.version, basis);
    } else if (!differs.isEmpty()) {
        const QStringList f = differs.first().split(' ');
        reason = (f.value(0) == target.follows
                      ? tr("It was built for %1 %2, and this computer has %3: bring the system up to "
                           "date (dnf upgrade), then build it again if they still differ.")
                      : tr("It was built against %1 %2, and this computer has %3: bring the system "
                           "up to date (dnf upgrade), then build it again if they still differ."))
                     .arg(packageTitle(f.value(0)), f.value(1), f.value(2));
    } else if (owner == Owner::Vitrine && build.rpm(target.package).digest == installed.digest &&
               locked) {
        reason = tr("This build is installed and locked.");
    } else if (!installed.isValid()) {
        reason = tr("%1 is not installed.").arg(target.title);
    } else if (!versionlock) {
        reason = tr("dnf has no versionlock command here, which would keep updates from replacing "
                    "it: Vitrine does not install it without one.");
    } else if (!pkexec) {
        reason = tr("pkexec (polkit) is missing: Vitrine installs it with the administrator's "
                    "password, through pkexec.");
    }
    if (why) {
        *why = reason;
    }
    return reason.isEmpty();
}

bool Status::canRevert(QString *why) const
{
    QString reason;

    switch (owner) {
    case Owner::Vitrine:
        break;
    case Owner::Foreign:
        reason = tr("The %1 installed is not one Vitrine installed: Vitrine leaves it alone.")
                     .arg(target.title);
        break;
    case Owner::Distribution:
        reason = tr("The distribution's %1 is installed.").arg(target.title);
        break;
    }
    if (reason.isEmpty() && !pkexec) {
        reason = tr("pkexec (polkit) is missing: Vitrine reverts it with the administrator's "
                    "password, through pkexec.");
    }
    if (why) {
        *why = reason;
    }
    return reason.isEmpty();
}

static QString executable(const char *name, const char *fallback)
{
    const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
    return found.isEmpty() ? QString::fromLatin1(fallback) : found;
}

QStringList installCommand(const QString &sourceDir, const Build &build)
{
    QStringList command = {executable("pkexec", "/usr/bin/pkexec"), executable("bash", "/bin/bash"),
                           sourceDir + "/install.sh", "install"};

    for (const Build::Rpm &rpm : build.rpms) {
        const QString file = build.dir + '/' + rpm.file;
        command << file + '=' + fileHash(file);
    }
    return command;
}

QStringList revertCommand(const QString &sourceDir, const QHash<QString, QStringList> &digests)
{
    QStringList command = {executable("pkexec", "/usr/bin/pkexec"), executable("bash", "/bin/bash"),
                           sourceDir + "/install.sh", "revert"};
    QStringList packages = digests.keys();

    std::sort(packages.begin(), packages.end());
    for (const QString &package : std::as_const(packages)) {
        for (const QString &digest : digests.value(package)) {
            command << package + '=' + digest;
        }
    }
    return command;
}

}

using namespace HostKde;

HostKdeMonitor *HostKdeMonitor::instance()
{
    static QPointer<HostKdeMonitor> monitor;

    if (!monitor) {
        monitor = new HostKdeMonitor(QCoreApplication::instance());
    }
    return monitor;
}

HostKdeMonitor::HostKdeMonitor(QObject *parent) : QObject(parent) {}

HostKdeMonitor::~HostKdeMonitor()
{
    if (m_process) {
        /* pkexec's own: it goes with its dialog; the install, once started, finishes */
        m_process->disconnect(this);
        m_process->terminate();
        m_process->waitForFinished(2000);
    }
}

QString HostKdeMonitor::sourceDir() const
{
    return m_sourceDir.isEmpty() ? HostKde::sourceDir() : m_sourceDir;
}

QString HostKdeMonitor::dataDir() const
{
    return m_dataDir.isEmpty() ? HostKde::dataDir() : m_dataDir;
}

QList<Target> HostKdeMonitor::targets() const
{
    /* before the first reading: what the folder says */
    return m_read ? m_targets : Target::read(sourceDir());
}

Target HostKdeMonitor::target(const QString &id) const
{
    return HostKde::target(targets(), id);
}

QString HostKdeMonitor::outDir(const QString &id) const
{
    return dataDir() + '/' + id;
}

Record HostKdeMonitor::record(const QString &id) const
{
    return Record::read(Record::path(outDir(id)));
}

QList<Build> HostKdeMonitor::builds(const QString &id) const
{
    return HostKde::builds(outDir(id));
}

Status HostKdeMonitor::status(const QString &id) const
{
    const Target t = target(id);
    Status s;

    if (!t.isValid()) {
        return s;
    }
    return HostKde::status(t, m_system, m_recipe, builds(id), record(id));
}

QList<Status> HostKdeMonitor::statuses() const
{
    QList<Status> list;

    for (const Target &t : targets()) {
        list << HostKde::status(t, m_system, m_recipe, builds(t.id), record(t.id));
    }
    return list;
}

void HostKdeMonitor::refresh()
{
    if (m_reading) {
        m_again = true;
        return;
    }
    m_reading = true;
    auto *watcher = new QFutureWatcher<Reading>(this);
    connect(watcher, &QFutureWatcher<Reading>::finished, this, [this, watcher]() {
        const Reading reading = watcher->result();
        watcher->deleteLater();
        m_reading = false;
        apply(reading);
        if (m_again) {
            m_again = false;
            refresh();
        }
    });
    watcher->setFuture(QtConcurrent::run(&HostKdeMonitor::read, sourceDir(), dataDir(), m_osRelease,
                                         m_proc));
}

void HostKdeMonitor::lookAgain()
{
    if (!m_reading && !m_process) {
        refresh();
    }
}

void HostKdeMonitor::refreshNow()
{
    apply(read(sourceDir(), dataDir(), m_osRelease, m_proc));
}

HostKdeMonitor::Reading HostKdeMonitor::read(const QString &source, const QString &dataDir,
                                             const QString &osRelease, const QString &proc)
{
    Reading r;
    QHash<QString, QString> versions;

    r.targets = Target::read(source);
    r.system = System::read(r.targets, osRelease, proc);
    if (!r.system.isPlasmaHost() || r.system.fedora.isEmpty()) {
        return r;
    }
    /* the set of each package for the version a build of it takes */
    for (const Target &t : std::as_const(r.targets)) {
        const QString out = t.outDir(dataDir);
        const QStringList mine = digests(t, HostKde::builds(out), Record::read(Record::path(out)));
        if (r.system.package(t.package).isValid()) {
            versions.insert(t.package, versionOf(t, r.system, mine));
        }
    }
    r.recipe = Recipe::read(source, r.system.fedora, versions);
    return r;
}

void HostKdeMonitor::apply(const Reading &reading)
{
    m_targets = reading.targets;
    m_system = reading.system;
    m_recipe = reading.recipe;
    m_read = true;
    emit changed();
}

void HostKdeMonitor::install(const QString &id, const Build &build)
{
    if (!m_process && target(id).isValid()) {
        run(installCommand(sourceDir(), build), {id}, true, build);
    }
}

void HostKdeMonitor::revert(const QStringList &ids)
{
    QHash<QString, QStringList> mine;

    if (m_process) {
        return;
    }
    for (const QString &id : ids) {
        const Target t = target(id);
        if (t.isValid()) {
            mine.insert(t.package, digests(t, builds(id), record(id)));
        }
    }
    if (!mine.isEmpty()) {
        run(revertCommand(sourceDir(), mine), ids, false, {});
    }
}

void HostKdeMonitor::run(const QStringList &command, const QStringList &ids, bool install,
                         const Build &build)
{
    m_log.clear();
    m_partial.clear();
    m_error.clear();
    m_busy = ids;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, [this]() {
        const QString text = QString::fromLocal8Bit(m_process->readAll());
        qsizetype end;
        m_log += text;
        m_partial += text;
        while ((end = m_partial.indexOf('\n')) >= 0) {
            const QString line = m_partial.left(end);
            m_partial.remove(0, end + 1);
            if (line.startsWith("vitrine-kde: error: ")) {
                m_error = line.mid(20);
            }
        }
        emit output(text);
    });
    const auto done = [this, ids, install, build](int code, bool crashed) {
        QString error;
        /* pkexec: 126 when its dialog was dismissed, 127 when not authorized */
        const bool cancelled = !crashed && (code == 126 || code == 127);
        if (!m_partial.isEmpty() && m_partial.startsWith("vitrine-kde: error: ")) {
            m_error = m_partial.mid(20).trimmed();
        }
        if (crashed || code != 0) {
            error = cancelled         ? (code == 126 ? tr("The password was not given")
                                                     : tr("Not authorized"))
                    : !m_error.isEmpty() ? m_error
                                         : tr("install.sh failed (status %1)").arg(code);
        } else if (install) {
            const QString out = outDir(ids.value(0));
            if (!Record::of(build, QDateTime::currentDateTimeUtc(), target(ids.value(0)).against)
                     .write(Record::path(out))) {
                error = tr("Installed, but Vitrine could not write its record in %1").arg(out);
            }
        } else {
            for (const QString &id : ids) {
                QFile::remove(Record::path(outDir(id)));
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
        m_busy.clear();
        emit finished(error, cancelled);
        refresh();
    };
    connect(m_process, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) {
        done(code, status == QProcess::CrashExit);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_error = tr("pkexec could not run: %1").arg(m_process->errorString());
            done(-1, false);
        }
    });
    emit started(install);
    m_log = "$ " + command.join(' ') + '\n';
    emit output(m_log);
    m_process->start(command.first(), command.mid(1));
}
