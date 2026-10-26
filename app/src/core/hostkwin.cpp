// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkwin.h"

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
    return QCoreApplication::translate("HostKwin", text);
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
static QString versionOf(const QString &evr)
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

namespace HostKwin {

static bool isKwinDir(const QString &dir)
{
    return !dir.isEmpty() && QFileInfo(dir + "/build.sh").isFile() &&
           QFileInfo(dir + "/install.sh").isFile() && QFileInfo(dir + "/components.conf").isFile();
}

QString sourceDir()
{
    const QString chosen = qEnvironmentVariable("VITRINE_HOST_KWIN_DIR");

    if (!chosen.isEmpty()) {
        return isKwinDir(chosen) ? QFileInfo(chosen).canonicalFilePath() : QString();
    }
    const QString host = StackBuilder::hostDir();
    return isKwinDir(host + "/kwin") ? QFileInfo(host + "/kwin").canonicalFilePath() : QString();
}

QString outDir()
{
    return Paths::dataDir() + "/host-kwin";
}

QString workDir()
{
    return Paths::cacheDir() + "/host-kwin-build";
}

QStringList kwinPackages()
{
    return {"kwin", "kwin-common", "kwin-libs", "kwin-devel", "kwin-doc"};
}

QStringList abiPackages()
{
    return {"qt6-qtbase", "qt6-qtdeclarative", "kdecoration", "libplasma"};
}

QString plasmaRelease(const QString &version)
{
    return version.section('.', 0, 2);
}

QString packageTitle(const QString &name)
{
    return name == "plasma-workspace"    ? QStringLiteral("Plasma")
           : name == "qt6-qtbase"        ? QStringLiteral("Qt")
           : name == "qt6-qtdeclarative" ? QStringLiteral("Qt Declarative")
           : name == "kdecoration"       ? QStringLiteral("KDecoration")
                                         : name;
}

QString versionText(const QString &pattern)
{
    return QString(pattern).replace('*', 'x');
}

QList<Set> sets(const QString &sourceDir)
{
    /* host/kwin/6.7.91) echo "hl39 hl39" ;;   # a comment */
    static const QRegularExpression line(
        "^\\s*(host/kwin/[^)]*)\\)\\s*echo\\s+\"([^\"]*)\"");
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
            versions << pattern.trimmed().mid(QStringLiteral("host/kwin/").size());
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

Package System::package(const QString &name) const
{
    for (const QList<Package> *list : {&kwin, &others}) {
        for (const Package &p : *list) {
            if (p.name == name) {
                return p;
            }
        }
    }
    return {};
}

QString System::plasma() const
{
    return package("plasma-workspace").version;
}

bool System::isPlasmaHost() const
{
    return rpm && package("kwin").isValid() && package("plasma-workspace").isValid();
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
    const QStringList kwin = kwinPackages();
    QHash<QString, QString> locks;
    QString current;

    for (const QByteArray &raw : list.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (const QRegularExpressionMatch m = name.match(line); m.hasMatch()) {
            current = kwin.contains(m.captured(1)) ? m.captured(1) : QString();
            if (!current.isEmpty() && !locks.contains(current)) {
                /* an entry of another comparator than = locks it too */
                locks.insert(current, QString());
            }
        } else if (const QRegularExpressionMatch m = evr.match(line);
                   m.hasMatch() && !current.isEmpty()) {
            locks.insert(current, m.captured(1));
        }
    }
    return locks;
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

System System::read(const QString &osRelease, const QString &proc)
{
    const QString rpm = QStandardPaths::findExecutable("rpm");
    const QString dnf = QStandardPaths::findExecutable("dnf");
    QStringList names = kwinPackages();
    System s;
    int status = -1;

    s.pkexec = !QStandardPaths::findExecutable("pkexec").isEmpty();
    s.fedora = fedoraRelease(osRelease);
    if (rpm.isEmpty() || dnf.isEmpty() || !run(dnf, {"--version"}).startsWith("dnf5")) {
        return s;
    }
    s.rpm = true;
    names << "plasma-workspace" << abiPackages();
    const QByteArray packages = run(rpm, QStringList{"-q", "--qf", rpmFormat()} + names);
    /* the local cache only: nothing downloaded, nothing written */
    const QByteArray repos = run(dnf, {"-q", "-C", "repoquery", "--installed", "--qf",
                                       "%{name} %{from_repo}\\n", "kwin", "plasma-workspace"});
    const QByteArray locks = run(dnf, {"versionlock", "list"}, &status);
    s.versionlock = status == 0;
    for (const Package &p : parsePackages(packages, repos)) {
        static const QRegularExpression kwinSource("^kwin-\\d");
        if (kwinPackages().contains(p.name) && kwinSource.match(p.sourceRpm).hasMatch()) {
            s.kwin << p;
        } else if (!kwinPackages().contains(p.name)) {
            s.others << p;
        }
    }
    s.locks = parseLocks(locks);
    s.session = sessionStart(proc);
    return s;
}

Recipe Recipe::parse(const QByteArray &output)
{
    const QHash<QString, QString> values = confValues(output);
    const QStringList set = values.value("set").split(' ', Qt::SkipEmptyParts);
    Recipe r;

    r.fedora = values.value("fedora");
    r.copr = values.value("copr");
    r.series = set.value(0);
    r.tag = set.value(1);
    r.key = values.value("key");
    return r;
}

Recipe Recipe::read(const QString &sourceDir, const QString &plasma, const QString &fedora)
{
    const QString bash = QStandardPaths::findExecutable("bash");
    int status = -1;

    if (sourceDir.isEmpty() || plasma.isEmpty() || fedora.isEmpty()) {
        return {};
    }
    const QByteArray out = run(bash.isEmpty() ? QStringLiteral("/bin/bash") : bash,
                               {sourceDir + "/build.sh", "--print-recipe", plasma, fedora}, &status);
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

Build Build::parse(const QString &dir, const QByteArray &conf, const QByteArray &buildroot)
{
    QHash<QString, QStringList> lists;
    const QHash<QString, QString> values = confValues(conf, &lists);
    Build b;

    b.dir = dir;
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
    const QStringList roots = QDir(dir + "/kwin").entryList({"buildroot-*.txt"}, QDir::Files, QDir::Name);
    return parse(dir, readFile(dir + "/build.conf"),
                 roots.isEmpty() ? QByteArray() : readFile(dir + "/kwin/" + roots.first()));
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
    QByteArray out = "# The host's KWin Vitrine installed (host/kwin/install.sh)\n";
    QStringList names = digests.keys();
    QStringList roots = buildroot.keys();

    std::sort(names.begin(), names.end());
    std::sort(roots.begin(), roots.end());
    out += "evr=" + evr.toUtf8() + '\n';
    out += "series=" + series.toUtf8() + '\n';
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

Record Record::of(const Build &build, const QDateTime &when)
{
    Record r;

    r.evr = build.evr;
    r.series = build.series;
    r.when = when;
    for (const Build::Rpm &rpm : build.rpms) {
        r.digests.insert(rpm.name, rpm.digest);
    }
    for (const QString &name : abiPackages()) {
        if (build.buildroot.contains(name)) {
            r.buildroot.insert(name, versionOf(build.buildroot.value(name)));
        }
    }
    return r;
}

Status status(const System &system, const Recipe &recipe, const QList<Build> &builds,
              const Record &record)
{
    const Package plasma = system.package("plasma-workspace");
    Status s;

    s.kwin = system.package("kwin");
    s.plasma = plasma.version;
    s.series = recipe.series;
    s.tag = recipe.tag;
    s.versionlock = system.versionlock;
    s.pkexec = system.pkexec;
    if (!system.isPlasmaHost() || system.fedora.isEmpty()) {
        return s;
    }

    /* whose KWin runs: Vitrine's by its digest alone */
    QStringList mine = record.digests.values();
    QHash<QString, QString> builtAgainst = record.buildroot;
    for (const Build &b : builds) {
        if (const Build::Rpm r = b.rpm("kwin"); !r.digest.isEmpty()) {
            mine << r.digest;
            if (r.digest == s.kwin.digest && !record.digests.values().contains(r.digest)) {
                builtAgainst.clear();
                for (const QString &name : abiPackages()) {
                    if (b.buildroot.contains(name)) {
                        builtAgainst.insert(name, versionOf(b.buildroot.value(name)));
                    }
                }
            }
        }
    }
    s.locked = system.locks.contains("kwin");
    s.lockedAt = system.locks.value("kwin");
    if (!s.kwin.digest.isEmpty() && mine.contains(s.kwin.digest)) {
        s.owner = Status::Owner::Vitrine;
    } else if (s.kwin.isLocal() || !system.locks.isEmpty()) {
        s.owner = Status::Owner::Foreign;
    }
    s.nextLogin = system.session.isValid() && s.kwin.installed.isValid() &&
                  s.kwin.installed > system.session;

    /* where Plasma comes from: Fedora's repositories have their names, and the
       installation's medium a hash of its own */
    if (!recipe.copr.isEmpty() && plasma.repo == recipe.copr) {
        s.plasmaFrom = "beta";
    } else if (plasma.repo == "updates-testing") {
        s.plasmaFrom = "testing";
    } else if (!plasma.repo.isEmpty() && !plasma.repo.startsWith("copr:") && !plasma.isLocal()) {
        s.plasmaFrom = "fedora";
    }
    if (!recipe.isValid()) {
        s.offer = Status::Offer::Unsupported;
        s.why = tr("Vitrine cannot read the research's recipe in host/kwin.");
    } else if (recipe.fedora != system.fedora) {
        s.offer = Status::Offer::Unsupported;
        s.why = tr("The research's recipe builds KWin for Fedora %1, and this computer runs "
                   "Fedora %2.")
                    .arg(recipe.fedora, system.fedora);
    } else if (s.plasmaFrom.isEmpty()) {
        s.offer = Status::Offer::Unsupported;
        s.why = tr("This computer's Plasma comes from %1: Vitrine builds KWin for Plasma from "
                   "Fedora's repositories or from the KDE SIG's beta COPR only.")
                    .arg(plasma.repo.isEmpty() ? tr("an unknown place") : plasma.repo);
    } else if (recipe.series.isEmpty()) {
        s.offer = Status::Offer::NoSet;
    } else {
        s.offer = Status::Offer::Offered;
    }

    /* the newest build for this Plasma, on this Fedora, from the same repositories */
    for (const Build &b : builds) {
        if (plasmaRelease(b.version) == plasmaRelease(s.plasma) && b.fedora == system.fedora &&
            b.plasma == s.plasmaFrom) {
            if (!s.build.isValid()) {
                s.build = b;
            }
        } else if (!s.other.isValid()) {
            s.other = b;
        }
    }
    if (s.build.isValid()) {
        s.recipeChanged = s.build.key != recipe.key;
        for (const QString &name : abiPackages()) {
            const QString built = versionOf(s.build.buildroot.value(name));
            const QString now = system.package(name).version;
            if (!built.isEmpty() && !now.isEmpty() && built != now) {
                s.differs << name + ' ' + built + ' ' + now;
            }
        }
    }

    /* Vitrine's KWin against what the system has now */
    if (s.owner == Status::Owner::Vitrine) {
        if (plasmaRelease(s.kwin.version) != plasmaRelease(s.plasma)) {
            s.outdated << "plasma-workspace " + s.kwin.version + ' ' + s.plasma;
        }
        for (const QString &name : abiPackages()) {
            const QString built = builtAgainst.value(name);
            const QString now = system.package(name).version;
            if (!built.isEmpty() && !now.isEmpty() && built != now) {
                s.outdated << name + ' ' + built + ' ' + now;
            }
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
    /* Vitrine's own KWin first: out of date, it matters whatever else holds */
    if (owner == Owner::Vitrine) {
        if (!outdated.isEmpty()) {
            return tr("Out of date: %1 is installed").arg(nameNow(outdated.first()));
        }
        if (!locked) {
            return tr("Installed, not version-locked");
        }
        if (build.isValid() && build.rpm("kwin").digest != kwin.digest && differs.isEmpty()) {
            return tr("Installed and locked; another build is ready");
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
        return tr("No host set for KWin %1").arg(plasma);
    }
    switch (owner) {
    case Owner::Vitrine:
        break;
    case Owner::Foreign:
        return kwin.isLocal() ? tr("Another KWin is installed: %1").arg(kwin.evr())
                              : tr("KWin is version-locked, not by Vitrine");
    case Owner::Distribution:
        break;
    }
    if (build.isValid()) {
        return differs.isEmpty() ? tr("Built, not installed")
                                 : tr("Built against %1 %2 (this computer has %3)")
                                       .arg(packageTitle(differs.first().split(' ').value(0)),
                                            differs.first().split(' ').value(1),
                                            differs.first().split(' ').value(2));
    }
    if (other.isValid()) {
        return tr("Built for KWin %1; this computer has Plasma %2").arg(other.version, plasma);
    }
    return tr("Not built");
}

bool Status::canBuild(QString *why) const
{
    QString reason;

    switch (offer) {
    case Offer::Hidden:
        reason = tr("Vitrine builds the host's KWin on Fedora with KDE Plasma only.");
        break;
    case Offer::Unsupported:
        reason = this->why;
        break;
    case Offer::NoSet:
        reason = tr("The research has no host set for KWin %1.").arg(plasma);
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
        reason = other.isValid()
                     ? tr("The build there is for KWin %1, and this computer has Plasma %2: "
                          "build it again.")
                           .arg(other.version, plasma)
                     : tr("It is not built yet.");
    } else if (!differs.isEmpty()) {
        const QStringList f = differs.first().split(' ');
        reason = tr("It was built against %1 %2, and this computer has %3: bring the system up "
                    "to date (dnf upgrade), then build it again if they still differ.")
                     .arg(packageTitle(f.value(0)), f.value(1), f.value(2));
    } else if (owner == Owner::Vitrine && build.rpm("kwin").digest == kwin.digest && locked) {
        reason = tr("This build is installed and locked.");
    } else if (!kwin.isValid()) {
        reason = tr("KWin is not installed.");
    } else if (!versionlock) {
        reason = tr("dnf has no versionlock command here, which would keep updates from replacing "
                    "this KWin: Vitrine does not install it without one.");
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
        reason = tr("The KWin installed is not one Vitrine installed: Vitrine leaves it alone.");
        break;
    case Owner::Distribution:
        reason = tr("The distribution's KWin is installed.");
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

QStringList revertCommand(const QString &sourceDir, const QList<Build> &builds,
                          const Record &record)
{
    QStringList command = {executable("pkexec", "/usr/bin/pkexec"), executable("bash", "/bin/bash"),
                           sourceDir + "/install.sh", "revert"};
    QStringList digests;

    if (record.digests.contains("kwin")) {
        digests << record.digests.value("kwin");
    }
    for (const Build &b : builds) {
        if (const QString d = b.rpm("kwin").digest; !d.isEmpty() && !digests.contains(d)) {
            digests << d;
        }
    }
    return command + digests;
}

}

using namespace HostKwin;

HostKwinMonitor *HostKwinMonitor::instance()
{
    static QPointer<HostKwinMonitor> monitor;

    if (!monitor) {
        monitor = new HostKwinMonitor(QCoreApplication::instance());
    }
    return monitor;
}

HostKwinMonitor::HostKwinMonitor(QObject *parent) : QObject(parent) {}

HostKwinMonitor::~HostKwinMonitor()
{
    if (m_process) {
        /* pkexec's own: it goes with its dialog; the install, once started, finishes */
        m_process->disconnect(this);
        m_process->terminate();
        m_process->waitForFinished(2000);
    }
}

QString HostKwinMonitor::sourceDir() const
{
    return m_sourceDir.isEmpty() ? HostKwin::sourceDir() : m_sourceDir;
}

QString HostKwinMonitor::outDir() const
{
    return m_outDir.isEmpty() ? HostKwin::outDir() : m_outDir;
}

Record HostKwinMonitor::record() const
{
    return Record::read(Record::path(outDir()));
}

QList<Build> HostKwinMonitor::builds() const
{
    return HostKwin::builds(outDir());
}

Status HostKwinMonitor::status() const
{
    return HostKwin::status(m_system, m_recipe, builds(), record());
}

void HostKwinMonitor::refresh()
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
    watcher->setFuture(QtConcurrent::run(&HostKwinMonitor::read, sourceDir(), outDir(), m_osRelease,
                                         m_proc));
}

void HostKwinMonitor::lookAgain()
{
    if (!m_reading && !m_process) {
        refresh();
    }
}

void HostKwinMonitor::refreshNow()
{
    apply(read(sourceDir(), outDir(), m_osRelease, m_proc));
}

HostKwinMonitor::Reading HostKwinMonitor::read(const QString &source, const QString &out,
                                               const QString &osRelease, const QString &proc)
{
    Reading r;

    r.system = System::read(osRelease, proc);
    if (r.system.isPlasmaHost() && !r.system.fedora.isEmpty()) {
        r.recipe = Recipe::read(source, r.system.plasma(), r.system.fedora);
    }
    r.builds = HostKwin::builds(out);
    r.record = Record::read(Record::path(out));
    return r;
}

void HostKwinMonitor::apply(const Reading &reading)
{
    m_system = reading.system;
    m_recipe = reading.recipe;
    m_read = true;
    emit changed();
}

void HostKwinMonitor::install(const Build &build)
{
    if (!m_process) {
        run(installCommand(sourceDir(), build), true, build);
    }
}

void HostKwinMonitor::revert()
{
    if (!m_process) {
        run(revertCommand(sourceDir(), builds(), record()), false, {});
    }
}

void HostKwinMonitor::run(const QStringList &command, bool install, const Build &build)
{
    m_log.clear();
    m_partial.clear();
    m_error.clear();
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
            if (line.startsWith("vitrine-kwin: error: ")) {
                m_error = line.mid(21);
            }
        }
        emit output(text);
    });
    const auto done = [this, install, build](int code, bool crashed) {
        QString error;
        /* pkexec: 126 when its dialog was dismissed, 127 when not authorized */
        const bool cancelled = !crashed && (code == 126 || code == 127);
        if (!m_partial.isEmpty() && m_partial.startsWith("vitrine-kwin: error: ")) {
            m_error = m_partial.mid(21).trimmed();
        }
        if (crashed || code != 0) {
            error = cancelled         ? (code == 126 ? tr("The password was not given")
                                                     : tr("Not authorized"))
                    : !m_error.isEmpty() ? m_error
                                         : tr("install.sh failed (status %1)").arg(code);
        } else if (install) {
            if (!Record::of(build, QDateTime::currentDateTimeUtc()).write(Record::path(outDir()))) {
                error = tr("Installed, but Vitrine could not write its record in %1")
                            .arg(outDir());
            }
        } else {
            QFile::remove(Record::path(outDir()));
        }
        m_process->deleteLater();
        m_process = nullptr;
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
