// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <unistd.h>

#include "core/buildcomponents.h"
#include "core/buildmanager.h"
#include "core/guesttoolsbuilder.h"
#include "core/hostkde.h"
#include "core/hostkdebuilder.h"
#include "core/stackbuilder.h"
#include "hostkdestandins.h"

using namespace HostKde;
using Offer = Status::Offer;
using Owner = Status::Owner;

static const char kCopr[] = "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta";

class TestHostKde : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_bin;          // the stand-ins, first in PATH
    QString m_state;
    QByteArray m_path;
    QList<Target> m_targets;

    static bool write(const QString &path, const QByteArray &text)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(text) == text.size();
    }

    static QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    /* The source package of a package of these tests: kwin's, Qt base's... */
    static QString sourceOf(const QString &name)
    {
        /* KWin's X11 session has its own */
        if (name.startsWith("kwin-x11")) {
            return QStringLiteral("kwin-x11");
        }
        for (const char *source : {"kwin", "qt6-qtbase", "kf6-ksvg"}) {
            if (name == QLatin1String(source) || name.startsWith(QLatin1String(source) + '-')) {
                return QLatin1String(source);
            }
        }
        if (name == "kf6-kirigami" || (name.startsWith("kf6-kirigami-") && name != "kf6-kirigami-addons")) {
            return QStringLiteral("kf6-kirigami");
        }
        return name;
    }

    /* A package's lines, as installed/NAME or an "RPM file" holds them */
    static QByteArray pkg(const QString &name, const QString &evr, const QString &digest,
                          const QString &repo = "fedora", const QString &arch = "x86_64")
    {
        const QString version = evr.section('-', 0, 0);
        const QString release = evr.section('-', 1);
        return QString("NAME=%1\nVERSION=%2\nRELEASE=%3\nARCH=%4\nDIGEST=%5\nSOURCERPM=%6-%2-%3.src.rpm\n"
                       "EPOCHNUM=0\nREPO=%7\nINSTALLTIME=1760000000\nVENDOR=(none)\n")
            .arg(name, version, release, arch, digest, sourceOf(name), repo)
            .toUtf8();
    }

    static Package package(const QString &name, const QString &evr, const QString &digest,
                           const QString &repo = "fedora")
    {
        Package p;
        p.name = name;
        p.version = evr.section('-', 0, 0);
        p.release = evr.section('-', 1);
        p.arch = "x86_64";
        p.digest = digest;
        p.sourceRpm = sourceOf(name) + '-' + evr + ".src.rpm";
        p.repo = repo;
        p.installed = QDateTime::fromSecsSinceEpoch(1760000000, QTimeZone::UTC);
        return p;
    }

    /* A Fedora 44 host with Plasma @plasma from @from and the distribution's
       KWin @kwin, KF 6.31.0 and Qt 6.11.2-3 */
    static System host(const QString &plasma = "6.7.91", const QString &kwin = "6.7.91-1.fc44",
                       const QString &from = kCopr)
    {
        System s;
        s.rpm = true;
        s.versionlock = true;
        s.pkexec = true;
        s.fedora = "44";
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            s.packages << package(name, kwin, QString("distro-%1").arg(name), from);
        }
        s.packages << package("plasma-workspace", plasma + "-1.fc44", "pw", from);
        for (const char *name : {"qt6-qtbase", "qt6-qtbase-common", "qt6-qtbase-gui"}) {
            s.packages << package(name, "6.11.2-3.fc44", QString("distro-%1").arg(name), "updates");
        }
        s.packages << package("qt6-qtdeclarative", "6.11.2-1.fc44", "qtd", "updates");
        for (const char *name : {"kf6-ksvg", "kf6-kirigami", "kf6-kcoreaddons"}) {
            s.packages << package(name, "6.31.0-1.fc44", QString("distro-%1").arg(name), "updates");
        }
        return s;
    }

    /* @system with @name at @version: a release of it, or of the frameworks... */
    static System moved(System system, const QString &name, const QString &evr)
    {
        for (Package &p : system.packages) {
            if (p.name == name) {
                p.version = evr.section('-', 0, 0);
                p.release = evr.section('-', 1);
                p.sourceRpm = sourceOf(name) + '-' + evr + ".src.rpm";
            }
        }
        return system;
    }

    static Recipe recipe(const QString &series = "hl39", const QString &tag = "hl39")
    {
        Recipe r;
        r.fedora = "44";
        r.copr = kCopr;
        if (!series.isEmpty()) {
            r.sets.insert("kwin", {series, tag});
        }
        r.sets.insert("kf6-ksvg", {"k2", "k2"});
        r.sets.insert("kf6-kirigami", {"f1", "f1"});
        r.sets.insert("qt6-qtbase", {"q4", "q4"});
        for (const char *p : {"kwin", "kf6-ksvg", "kf6-kirigami", "qt6-qtbase"}) {
            r.keys.insert(p, "key1");
        }
        return r;
    }

    /* A build of KWin @evr from the beta COPR against Qt 6.11.2, its digests "<evr>-<name>" */
    static Build build(const QString &evr = "6.7.91-1.hl39.fc44", const QString &qt = "6.11.2-3.fc44",
                       const QString &key = "key1", const QString &plasma = "beta")
    {
        Build b;
        b.dir = "/out/" + evr;
        b.package = "kwin";
        b.evr = evr;
        b.version = evr.section('-', 0, 0);
        b.series = "hl39";
        b.tag = "hl39";
        b.plasma = plasma;
        b.fedora = "44";
        b.key = key;
        b.built = QDateTime::fromSecsSinceEpoch(1760100000, QTimeZone::UTC);
        for (const char *name : {"kwin", "kwin-common", "kwin-libs", "kwin-devel"}) {
            b.rpms << Build::Rpm{name, "x86_64", evr + '-' + name,
                                 QString("kwin/%1-%2.x86_64.rpm").arg(name, evr)};
        }
        b.buildroot.insert("qt6-qtbase", qt);
        b.buildroot.insert("libplasma", "6.7.91-1.fc44");
        return b;
    }

    /* A build of @package (one of the three libraries) at @evr, set @tag, its digests "<evr>-<name>" */
    static Build libraryBuild(const QString &package, const QString &evr, const QString &tag,
                              const QStringList &names, const QString &qt = "6.11.2-3.fc44",
                              const QString &plasma = "beta")
    {
        Build b;
        b.dir = "/out/" + package + '/' + evr;
        b.package = package;
        b.evr = evr;
        b.version = evr.section('-', 0, 0);
        b.series = tag;
        b.tag = tag;
        b.plasma = plasma;
        b.fedora = "44";
        b.key = "key1";
        b.built = QDateTime::fromSecsSinceEpoch(1760100000, QTimeZone::UTC);
        for (const QString &name : names) {
            b.rpms << Build::Rpm{name, "x86_64", evr + '-' + name,
                                 QString("%1/%2-%3.x86_64.rpm").arg(package, name, evr)};
        }
        b.buildroot.insert("qt6-qtbase", qt);
        b.buildroot.insert("qt6-qtdeclarative", qt.section('-', 0, 0) + "-1.fc44");
        return b;
    }

    /* @system with Vitrine's @b installed (locked if @lock) */
    static System withVitrines(System system, const Build &b, bool lock = true)
    {
        for (Package &p : system.packages) {
            if (!b.rpm(p.name).name.isEmpty()) {
                p.version = b.version;
                p.release = b.evr.section('-', 1);
                p.digest = b.rpm(p.name).digest;
                p.repo = "@commandline";
                if (lock) {
                    system.locks.insert(p.name, b.evr);
                }
            }
        }
        return system;
    }

    Target kwin() const { return HostKde::target(m_targets, "host-kwin"); }
    Target ksvg() const { return HostKde::target(m_targets, "host-ksvg"); }
    Target kirigami() const { return HostKde::target(m_targets, "host-kirigami"); }
    Target qtbase() const { return HostKde::target(m_targets, "host-qtbase"); }

    QString hostKdeDir() const { return QStringLiteral(HOST_KDE_SOURCE_DIR); }
    /* $XDG_RUNTIME_DIR of the user installing, for install.sh */
    QString runDir() const { return m_state + "/run/" + QString::number(getuid()); }

    /* install.sh with the stand-ins, as "root"; its output */
    QString installScript(const QStringList &args, int *status, bool root = true)
    {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PATH", m_bin + ":/usr/bin:/bin");
        env.insert("STATE", m_state);
        env.insert("TMPDIR", m_tmp.filePath("tmp"));
        env.insert("FAKE_UID", root ? "0" : "1000");
        /* the switches: entries and files of the user installing, here */
        env.insert("VITRINE_TMPFILES_DIR", m_state + "/tmpfiles");
        env.insert("VITRINE_RUN_USER", m_state + "/run");
        env.insert("PKEXEC_UID", QString::number(getuid()));
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        QDir().mkpath(m_tmp.filePath("tmp"));
        p.start("/bin/bash", QStringList{hostKdeDir() + "/install.sh"} + args);
        p.waitForFinished(30000);
        *status = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        return QString::fromUtf8(p.readAll());
    }

    void resetState()
    {
        QDir(m_state).removeRecursively();
        for (const char *dir : {"installed", "repo", "locks", "files", "available"}) {
            QDir().mkpath(m_state + '/' + dir);
        }
        QDir().mkpath(runDir());
    }

    /* An "RPM file" of a build, and its argument FILE=SHA256 */
    QString rpmFile(const QString &name, const QString &evr, const QString &digest,
                    const QString &arch = "x86_64")
    {
        const QString path = m_state + "/files/" + name + '-' + evr + '.' + arch + ".rpm";
        write(path, pkg(name, evr, digest, "@commandline", arch));
        QProcess p;
        p.start("sha256sum", {path});
        p.waitForFinished();
        return path + '=' + QString::fromLatin1(p.readAllStandardOutput()).section(' ', 0, 0);
    }

    QString installed(const QString &name, const QString &field = "DIGEST")
    {
        for (const QByteArray &line : read(m_state + "/installed/" + name).split('\n')) {
            if (line.startsWith(field.toUtf8() + '=')) {
                return QString::fromUtf8(line.mid(field.size() + 1));
            }
        }
        return {};
    }

    /* A digest as install.sh takes it: 64 hexadecimal digits, from @text */
    static QString hex(const QString &text)
    {
        return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_bin = m_tmp.filePath("bin");
        m_state = m_tmp.filePath("state");
        QVERIFY(write(m_bin + "/rpm", kFakeRpm));
        QVERIFY(write(m_bin + "/dnf", kFakeDnf));
        QVERIFY(write(m_bin + "/id", "#!/bin/bash\n[ \"$1\" = -u ] && { echo \"${FAKE_UID:-0}\"; exit 0; }\nexec /usr/bin/id \"$@\"\n"));
        /* as the user: the command after --, as it is */
        QVERIFY(write(m_bin + "/runuser", "#!/bin/bash\necho \"$*\" >> \"$STATE/runuser\"\n"
                                          "while [ $# -gt 0 ] && [ \"$1\" != -- ]; do shift; done\n"
                                          "shift\nexec \"$@\"\n"));
        for (const char *name : {"rpm", "dnf", "id", "runuser"}) {
            QFile::setPermissions(m_bin + '/' + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        m_path = qgetenv("PATH");
        resetState();
        m_targets = Target::read(hostKdeDir());
        QCOMPARE(m_targets.size(), 4);
    }

    void cleanup()
    {
        qputenv("PATH", m_path);
    }

    /* components.conf's packages, as the app reads them */
    void targets()
    {
        QStringList ids;
        for (const Target &t : std::as_const(m_targets)) {
            ids << t.id;
        }
        QCOMPARE(ids, QStringList({"host-kwin", "host-ksvg", "host-kirigami", "host-qtbase"}));
        QCOMPARE(kwin().package, "kwin");
        QCOMPARE(kwin().follows, "plasma-workspace");
        QCOMPARE(kwin().parts, 3);
        QCOMPARE(kwin().against, QStringList({"qt6-qtbase", "qt6-qtdeclarative", "kdecoration", "libplasma"}));
        QVERIFY(kwin().heavy);
        QCOMPARE(ksvg().package, "kf6-ksvg");
        QCOMPARE(ksvg().follows, "kf6-kcoreaddons");
        QCOMPARE(ksvg().parts, 2);
        QVERIFY(ksvg().against.isEmpty());
        QVERIFY(!ksvg().heavy);
        QCOMPARE(kirigami().package, "kf6-kirigami");
        QCOMPARE(kirigami().against, QStringList({"qt6-qtbase", "qt6-qtdeclarative"}));
        QCOMPARE(qtbase().package, "qt6-qtbase");
        QVERIFY(qtbase().follows.isEmpty());
        QCOMPARE(qtbase().reference(), "qt6-qtbase");
        QVERIFY(qtbase().heavy);
        QCOMPARE(qtbase().outDir("/data"), "/data/host-qtbase");
        /* each one's patches are the folder versions.sh's kde_patch_dir names */
        for (const Target &t : std::as_const(m_targets)) {
            QProcess p;
            p.start("/bin/bash", {"-c", ". \"$1\"; kde_patch_dir \"$2\"", "_",
                                  hostKdeDir() + "/research/setup/versions.sh", t.package});
            QVERIFY(p.waitForFinished());
            QCOMPARE("research/patches/" + QString::fromUtf8(p.readAllStandardOutput()).trimmed(), t.patches);
        }
        /* not a system section, or without its package: none */
        QVERIFY(Target::parse("[a]\ntitle=A\nkind=system\n[b]\npackage=b\n").isEmpty());
    }

    /* What versions.sh maps to sets, as the window lists them */
    void setsOfTheRecipe()
    {
        const QList<Set> sets = HostKde::sets(hostKdeDir(), "kwin");
        QCOMPARE(sets.size(), 3);
        /* "${VGD_HOST_KWIN_SET:-hl39y}": the research's override left out */
        QCOMPARE(sets[0].series, "hl39y");
        QCOMPARE(sets[0].tag, "hl39y");
        QCOMPARE(sets[0].versions, QStringList{"6.7.91"});
        QCOMPARE(sets[1].series, "hl39-68");
        QCOMPARE(sets[1].tag, "h18");
        QCOMPARE(sets[1].versions, QStringList{"6.8.0"});
        QCOMPARE(sets[2].series, "h681a");
        QCOMPARE(sets[2].tag, "h681a");
        QCOMPARE(sets[2].versions, QStringList{"6.8.*"});
        /* the libraries' */
        for (const auto &[package, series, version] :
             {std::tuple<QString, QString, QString>{"kf6-ksvg", "k2", "6.31.0"},
              {"kf6-kirigami", "f1", "6.31.0"},
              {"qt6-qtbase", "q4", "6.11.2"}}) {
            const QList<Set> list = HostKde::sets(hostKdeDir(), package);
            QCOMPARE(list.size(), 1);
            QCOMPARE(list[0].series, series);
            QCOMPARE(list[0].versions, QStringList{version});
        }
        /* the guests' are not the host's */
        QVERIFY(HostKde::sets(hostKdeDir(), "kpipewire").isEmpty());
        /* their series files are in the copy */
        for (const Target &t : std::as_const(m_targets)) {
            for (const Set &set : HostKde::sets(hostKdeDir(), t.package)) {
                QVERIFY2(QFileInfo::exists(hostKdeDir() + '/' + t.patches + "/sets/" + set.series + ".series"),
                         qPrintable(set.series));
            }
        }
    }

    /* build.sh --print-recipe: the research's kde_set_for, run by bash */
    void recipeFromTheScript()
    {
        const Recipe r = Recipe::read(hostKdeDir(), "44", {{"kwin", "6.7.91"}, {"kf6-ksvg", "6.31.0"},
                                                          {"kf6-kirigami", "6.31.0"},
                                                          {"qt6-qtbase", "6.11.2"}});
        QVERIFY(r.isValid());
        QCOMPARE(r.fedora, "44");
        QCOMPARE(r.copr, kCopr);
        QCOMPARE(r.series("kwin"), "hl39y");
        QCOMPARE(r.tag("kwin"), "hl39y");
        QCOMPARE(r.series("kf6-ksvg"), "k2");
        QCOMPARE(r.series("kf6-kirigami"), "f1");
        QCOMPARE(r.series("qt6-qtbase"), "q4");
        QCOMPARE(r.tag("qt6-qtbase"), "q4");
        QCOMPARE(r.keys.size(), 4);
        QCOMPARE(Recipe::read(hostKdeDir(), "44", {{"kwin", "6.8.0"}}).series("kwin"), "hl39-68");
        QCOMPARE(Recipe::read(hostKdeDir(), "44", {{"kwin", "6.8.0"}}).tag("kwin"), "h18");
        QCOMPARE(Recipe::read(hostKdeDir(), "44", {{"kwin", "6.8.3"}}).series("kwin"), "h681a");
        /* the research's overrides of its sets, and its switches, stay its own */
        qputenv("VGD_HOST_KWIN_SET", "hl39");
        qputenv("VGD_HOST_KWIN_SET_68", "hl39-681");
        qputenv("VGD_HOST_QT_SET", "q7");
        qputenv("HOST_QT_Q4", "0");
        const Recipe overridden = Recipe::read(hostKdeDir(), "44", {{"kwin", "6.7.91"}, {"qt6-qtbase", "6.11.2"}});
        QCOMPARE(overridden.series("kwin"), "hl39y");
        QCOMPARE(overridden.series("qt6-qtbase"), "q4");
        QCOMPARE(Recipe::read(hostKdeDir(), "44", {{"kwin", "6.8.3"}}).series("kwin"), "h681a");
        for (const char *v : {"VGD_HOST_KWIN_SET", "VGD_HOST_KWIN_SET_68", "VGD_HOST_QT_SET", "HOST_QT_Q4"}) {
            qunsetenv(v);
        }
        /* Fedora 44's own Plasma, one after the sets, the next frameworks and Qt */
        const Recipe none = Recipe::read(hostKdeDir(), "44", {{"kwin", "6.7.5"}, {"kf6-ksvg", "6.32.0"},
                                                             {"qt6-qtbase", "6.11.3"}});
        QVERIFY(none.isValid());
        QVERIFY(none.series("kwin").isEmpty());
        QVERIFY(none.series("kf6-ksvg").isEmpty());
        QVERIFY(none.series("qt6-qtbase").isEmpty());
        QVERIFY(Recipe::read(hostKdeDir(), "44", {{"kwin", "6.9.0"}}).series("kwin").isEmpty());
        /* VGD_FEDORA: the recipe builds for the host's Fedora */
        QCOMPARE(Recipe::read(hostKdeDir(), "45", {{"kwin", "6.7.91"}}).fedora, "45");
        QVERIFY(!Recipe::read(QString(), "44", {{"kwin", "6.7.91"}}).isValid());
        QVERIFY(!Recipe::read(hostKdeDir(), "44", {}).isValid());

        /* each key is its Builds component's, which a build records */
        Builds::Places places;
        places.systemDir = hostKdeDir();
        for (const Target &t : std::as_const(m_targets)) {
            Status s;
            s.offer = Offer::Offered;
            places.system.insert(t.id, s);
        }
        const QList<Builds::Component> all = Builds::load(places);
        QCOMPARE(all.size(), 4);
        for (const Target &t : std::as_const(m_targets)) {
            const Builds::Component c = Builds::find(all, t.id);
            QCOMPARE(c.kind, Builds::Kind::System);
            QCOMPARE(c.heavy, t.heavy);
            QCOMPARE(Builds::key(c, all), r.key(t.package));
            QProcess p;
            p.start("/bin/bash", {hostKdeDir() + "/build.sh", "--print-inputs", t.package});
            QVERIFY(p.waitForFinished());
            QCOMPARE(p.readAllStandardOutput(), Builds::inputLines(c, all));
        }
        /* a package's patches are in its key only: Qt base's are not KWin's */
        QVERIFY(!Builds::inputLines(Builds::find(all, "host-kwin"), all).contains("patches/qtbase"));
        QVERIFY(Builds::inputLines(Builds::find(all, "host-qtbase"), all).contains("patches/qtbase/sets/q4.series"));
        /* hidden: not loaded */
        places.system.insert("host-kwin", Status());
        QCOMPARE(Builds::load(places).size(), 3);
    }

    void patchesBySet()
    {
        Builds::Places places;
        places.systemDir = hostKdeDir();
        for (const Target &t : std::as_const(m_targets)) {
            Status s;
            s.offer = Offer::Offered;
            places.system.insert(t.id, s);
        }
        const QList<Builds::Component> all = Builds::load(places);
        const Builds::Component c = Builds::find(all, "host-kwin");
        const QList<Builds::Patch> patches = Builds::patches(c);
        QCOMPARE(patches.size(), 29 + 14 + 28);
        QCOMPARE(patches.first().origin, "hl39y, for KWin 6.7.91");
        QCOMPARE(patches.last().origin, "h681a, for KWin 6.8.x");
        QCOMPARE(patches.last().name(),
                 "0082-EXP-compositor-a-layer-with-no-damage-keeps-its-buffer-at-run-time.patch");
        QCOMPARE(Builds::subject(c, patches.first()), "Patch100 kwin-vm-frame-timing (ported to 6.8)");
        QVERIFY(Builds::text(c, patches.last()).contains("kwin-skip-empty-layer-render"));

        const Builds::Component svg = Builds::find(all, "host-ksvg");
        const QList<Builds::Patch> k2 = Builds::patches(svg);
        QCOMPARE(k2.size(), 2);
        QCOMPARE(k2.first().origin, "k2, for KSvg 6.31.0");
        QVERIFY(QFileInfo::exists(svg.dir + '/' + k2.first().path));
        QCOMPARE(Builds::patches(Builds::find(all, "host-kirigami")).size(), 1);
        const QList<Builds::Patch> q4 = Builds::patches(Builds::find(all, "host-qtbase"));
        QCOMPARE(q4.size(), 1);
        QCOMPARE(q4.first().name(), "qtbase-wayland-shm-reuse-pools-v3.patch");
        QVERIFY(Builds::subject(Builds::find(all, "host-qtbase"), q4.first()).startsWith("Qt Wayland: a resized window"));
        QCOMPARE(Builds::source(svg).revision, "f659cb4d8fcbcb64d9ff5467ca0ba4891d13a28d");
    }

    void parsing()
    {
        const QByteArray rpm =
            "kwin\t6.7.91\t1.hm4.fc44\tx86_64\t8fe2\t(none)\tkwin-6.7.91-1.hm4.fc44.src.rpm\t1760375000\n"
            "package kwin-devel is not installed\n"
            "plasma-workspace\t6.7.91\t1.fc44\tx86_64\t85f5\tFedora Copr - group @kdesig\t"
            "plasma-workspace-6.7.91-1.fc44.src.rpm\t1760300000\n";
        const QByteArray repos = "kwin @commandline\nplasma-workspace " + QByteArray(kCopr) + '\n';
        const QList<Package> list = System::parsePackages(rpm, repos);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].nevra(), "kwin-6.7.91-1.hm4.fc44.x86_64");
        QCOMPARE(list[0].digest, "8fe2");
        QCOMPARE(list[0].source(), "kwin");
        QVERIFY(list[0].vendor.isEmpty());
        QVERIFY(list[0].isLocal());
        QCOMPARE(list[0].installed.toSecsSinceEpoch(), 1760375000);
        QCOMPARE(list[1].vendor, "Fedora Copr - group @kdesig");
        QCOMPARE(list[1].source(), "plasma-workspace");
        QVERIFY(!list[1].isLocal());
        Package p;
        p.sourceRpm = "qt6-qtbase-6.11.2-3.q4.fc44.src.rpm";
        QCOMPARE(p.source(), "qt6-qtbase");
        p.sourceRpm = "kf6-kirigami-addons-1.13.1-1.fc44.src.rpm";
        QCOMPARE(p.source(), "kf6-kirigami-addons");

        /* dnf5's versionlock list, all of it */
        const QByteArray locks =
            "# Added by 'versionlock add' command on 2026-10-05 17:26:04\n"
            "Package name: virglrenderer\nevr = 1.3.0^20260928gitaafa9bd2-1.fc44\n\n"
            "# Added by 'versionlock add' command on 2026-10-13 19:21:58\n"
            "Package name: kwin\nevr = 6.7.91-1.hm4.fc44\n\n"
            "Package name: kwin-libs\nevr = 6.7.91-1.hm4.fc44\n"
            "Package name: kwin-common\nevr = 0:6.7.5-1.23.fc44\n";
        const QHash<QString, QString> l = System::parseLocks(locks);
        QCOMPARE(l.size(), 4);
        QCOMPARE(l.value("kwin-common"), "6.7.5-1.23.fc44");
        QCOMPARE(l.value("kwin"), "6.7.91-1.hm4.fc44");
        QCOMPARE(l.value("virglrenderer"), "1.3.0^20260928gitaafa9bd2-1.fc44");

        /* the repositories' newest, of the arch installed */
        const QByteArray newest = "qt6-qtbase i686 6.11.3-1.fc44\nqt6-qtbase x86_64 6.11.3-1.fc44\n"
                                  "kf6-ksvg x86_64 6.31.0-1.fc44\nnot-installed x86_64 1-1\n";
        const QHash<QString, QString> n = System::parseNewest(newest, host().packages);
        QCOMPARE(n.size(), 2);
        QCOMPARE(n.value("qt6-qtbase"), "6.11.3-1.fc44");

        const Recipe r = Recipe::parse("fedora=44\ncopr=c\nset.kwin=hl39-68 h18\nkey.kwin=abc\n"
                                       "set.kf6-ksvg=\nkey.kf6-ksvg=def\n");
        QCOMPARE(r.series("kwin"), "hl39-68");
        QCOMPARE(r.tag("kwin"), "h18");
        QCOMPARE(r.key("kwin"), "abc");
        QVERIFY(r.series("kf6-ksvg").isEmpty());
        QCOMPARE(r.key("kf6-ksvg"), "def");

        QCOMPARE(release("6.8.1.1", 3), "6.8.1");
        QCOMPARE(release("6.7.91", 3), "6.7.91");
        QCOMPARE(release("6.31.1", 2), "6.31");
    }

    /* compareEvr() is rpm's own comparison */
    void comparesAsRpm()
    {
        const QList<std::pair<QString, QString>> pairs = {
            {"6.31.0-1.f1.fc44", "6.31.0-1.fc44"},  {"6.31.0-1.k2.fc44", "6.31.0-1.fc44"},
            {"6.11.2-3.q4.fc44", "6.11.2-3.fc44"},  {"6.11.2-3.q4.fc44", "6.11.2-4.fc44"},
            {"6.11.3-1.fc44", "6.11.2-3.q4.fc44"},  {"6.7.91-1.hl39y.fc44", "6.7.91-1.fc44"},
            {"1.0~rc1-1", "1.0-1"},                 {"1.0^git1-1", "1.0-1"},
            {"1.0-1", "1.0-1"},                     {"6.10-1", "6.9-1"},
            {"1:1.0-1", "2.0-1"},                   {"1.0a-1", "1.0-1"},
            {"6.11.2", "6.11.10"},                  {"010-1", "10-1"},
        };
        for (const auto &[a, b] : pairs) {
            QProcess p;
            p.start("rpm", {"--eval", QString("%{lua: print(rpm.vercmp('%1', '%2'))}").arg(a, b)});
            QVERIFY(p.waitForFinished());
            const int rpm = QString::fromUtf8(p.readAllStandardOutput()).trimmed().toInt();
            QVERIFY2(compareEvr(a, b) == rpm, qPrintable(a + " " + b));
            QCOMPARE(compareEvr(b, a), -rpm);
        }
    }

    void buildsAndRecord()
    {
        const QString out = m_tmp.filePath("out");
        QVERIFY(write(out + "/6.7.91-1.hl39.fc44/build.conf",
                      "# a comment\nevr=6.7.91-1.hl39.fc44\nversion=6.7.91\nset=hl39\ntag=hl39\n"
                      "plasma=beta\nfedora=44\nkey=k\nresearch=3653233\nbuilt=2026-10-14T20:00:00Z\n"
                      "rpm=kwin x86_64 d1 kwin/kwin-6.7.91-1.hl39.fc44.x86_64.rpm\n"
                      "rpm=kwin-libs x86_64 d2 kwin/kwin-libs-6.7.91-1.hl39.fc44.x86_64.rpm\n"));
        QVERIFY(write(out + "/6.7.91-1.hl39.fc44/kwin/buildroot-hl39.txt",
                      "kdecoration 6.7.91-1.fc44\nqt6-qtbase 6.11.2-3.fc44\n"));
        QVERIFY(write(out + "/6.8.0-1.h18.fc44/build.conf",
                      "evr=6.8.0-1.h18.fc44\nversion=6.8.0\ntag=h18\nbuilt=2026-10-15T08:00:00Z\n"
                      "rpm=kwin x86_64 d3 kwin/kwin-6.8.0-1.h18.fc44.x86_64.rpm\n"));
        /* a build going on, and one without RPMs */
        QVERIFY(write(out + "/.new/build.conf", "evr=x\nrpm=kwin x86_64 d kwin/x.rpm\n"));
        QVERIFY(write(out + "/broken/build.conf", "evr=6.7.91-2\n"));
        const QList<Build> list = builds(out);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].evr, "6.8.0-1.h18.fc44");
        /* an older Vitrine's builds are KWin's */
        QCOMPARE(list[0].package, "kwin");
        QCOMPARE(list[0].sourceEvr(), "6.8.0-1.fc44");
        QCOMPARE(list[1].series, "hl39");
        QCOMPARE(list[1].research, "3653233");
        QCOMPARE(list[1].rpm("kwin-libs").digest, "d2");
        QCOMPARE(list[1].buildroot.value("qt6-qtbase"), "6.11.2-3.fc44");
        QCOMPARE(list[1].built, QDateTime(QDate(2026, 10, 14), QTime(20, 0), QTimeZone::UTC));

        /* a library's, in its package's folder */
        const QString qt = m_tmp.filePath("out-qt");
        QVERIFY(write(qt + "/6.11.2-3.q4.fc44/build.conf",
                      "package=qt6-qtbase\nevr=6.11.2-3.q4.fc44\nversion=6.11.2\nset=q4\ntag=q4\n"
                      "built=2026-10-31T22:00:00Z\n"
                      "rpm=qt6-qtbase x86_64 q1 qt6-qtbase/qt6-qtbase-6.11.2-3.q4.fc44.x86_64.rpm\n"));
        QVERIFY(write(qt + "/6.11.2-3.q4.fc44/qt6-qtbase/buildroot-q4.txt", "glibc 2.43-3.fc44\n"));
        const Build b = builds(qt).value(0);
        QCOMPARE(b.package, "qt6-qtbase");
        QCOMPARE(b.sourceEvr(), "6.11.2-3.fc44");
        QCOMPARE(b.buildroot.value("glibc"), "2.43-3.fc44");

        const Record r = Record::of(list[1], QDateTime(QDate(2026, 10, 14), QTime(21, 0), QTimeZone::UTC),
                                    kwin().against);
        QCOMPARE(r.digests.value("kwin"), "d1");
        QCOMPARE(r.tag, "hl39");
        QCOMPARE(r.buildroot.value("qt6-qtbase"), "6.11.2");
        QCOMPARE(r.buildroot.value("kdecoration"), "6.7.91");
        QVERIFY(r.write(Record::path(out)));
        const Record back = Record::read(Record::path(out));
        QCOMPARE(back.evr, r.evr);
        QCOMPARE(back.tag, r.tag);
        QCOMPARE(back.when, r.when);
        QCOMPARE(back.digests, r.digests);
        QCOMPARE(back.buildroot, r.buildroot);
        QVERIFY(!Record::read(m_tmp.filePath("nothing")).isValid());
    }

    /* Not a Fedora Plasma host, or a package that is not installed: not shown at all */
    void hidden()
    {
        System s = host();
        QCOMPARE(status(kwin(), s, recipe(), {}, {}).offer, Offer::Offered);
        QVERIFY(!status(kwin(), s, recipe(), {}, {}).summary().isEmpty());
        s.rpm = false;
        QCOMPARE(status(kwin(), s, recipe(), {}, {}).offer, Offer::Hidden);
        QCOMPARE(status(qtbase(), s, recipe(), {}, {}).offer, Offer::Hidden);
        s = host();
        s.fedora.clear();
        QCOMPARE(status(ksvg(), s, recipe(), {}, {}).offer, Offer::Hidden);
        s = host();
        s.packages.removeIf([](const Package &p) { return p.name.startsWith("kwin"); });
        QCOMPARE(status(kwin(), s, recipe(), {}, {}).offer, Offer::Hidden);
        QVERIFY(status(kwin(), s, recipe(), {}, {}).summary().isEmpty());
        /* the others stay */
        QCOMPARE(status(ksvg(), s, recipe(), {}, {}).offer, Offer::Offered);
        s.packages.removeIf([](const Package &p) { return p.name == "plasma-workspace"; });
        QCOMPARE(status(ksvg(), s, recipe(), {}, {}).offer, Offer::Hidden);
    }

    /* Fedora 44's own Plasma 6.7.5: no KWin set, the libraries' though; another Fedora or Plasma: not offered */
    void notOffered()
    {
        const System stock = host("6.7.5", "6.7.5-1.fc44", "6ecc2dfaa0dc41e5ad51e007707a786b");
        Status s = status(kwin(), stock, recipe(QString(), QString()), {}, {});
        QCOMPARE(s.offer, Offer::NoSet);
        QCOMPARE(s.plasmaFrom, "fedora");
        QCOMPARE(s.version, "6.7.5");
        QCOMPARE(s.summary(), "No host set for KWin 6.7.5");
        QString why;
        QVERIFY(!s.canBuild(&why));
        QVERIFY(why.contains("6.7.5"));
        QVERIFY(!s.canInstall());
        QCOMPARE(s.owner, Owner::Distribution);
        /* the user's: Fedora's Plasma, the frameworks' and Qt's sets */
        for (const Target &t : {ksvg(), kirigami(), qtbase()}) {
            s = status(t, stock, recipe(QString(), QString()), {}, {});
            QCOMPARE(s.offer, Offer::Offered);
            QCOMPARE(s.summary(), "Not built");
            QVERIFY(s.canBuild());
        }
        QCOMPARE(status(qtbase(), stock, recipe(), {}, {}).version, "6.11.2");
        QCOMPARE(status(qtbase(), stock, recipe(), {}, {}).names,
                 QStringList({"qt6-qtbase", "qt6-qtbase-common", "qt6-qtbase-gui"}));

        Recipe other = recipe();
        other.fedora = "45";
        s = status(ksvg(), host(), other, {}, {});
        QCOMPARE(s.offer, Offer::Unsupported);
        QVERIFY(s.why.contains("Fedora 45"));
        QVERIFY(!s.canBuild());

        s = status(kwin(), host("6.7.91", "6.7.91-1.fc44", "copr:copr.fedorainfracloud.org:someone:plasma"),
                   recipe(), {}, {});
        QCOMPARE(s.offer, Offer::Unsupported);
        QVERIFY(s.why.contains("someone"));
        s = status(kwin(), host("6.7.91", "6.7.91-1.fc44", "updates-testing"), recipe(), {}, {});
        QCOMPARE(s.offer, Offer::Offered);
        QCOMPARE(s.plasmaFrom, "testing");
        QCOMPARE(status(kwin(), host(), Recipe(), {}, {}).offer, Offer::Unsupported);
        /* no set for this version of the library */
        Recipe noQt = recipe();
        noQt.sets.remove("qt6-qtbase");
        s = status(qtbase(), host(), noQt, {}, {});
        QCOMPARE(s.offer, Offer::NoSet);
        QCOMPARE(s.summary(), "No host set for Qt base 6.11.2");
    }

    void notBuiltThenBuilt()
    {
        const System system = host();
        Status s = status(kwin(), system, recipe(), {}, {});
        QCOMPARE(s.offer, Offer::Offered);
        QCOMPARE(s.plasmaFrom, "beta");
        QCOMPARE(s.summary(), "Not built");
        QVERIFY(s.canBuild());
        QVERIFY(!s.canInstall());
        QVERIFY(!s.canRevert());
        QVERIFY(!s.wantsUpdates());

        s = status(kwin(), system, recipe(), {build()}, {});
        QVERIFY(s.build.isValid());
        QVERIFY(s.differs.isEmpty());
        QVERIFY(!s.recipeChanged);
        QCOMPARE(s.summary(), "Built, not installed");
        QVERIFY(s.canInstall());

        /* a newer recipe: still installable, out of date as a build */
        s = status(kwin(), system, recipe(), {build("6.7.91-1.hl39.fc44", "6.11.2-3.fc44", "old")}, {});
        QVERIFY(s.recipeChanged);
        QVERIFY(s.canInstall());

        /* against another Qt than the system's: not before they match */
        s = status(kwin(), system, recipe(), {build("6.7.91-1.hl39.fc44", "6.11.3-1.fc44")}, {});
        QCOMPARE(s.differs, QStringList{"qt6-qtbase 6.11.3 6.11.2"});
        QString why;
        QVERIFY(!s.canInstall(&why));
        QVERIFY(why.contains("6.11.3"));
        QCOMPARE(s.summary(), "Built against Qt 6.11.3 (this computer has 6.11.2)");

        /* without versionlock or pkexec: a plain word, before any password */
        System bare = system;
        bare.versionlock = false;
        QVERIFY(!status(kwin(), bare, recipe(), {build()}, {}).canInstall(&why));
        QVERIFY(why.contains("versionlock"));
        bare = system;
        bare.pkexec = false;
        QVERIFY(!status(kwin(), bare, recipe(), {build()}, {}).canInstall(&why));
        QVERIFY(why.contains("pkexec"));

        /* for another Plasma, another origin, another Fedora: none of them is for this one */
        s = status(kwin(), host("6.8.0", "6.8.0-1.fc44"), recipe("hl39-68", "h18"), {build()}, {});
        QVERIFY(!s.build.isValid());
        QCOMPARE(s.other.evr, "6.7.91-1.hl39.fc44");
        QCOMPARE(s.summary(), "Built for KWin 6.7.91; this computer has Plasma 6.8.0");
        QVERIFY(!s.canInstall(&why));
        QVERIFY(why.contains("6.8.0"));
        s = status(kwin(), system, recipe(), {build("6.7.91-1.hl39.fc44", "6.11.2-3.fc44", "key1", "fedora")}, {});
        QVERIFY(!s.build.isValid());
        Build b = build();
        b.fedora = "43";
        QVERIFY(!status(kwin(), system, recipe(), {b}, {}).build.isValid());

        /* the repositories have the next Plasma's KWin, the system not yet: a
           build takes it, and waits for the system before it installs */
        System ahead = system;
        ahead.newest.insert("kwin", "6.8.0-1.fc44");
        s = status(kwin(), ahead, recipe("hl39-68", "h18"), {build("6.8.0-1.h18.fc44")}, {});
        QCOMPARE(s.version, "6.8.0");
        QVERIFY(s.build.isValid());
        QCOMPARE(s.differs, QStringList{"plasma-workspace 6.8.0 6.7.91"});
        QCOMPARE(s.summary(), "Built for Plasma 6.8.0 (this computer has 6.7.91)");
        QVERIFY(!s.canInstall(&why));
        QVERIFY(why.contains("dnf upgrade"));
    }

    /* KSvg, Kirigami and Qt base: built, then installable, all of a package's at once */
    void librariesBuilt()
    {
        const System system = host("6.7.5", "6.7.5-1.fc44", "updates");
        const Build svg = libraryBuild("kf6-ksvg", "6.31.0-1.k2.fc44", "k2",
                                       {"kf6-ksvg", "kf6-ksvg-devel"}, "6.11.2-3.fc44", "fedora");
        Status s = status(ksvg(), system, recipe(), {svg}, {});
        QCOMPARE(s.version, "6.31.0");
        QVERIFY(s.build.isValid());
        QCOMPARE(s.summary(), "Built, not installed");
        QVERIFY(s.canInstall());
        /* Kirigami: built against another Qt than the system's: it waits */
        const Build kir = libraryBuild("kf6-kirigami", "6.31.0-1.f1.fc44", "f1", {"kf6-kirigami"},
                                       "6.11.3-1.fc44", "fedora");
        s = status(kirigami(), system, recipe(), {kir}, {});
        QCOMPARE(s.differs, QStringList({"qt6-qtbase 6.11.3 6.11.2", "qt6-qtdeclarative 6.11.3 6.11.2"}));
        QVERIFY(!s.canInstall());
        /* KSvg is built against no private API: another Qt is fine */
        const Build svg2 = libraryBuild("kf6-ksvg", "6.31.0-1.k2.fc44", "k2", {"kf6-ksvg"},
                                        "6.11.3-1.fc44", "fedora");
        QVERIFY(status(ksvg(), system, recipe(), {svg2}, {}).canInstall());
        /* a build of the frameworks' next release: the system first */
        const Build next = libraryBuild("kf6-ksvg", "6.32.0-1.k3.fc44", "k3", {"kf6-ksvg"},
                                        "6.11.2-3.fc44", "fedora");
        System ahead = system;
        ahead.newest.insert("kf6-ksvg", "6.32.0-1.fc44");
        s = status(ksvg(), ahead, recipe(), {next}, {});
        QCOMPARE(s.version, "6.32.0");
        QCOMPARE(s.differs, QStringList{"kf6-kcoreaddons 6.32.0 6.31.0"});
        QCOMPARE(s.summary(), "Built for KDE Frameworks 6.32.0 (this computer has 6.31.0)");
        /* Qt base: a build of the next Qt installs over this one, its other modules come along */
        const Build qt = libraryBuild("qt6-qtbase", "6.11.3-1.q5.fc44", "q5",
                                      {"qt6-qtbase", "qt6-qtbase-common", "qt6-qtbase-gui"},
                                      "6.11.3-1.fc44", "fedora");
        ahead = system;
        ahead.newest.insert("qt6-qtbase", "6.11.3-1.fc44");
        s = status(qtbase(), ahead, recipe(), {qt}, {});
        QCOMPARE(s.version, "6.11.3");
        QVERIFY(s.differs.isEmpty());
        QVERIFY(s.canInstall());
        /* and an older one is for another version */
        const Build old = libraryBuild("qt6-qtbase", "6.11.2-3.q4.fc44", "q4", {"qt6-qtbase"},
                                       "6.11.2-3.fc44", "fedora");
        s = status(qtbase(), ahead, recipe(), {old}, {});
        QVERIFY(!s.build.isValid());
        QVERIFY(s.basisInRepositories);
        QCOMPARE(s.summary(), "Built for Qt base 6.11.2; the repositories have 6.11.3");
    }

    /* The research's own build on this machine: not Vitrine's, whatever its version */
    void foreign()
    {
        System system = host();
        for (Package &p : system.packages) {
            if (p.source() == "kwin") {
                p.release = "1.hm4.fc44";
                p.digest = "hm4-" + p.name;
                p.repo = "@commandline";
                system.locks.insert(p.name, "6.7.91-1.hm4.fc44");
            }
        }
        Status s = status(kwin(), system, recipe(), {build()}, {});
        QCOMPARE(s.owner, Owner::Foreign);
        QVERIFY(s.locked);
        QCOMPARE(s.lockedAt, "6.7.91-1.hm4.fc44");
        QCOMPARE(s.summary(), "Another KWin is installed: 6.7.91-1.hm4.fc44");
        QString why;
        QVERIFY(!s.canRevert(&why));
        QVERIFY(why.contains("leaves it alone"));
        /* installed over it only after a warning: the window's */
        QVERIFY(s.canInstall());
        QVERIFY(s.outdated.isEmpty());
        QVERIFY(!s.wantsUpdates());
        /* another package's lock is not KWin's */
        QCOMPARE(status(ksvg(), system, recipe(), {}, {}).owner, Owner::Distribution);

        /* the repositories' Qt base, one of its packages locked by someone else */
        system = host();
        system.locks.insert("qt6-qtbase-gui", "6.11.2-3.fc44");
        s = status(qtbase(), system, recipe(), {}, {});
        QCOMPARE(s.owner, Owner::Foreign);
        QCOMPARE(s.summary(), "Qt base is version-locked, not by Vitrine");
        /* the research's own KSvg k2, from a file */
        system = host();
        for (Package &p : system.packages) {
            if (p.name == "kf6-ksvg") {
                p.release = "1.k2.fc44";
                p.digest = "research-k2";
                p.repo = "@commandline";
            }
        }
        s = status(ksvg(), system, recipe(), {}, {});
        QCOMPARE(s.summary(), "Another KSvg is installed: 6.31.0-1.k2.fc44");
    }

    void installedByVitrine()
    {
        const Build b = build();
        const Record record = Record::of(b, QDateTime::currentDateTimeUtc(), kwin().against);
        System system = withVitrines(host(), b);
        Status s = status(kwin(), system, recipe(), {b}, record);
        QCOMPARE(s.owner, Owner::Vitrine);
        QVERIFY(s.locked);
        QCOMPARE(s.summary(), "Installed and locked");
        QVERIFY(s.canRevert());
        QString why;
        QVERIFY(!s.canInstall(&why));
        QVERIFY(why.contains("installed and locked"));
        QVERIFY(s.wantsUpdates());
        QVERIFY(!s.isOutdated());

        /* by the record alone, its build gone; by the build alone, without a record */
        QCOMPARE(status(kwin(), system, recipe(), {}, record).owner, Owner::Vitrine);
        QCOMPARE(status(kwin(), system, recipe(), {b}, {}).owner, Owner::Vitrine);

        /* a rebuild of it (a newer recipe): ready to replace it */
        Build rebuilt = build("6.7.91-1.hl39.fc44", "6.11.2-3.fc44", "key2");
        for (Build::Rpm &rpm : rebuilt.rpms) {
            rpm.digest += "-2";
        }
        rebuilt.built = b.built.addSecs(3600);
        s = status(kwin(), system, recipe(), {rebuilt, b}, record);
        QCOMPARE(s.owner, Owner::Vitrine);
        QCOMPARE(s.summary(), "Installed and locked; another build is ready");
        QVERIFY(s.canInstall());

        /* the lock gone: installing again locks it */
        system = withVitrines(host(), b, false);
        s = status(kwin(), system, recipe(), {b}, record);
        QCOMPARE(s.summary(), "Installed, not version-locked");
        QVERIFY(s.canInstall());

        /* installed after the session's KWin started: from the next login */
        system = withVitrines(host(), b);
        system.session = QDateTime::fromSecsSinceEpoch(1759990000, QTimeZone::UTC);
        s = status(kwin(), system, recipe(), {b}, record);
        QVERIFY(s.nextLogin);
        QCOMPARE(s.summary(), "Installed and locked, from the next login");
        system.session = QDateTime::fromSecsSinceEpoch(1760010000, QTimeZone::UTC);
        QVERIFY(!status(kwin(), system, recipe(), {b}, record).nextLogin);
    }

    /* Plasma or Qt updated past Vitrine's KWin: out of date, said plainly */
    void outdatedByUpdates()
    {
        const Build b = build();
        const Record record = Record::of(b, QDateTime::currentDateTimeUtc(), kwin().against);
        System system = withVitrines(host("6.8.0", "6.8.0-1.fc44"), b);
        Status s = status(kwin(), system, recipe("hl39-68", "h18"), {b}, record);
        QCOMPARE(s.owner, Owner::Vitrine);
        QCOMPARE(s.version, "6.8.0");
        QCOMPARE(s.outdated, QStringList{"plasma-workspace 6.7.91 6.8.0"});
        QVERIFY(s.isOutdated());
        QCOMPARE(s.summary(), "Out of date: Plasma 6.8.0 is installed");
        QVERIFY(s.canRevert());
        QVERIFY(s.canBuild());
        QVERIFY(!s.canInstall());
        /* once built for it: installable */
        s = status(kwin(), system, recipe("hl39-68", "h18"),
                   {build("6.8.0-1.h18.fc44", "6.11.2-3.fc44"), b}, record);
        QVERIFY(s.canInstall());
        QVERIFY(s.isOutdated());

        /* no set for the new Plasma: reverting is what is left */
        s = status(kwin(), withVitrines(host("6.9.0", "6.9.0-1.fc44"), b), recipe(QString(), QString()), {b},
                   record);
        QCOMPARE(s.offer, Offer::NoSet);
        QVERIFY(s.isOutdated());
        QCOMPARE(s.summary(), "Out of date: Plasma 6.9.0 is installed");
        QVERIFY(s.canRevert());
        QVERIFY(!s.canBuild());
        QVERIFY(!s.wantsUpdates());

        /* Qt: the version a lock lets through, which KWin's private API does not */
        system = moved(withVitrines(host(), b), "qt6-qtbase", "6.11.3-1.fc44");
        s = status(kwin(), system, recipe(), {b}, record);
        QCOMPARE(s.outdated, QStringList{"qt6-qtbase 6.11.2 6.11.3"});
        QCOMPARE(s.summary(), "Out of date: Qt 6.11.3 is installed");
        /* a rebuild of the same Qt (another release) is not an update of it */
        system = moved(withVitrines(host(), b), "qt6-qtbase", "6.11.2-4.fc44");
        QVERIFY(status(kwin(), system, recipe(), {b}, record).outdated.isEmpty());
    }

    /* The KDE Frameworks past a locked KSvg or Kirigami, Qt past Kirigami */
    void librariesOutdated()
    {
        const Build svg = libraryBuild("kf6-ksvg", "6.31.0-1.k2.fc44", "k2", {"kf6-ksvg"});
        const Build kir = libraryBuild("kf6-kirigami", "6.31.0-1.f1.fc44", "f1", {"kf6-kirigami"});
        System system = withVitrines(withVitrines(host(), svg), kir);
        Status s = status(ksvg(), system, recipe(), {svg}, {});
        QCOMPARE(s.summary(), "Installed and locked");
        QCOMPARE(status(kirigami(), system, recipe(), {kir}, {}).summary(), "Installed and locked");

        /* KF 6.32 came in around them: the set is looked up for it */
        system = moved(system, "kf6-kcoreaddons", "6.32.0-1.fc44");
        Recipe none = recipe();
        none.sets.remove("kf6-ksvg");
        s = status(ksvg(), system, none, {svg}, {});
        QCOMPARE(s.version, "6.32.0");
        QCOMPARE(s.outdated, QStringList{"kf6-kcoreaddons 6.31.0 6.32.0"});
        QCOMPARE(s.summary(), "Out of date: KDE Frameworks 6.32.0 is installed");
        QCOMPARE(s.offer, Offer::NoSet);
        QVERIFY(!s.canBuild());
        QVERIFY(s.canRevert());
        /* a framework's own bugfix release (6.31.1 of one) is the same release */
        system = moved(withVitrines(host(), svg), "kf6-kcoreaddons", "6.31.1-1.fc44");
        QVERIFY(status(ksvg(), system, recipe(), {svg}, {}).outdated.isEmpty());

        /* Kirigami against another Qt */
        const Record record = Record::of(kir, QDateTime::currentDateTimeUtc(), kirigami().against);
        QCOMPARE(record.buildroot.value("qt6-qtdeclarative"), "6.11.2");
        system = moved(moved(withVitrines(host(), kir), "qt6-qtbase", "6.11.3-1.fc44"),
                       "qt6-qtdeclarative", "6.11.3-1.fc44");
        s = status(kirigami(), system, recipe(), {kir}, record);
        QCOMPARE(s.outdated, QStringList({"qt6-qtbase 6.11.2 6.11.3", "qt6-qtdeclarative 6.11.2 6.11.3"}));
        QCOMPARE(s.summary(), "Out of date: Qt 6.11.3 is installed");
        /* KSvg: no private API, no matter */
        system = moved(withVitrines(host(), svg), "qt6-qtbase", "6.11.3-1.fc44");
        QVERIFY(!status(ksvg(), system, recipe(), {svg}, {}).isOutdated());
    }

    /* An update the lock keeps out: said, never out of date */
    void heldBack()
    {
        const Build qt = libraryBuild("qt6-qtbase", "6.11.2-3.q4.fc44", "q4",
                                      {"qt6-qtbase", "qt6-qtbase-common", "qt6-qtbase-gui"});
        const Record record = Record::of(qt, QDateTime::currentDateTimeUtc(), {});
        System system = withVitrines(host(), qt);
        system.newest.insert("qt6-qtbase", "6.11.2-3.fc44");
        Status s = status(qtbase(), system, recipe(), {qt}, record);
        QCOMPARE(s.owner, Owner::Vitrine);
        QVERIFY(s.heldBack.isEmpty());
        QCOMPARE(s.summary(), "Installed and locked");

        /* Fedora's next release of it: held back, a rebuild takes it */
        system.newest.insert("qt6-qtbase", "6.11.2-4.fc44");
        s = status(qtbase(), system, recipe(), {qt}, record);
        QCOMPARE(s.heldBack, "6.11.2-4.fc44");
        QVERIFY(!s.isOutdated());
        QCOMPARE(s.summary(), "Installed and locked; the update to 6.11.2-4.fc44 waits");
        QCOMPARE(s.version, "6.11.2");
        /* Qt 6.11.3, the research's set not there yet */
        system.newest.insert("qt6-qtbase", "6.11.3-1.fc44");
        Recipe none = recipe();
        none.sets.remove("qt6-qtbase");
        s = status(qtbase(), system, none, {qt}, record);
        QCOMPARE(s.version, "6.11.3");
        QCOMPARE(s.offer, Offer::NoSet);
        QCOMPARE(s.heldBack, "6.11.3-1.fc44");
        QVERIFY(s.canRevert());
        /* older than the build: not held back (a stale cache) */
        system.newest.insert("qt6-qtbase", "6.11.2-2.fc44");
        QVERIFY(status(qtbase(), system, recipe(), {qt}, record).heldBack.isEmpty());
        /* the tag from the build when the record has none (an older one) */
        Record old = record;
        old.tag.clear();
        old.series.clear();
        system.newest.insert("qt6-qtbase", "6.11.2-3.fc44");
        QVERIFY(status(qtbase(), system, recipe(), {qt}, {}).heldBack.isEmpty());
        /* not Vitrine's: nothing of the kind */
        System distro = host();
        distro.newest.insert("qt6-qtbase", "6.11.3-1.fc44");
        QVERIFY(status(qtbase(), distro, recipe(), {}, {}).heldBack.isEmpty());

        /* in the Builds window: out of date as a build, with a set for it */
        system.newest.insert("qt6-qtbase", "6.11.2-4.fc44");
        Builds::Places places;
        places.systemDir = hostKdeDir();
        places.system.insert("host-qtbase", status(qtbase(), system, recipe(), {qt}, record));
        const QList<Builds::Component> all = Builds::load(places);
        const Builds::Status b = Builds::status(Builds::find(all, "host-qtbase"), all, places);
        QCOMPARE(b.state, Builds::Status::State::Outdated);
        QCOMPARE(Builds::describe(b.changes.last()),
                 "Built from 6.11.2-3.fc44, and the repositories have 6.11.2-4.fc44 now");
        QCOMPARE(Builds::summary(b.changes), "a newer source package");
    }

    /* The version a set is looked up for */
    void versionPicking()
    {
        const Build svg = libraryBuild("kf6-ksvg", "6.31.0-1.k2.fc44", "k2", {"kf6-ksvg"});
        System system = host();
        QCOMPARE(versionOf(ksvg(), system, {}), "6.31.0");
        /* the distribution's own: its version, even when the frameworks differ */
        system = moved(host(), "kf6-ksvg", "6.31.1-1.fc44");
        QCOMPARE(versionOf(ksvg(), system, {}), "6.31.1");
        /* Vitrine's: the frameworks' */
        system = moved(withVitrines(host(), svg), "kf6-kcoreaddons", "6.32.0-1.fc44");
        QCOMPARE(versionOf(ksvg(), system, digests(ksvg(), {svg}, {})), "6.32.0");
        /* the repositories' newest, when newer */
        system = host();
        system.newest.insert("kf6-ksvg", "6.32.0-1.fc44");
        QCOMPARE(versionOf(ksvg(), system, {}), "6.32.0");
        system.newest.insert("kf6-ksvg", "6.30.0-1.fc44");
        QCOMPARE(versionOf(ksvg(), system, {}), "6.31.0");
        /* KWin: Plasma's while not the distribution's */
        const Build b = build();
        system = withVitrines(host("6.8.0", "6.8.0-1.fc44"), b);
        QCOMPARE(versionOf(kwin(), system, digests(kwin(), {b}, {})), "6.8.0");
        QCOMPARE(digests(kwin(), {b, b}, Record::of(b, QDateTime(), {})), QStringList{"6.7.91-1.hl39.fc44-kwin"});
    }

    void commandLines()
    {
        const QString out = m_tmp.filePath("cmd");
        Build b = build();
        b.dir = out;
        for (const Build::Rpm &rpm : b.rpms) {
            QVERIFY(write(out + '/' + rpm.file, rpm.name.toUtf8()));
        }
        qputenv("PATH", m_bin.toUtf8() + ":/usr/bin:/bin");
        QVERIFY(write(m_bin + "/pkexec", "#!/bin/bash\n"));
        QFile::setPermissions(m_bin + "/pkexec", QFile::ReadOwner | QFile::ExeOwner);
        const QStringList install = installCommand("/src/host/kde", b);
        QCOMPARE(install.mid(0, 4), QStringList({m_bin + "/pkexec", "/usr/bin/bash",
                                                 "/src/host/kde/install.sh", "install"}));
        QCOMPARE(install.size(), 4 + b.rpms.size());
        /* sha256sum's hash of the files */
        QCOMPARE(install[4], out + "/kwin/kwin-6.7.91-1.hl39.fc44.x86_64.rpm=" +
                                 QCryptographicHash::hash("kwin", QCryptographicHash::Sha256).toHex());

        Record record;
        record.digests.insert("kwin", "recorded");
        record.digests.insert("kwin-libs", "not-kwin");
        const QStringList revert =
            revertCommand("/src/host/kde", {{"kwin", digests(kwin(), {b, build()}, record)},
                                            {"kf6-ksvg", {"s1"}}});
        QCOMPARE(revert.mid(2), QStringList({"/src/host/kde/install.sh", "revert", "kf6-ksvg=s1",
                                             "kwin=recorded", "kwin=6.7.91-1.hl39.fc44-kwin"}));
        QFile::remove(m_bin + "/pkexec");
    }

    /* The system as rpm and dnf say it is, and the session's KWin from /proc */
    void readsTheSystem()
    {
        resetState();
        QVERIFY(write(m_state + "/installed/kwin", pkg("kwin", "6.7.91-1.hm4.fc44", "hm4", "@commandline")));
        QVERIFY(write(m_state + "/installed/kwin-common", pkg("kwin-common", "6.7.91-1.hm4.fc44", "hm4c", "@commandline")));
        QVERIFY(write(m_state + "/installed/plasma-workspace", pkg("plasma-workspace", "6.7.91-1.fc44", "pw", kCopr)));
        QVERIFY(write(m_state + "/installed/qt6-qtbase", pkg("qt6-qtbase", "6.11.2-3.fc44", "qt")));
        QVERIFY(write(m_state + "/installed/qt6-qtbase-gui", pkg("qt6-qtbase-gui", "6.11.2-3.fc44", "qtg")));
        QVERIFY(write(m_state + "/installed/qt6-qtbase-debuginfo", pkg("qt6-qtbase-debuginfo", "6.11.2-3.fc44", "dbg")));
        QVERIFY(write(m_state + "/installed/kf6-kirigami", pkg("kf6-kirigami", "6.31.0-1.fc44", "kir")));
        QVERIFY(write(m_state + "/installed/kf6-kirigami-addons", pkg("kf6-kirigami-addons", "1.13.1-1.fc44", "add")));
        QVERIFY(write(m_state + "/installed/kf6-kcoreaddons", pkg("kf6-kcoreaddons", "6.31.0-1.fc44", "kca")));
        QVERIFY(write(m_state + "/installed/kwin-x11", pkg("kwin-x11", "6.7.5-1.fc44", "x11")));
        QVERIFY(write(m_state + "/locks/kwin", "6.7.91-1.hm4.fc44"));
        QVERIFY(write(m_state + "/available/qt6-qtbase", "qt6-qtbase i686 6.11.3-1.fc44\nqt6-qtbase x86_64 6.11.3-1.fc44\n"));
        QVERIFY(write(m_tmp.filePath("os-release"), "NAME=\"Fedora Linux\"\nID=fedora\nVERSION_ID=44\n"));
        /* a KWin of this user's, and another's */
        const QString proc = m_tmp.filePath("proc");
        QVERIFY(write(proc + "/stat", "cpu  1 2 3\nbtime 1760000000\nprocesses 5\n"));
        QVERIFY(write(proc + "/4242/comm", "kwin_wayland\n"));
        QVERIFY(write(proc + "/4242/stat",
                      "4242 (kwin_wayland) S 1 4242 4242 0 -1 4194560 1 0 0 0 1 1 0 0 20 0 9 0 " +
                          QByteArray::number(qint64(sysconf(_SC_CLK_TCK)) * 100) + " 1 1\n"));
        QVERIFY(write(proc + "/4243/comm", "plasmashell\n"));
        QVERIFY(write(proc + "/self/comm", "kwin_wayland\n"));

        qputenv("PATH", m_bin.toUtf8() + ":/usr/bin:/bin");
        qputenv("STATE", m_state.toUtf8());
        const System s = System::read(m_targets, m_tmp.filePath("os-release"), proc);
        QVERIFY(s.rpm);
        QVERIFY(s.versionlock);
        QCOMPARE(s.fedora, "44");
        QCOMPARE(s.of("kwin").size(), 2);
        QCOMPARE(s.package("kwin").release, "1.hm4.fc44");
        QCOMPARE(s.package("kwin").repo, "@commandline");
        QCOMPARE(s.package("plasma-workspace").repo, kCopr);
        QCOMPARE(s.plasma(), "6.7.91");
        QVERIFY(s.isPlasmaHost());
        QCOMPARE(s.locks.value("kwin"), "6.7.91-1.hm4.fc44");
        QCOMPARE(s.session.toSecsSinceEpoch(), 1760000100);
        /* the subpackages of the source packages, not their debug information
           nor other source packages named alike */
        QStringList qt;
        for (const Package &p : s.of("qt6-qtbase")) {
            qt << p.name;
        }
        QCOMPARE(qt, QStringList({"qt6-qtbase", "qt6-qtbase-gui"}));
        QCOMPARE(s.of("kf6-kirigami").size(), 1);
        QVERIFY(!s.package("kwin-x11").isValid());
        QVERIFY(!s.package("kf6-kirigami-addons").isValid());
        QVERIFY(s.package("kf6-kcoreaddons").isValid());
        QCOMPARE(s.newest.value("qt6-qtbase"), "6.11.3-1.fc44");
        /* what it ran: nothing that writes */
        const QByteArray calls = read(m_state + "/calls");
        QVERIFY2(calls.contains("dnf -q -C repoquery --installed"), calls.constData());
        /* the system's metadata cache, not a copy of it in the user's */
        QVERIFY2(calls.contains("dnf -q -C repoquery --setopt=cachedir=/var/cache/libdnf5 --latest-limit=1"),
                 calls.constData());
        QVERIFY(!calls.contains("versionlock add"));

        /* not Fedora; no dnf5 */
        QVERIFY(write(m_tmp.filePath("os-release"), "ID=debian\nVERSION_ID=13\n"));
        QVERIFY(System::read(m_targets, m_tmp.filePath("os-release"), proc).fedora.isEmpty());
        qputenv("PATH", "/nonexistent");
        QVERIFY(!System::read(m_targets, m_tmp.filePath("os-release"), proc).rpm);
    }

    /* install.sh's install: the right dnf action, the lock, the old locks back on failure */
    void installScriptInstalls()
    {
        int status = -1;
        resetState();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            QVERIFY(write(m_state + "/installed/" + name, pkg(name, "6.7.91-1.fc44", "distro")));
        }
        const QStringList files = {rpmFile("kwin", "6.7.91-1.hl39.fc44", "v-kwin"),
                                   rpmFile("kwin-common", "6.7.91-1.hl39.fc44", "v-common"),
                                   rpmFile("kwin-libs", "6.7.91-1.hl39.fc44", "v-libs"),
                                   rpmFile("kwin-devel", "6.7.91-1.hl39.fc44", "v-devel")};
        QString out = installScript(QStringList{"install"} + files, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY(out.contains("vitrine-kde: left out: kwin-devel, not installed"));
        QVERIFY(out.contains("vitrine-kde: installed: kwin-6.7.91-1.hl39.fc44.x86_64"));
        QVERIFY(out.contains("vitrine-kde: locked: kwin kwin-common kwin-libs"));
        QVERIFY(out.trimmed().endsWith("vitrine-kde: done"));
        QCOMPARE(installed("kwin"), "v-kwin");
        QCOMPARE(installed("kwin-libs"), "v-libs");
        QVERIFY(!QFileInfo::exists(m_state + "/installed/kwin-devel"));
        QCOMPARE(read(m_state + "/actions"), "upgrade\n");
        QCOMPARE(read(m_state + "/locks/kwin").trimmed(), "6.7.91-1.hl39.fc44");
        /* the set's switches: an entry each for every user, and the files made now for the one
           installing */
        const QByteArray switches = read(m_state + "/tmpfiles/vitrine-host-kwin.conf");
        QVERIFY2(switches.startsWith("# Vitrine's host KWin (host/kde/install.sh)"), switches.constData());
        QCOMPARE(switches.count("\nf %t/kwin-"), 5);
        QVERIFY(switches.contains("\nf %t/kwin-scanout-variance-cap 0644 - - - 500\n"));
        QVERIFY(out.contains("vitrine-kde: step: turning the set's run-time switches on"));
        QVERIFY2(read(m_state + "/runuser").contains(" -- env XDG_RUNTIME_DIR=" + runDir().toUtf8() +
                                                     " systemd-tmpfiles --user --create "),
                 read(m_state + "/runuser").constData());
        QCOMPARE(read(runDir() + "/kwin-scanout-variance-cap"), "500");
        QCOMPARE(read(runDir() + "/kwin-skip-empty-layer-render"), "1");
        QCOMPARE(QDir(runDir()).entryList(QDir::Files).size(), 5);
        /* its copies went with it */
        QCOMPARE(QDir(m_tmp.filePath("tmp")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries), QStringList());

        /* the same build again: only the lock */
        QFile::remove(m_state + "/actions");
        QFile::remove(m_state + "/locks/kwin");
        out = installScript(QStringList{"install"} + files, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY(!QFileInfo::exists(m_state + "/actions"));
        QVERIFY(QFileInfo::exists(m_state + "/locks/kwin"));

        /* a rebuild of the same version (a Qt update): reinstalled, not "installed already" */
        const QStringList rebuilt = {rpmFile("kwin", "6.7.91-1.hl39.fc44", "v2-kwin"),
                                     rpmFile("kwin-common", "6.7.91-1.hl39.fc44", "v2-common"),
                                     rpmFile("kwin-libs", "6.7.91-1.hl39.fc44", "v2-libs")};
        out = installScript(QStringList{"install"} + rebuilt, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QCOMPARE(read(m_state + "/actions"), "reinstall\n");
        QCOMPARE(installed("kwin"), "v2-kwin");
    }

    /* Qt base: all its packages installed from the build, or none */
    void installScriptAllOrNone()
    {
        int status = -1;
        resetState();
        for (const char *name : {"qt6-qtbase", "qt6-qtbase-common", "qt6-qtbase-gui", "qt6-qtbase-mysql"}) {
            QVERIFY(write(m_state + "/installed/" + name, pkg(name, "6.11.2-3.fc44", "distro")));
        }
        QVERIFY(write(m_state + "/installed/qt6-qtbase-debuginfo", pkg("qt6-qtbase-debuginfo", "6.11.2-3.fc44", "dbg")));
        QStringList files;
        for (const char *name : {"qt6-qtbase", "qt6-qtbase-common", "qt6-qtbase-gui", "qt6-qtbase-devel"}) {
            files << rpmFile(name, "6.11.2-3.q4.fc44", QString("q4-%1").arg(name));
        }
        QString out = installScript(QStringList{"install"} + files, &status);
        QCOMPARE(status, 1);
        QVERIFY2(out.contains("vitrine-kde: error: qt6-qtbase-mysql (x86_64) is installed and the build of "
                              "qt6-qtbase has none: Vitrine replaces all of qt6-qtbase's packages or none"),
                 qPrintable(out));
        QCOMPARE(installed("qt6-qtbase"), "distro");
        QVERIFY(!QFileInfo::exists(m_state + "/transactions"));

        /* a 32-bit one installed too, which the build has not */
        files << rpmFile("qt6-qtbase-mysql", "6.11.2-3.q4.fc44", "q4-qt6-qtbase-mysql");
        QVERIFY(write(m_state + "/installed/qt6-qtbase-gui.i686",
                      pkg("qt6-qtbase-gui", "6.11.2-3.fc44", "distro32", "fedora", "i686")));
        out = installScript(QStringList{"install"} + files, &status);
        QCOMPARE(status, 1);
        QVERIFY2(out.contains("qt6-qtbase-gui (i686) is installed"), qPrintable(out));
        QFile::remove(m_state + "/installed/qt6-qtbase-gui.i686");

        out = installScript(QStringList{"install"} + files, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY(out.contains("vitrine-kde: left out: qt6-qtbase-devel, not installed"));
        /* one transaction, every package of it */
        const QList<QByteArray> transactions = read(m_state + "/transactions").trimmed().split('\n');
        QCOMPARE(transactions.size(), 1);
        QCOMPARE(transactions[0].count(".rpm"), 4);
        for (const char *name : {"qt6-qtbase", "qt6-qtbase-common", "qt6-qtbase-gui", "qt6-qtbase-mysql"}) {
            QCOMPARE(installed(name), QString("q4-%1").arg(name));
            /* locked by its exact name */
            QCOMPARE(read(m_state + "/locks/" + name).trimmed(), "6.11.2-3.q4.fc44");
        }
        QCOMPARE(QDir(m_state + "/locks").entryList(QDir::Files).size(), 4);
        QVERIFY(out.contains("vitrine-kde: locked: qt6-qtbase qt6-qtbase-common qt6-qtbase-gui qt6-qtbase-mysql"));
        /* KWin's switches are KWin's */
        QVERIFY(!QFileInfo::exists(m_state + "/tmpfiles/vitrine-host-kwin.conf"));
        /* the debug information as it was */
        QCOMPARE(installed("qt6-qtbase-debuginfo"), "dbg");

        /* two packages at once: one transaction */
        resetState();
        QVERIFY(write(m_state + "/installed/kf6-ksvg", pkg("kf6-ksvg", "6.31.0-1.fc44", "distro")));
        QVERIFY(write(m_state + "/installed/kf6-kirigami", pkg("kf6-kirigami", "6.31.0-1.fc44", "distro")));
        out = installScript({"install", rpmFile("kf6-ksvg", "6.31.0-1.k2.fc44", "k2"),
                             rpmFile("kf6-kirigami", "6.31.0-1.f1.fc44", "f1")},
                            &status);
        QVERIFY2(status == 0, qPrintable(out));
        QCOMPARE(read(m_state + "/transactions").count('\n'), 1);
        /* f1 sorts below Fedora's own 1.fc44: a downgrade, and only the lock keeps it */
        QVERIFY(read(m_state + "/transactions").contains("--action=upgrade"));
        QVERIFY(read(m_state + "/transactions").contains("--action=downgrade"));
        QCOMPARE(read(m_state + "/locks/kf6-kirigami").trimmed(), "6.31.0-1.f1.fc44");
    }

    void installScriptOverAnother()
    {
        int status = -1;
        resetState();
        /* the research's hm4, locked: above hl39 for rpm, so a downgrade */
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            QVERIFY(write(m_state + "/installed/" + name, pkg(name, "6.7.91-1.hm4.fc44", "hm4", "@commandline")));
            QVERIFY(write(m_state + "/locks/" + name, "6.7.91-1.hm4.fc44"));
        }
        const QStringList files = {rpmFile("kwin", "6.7.91-1.hl39.fc44", "v-kwin"),
                                   rpmFile("kwin-common", "6.7.91-1.hl39.fc44", "v-common"),
                                   rpmFile("kwin-libs", "6.7.91-1.hl39.fc44", "v-libs")};
        QVERIFY(write(m_state + "/fail", ""));
        QString out = installScript(QStringList{"install"} + files, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("vitrine-kde: error: dnf could not install them"));
        /* as it was: hm4, locked again */
        QCOMPARE(installed("kwin"), "hm4");
        QCOMPARE(read(m_state + "/locks/kwin").trimmed(), "6.7.91-1.hm4.fc44");
        QCOMPARE(read(m_state + "/locks/kwin-libs").trimmed(), "6.7.91-1.hm4.fc44");

        QFile::remove(m_state + "/fail");
        out = installScript(QStringList{"install"} + files, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QCOMPARE(read(m_state + "/actions"), "downgrade\n");
        QCOMPARE(installed("kwin"), "v-kwin");
        QCOMPARE(read(m_state + "/locks/kwin").trimmed(), "6.7.91-1.hl39.fc44");

        /* without dnf do: one command per action */
        resetState();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            QVERIFY(write(m_state + "/installed/" + name, pkg(name, "6.7.91-1.fc44", "distro")));
        }
        QVERIFY(write(m_state + "/no-do", ""));
        const QStringList again = {rpmFile("kwin", "6.7.91-1.hl39.fc44", "v-kwin"),
                                   rpmFile("kwin-common", "6.7.91-1.hl39.fc44", "v-common"),
                                   rpmFile("kwin-libs", "6.7.91-1.hl39.fc44", "v-libs")};
        out = installScript(QStringList{"install"} + again, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QCOMPARE(read(m_state + "/actions"), "upgrade\n");
    }

    void installScriptRefuses()
    {
        int status = -1;
        resetState();
        QVERIFY(write(m_state + "/installed/kwin", pkg("kwin", "6.7.91-1.fc44", "distro")));
        const QString file = rpmFile("kwin", "6.7.91-1.hl39.fc44", "v-kwin");
        /* not root */
        QString out = installScript({"install", file}, &status, false);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("run it as root"));
        /* a file changed since it was built */
        out = installScript({"install", file.section('=', 0, 0) + '=' + QString(64, 'a')}, &status);
        QCOMPARE(status, 1);
        QVERIFY2(out.contains("its sha256 differs"), qPrintable(out));
        QCOMPARE(installed("kwin"), "distro");
        out = installScript({"install", "/etc/passwd"}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("FILE.rpm=SHA256"));
        /* a package of another source than components.conf's */
        QVERIFY(write(m_state + "/installed/mesa-libGL", pkg("mesa-libGL", "26.2.5-1.fc44", "distro")));
        out = installScript({"install", rpmFile("mesa-libGL", "26.2.5-1.xe.fc44", "xe")}, &status);
        QCOMPARE(status, 1);
        QVERIFY2(out.contains("is not a package of kwin kf6-ksvg kf6-kirigami qt6-qtbase"), qPrintable(out));
        QCOMPARE(installed("mesa-libGL"), "distro");
        /* KWin not installed: nothing to replace */
        resetState();
        out = installScript({"install", rpmFile("kwin", "6.7.91-1.hl39.fc44", "v-kwin")}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("nothing to replace"));
        QVERIFY(!QFileInfo::exists(m_state + "/installed/kwin"));
    }

    /* install.sh's revert: Vitrine's packages only, the repositories' back, the locks gone */
    void revertScript()
    {
        int status = -1;
        resetState();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            QVERIFY(write(m_state + "/installed/" + name,
                          pkg(name, "6.7.91-1.hl39.fc44", hex(QString("v-%1").arg(name)), "@commandline")));
            QVERIFY(write(m_state + "/locks/" + name, "6.7.91-1.hl39.fc44"));
            QVERIFY(write(m_state + "/repo/" + name, pkg(name, "6.7.91-1.fc44", "distro")));
        }
        QVERIFY(write(m_state + "/tmpfiles/vitrine-host-kwin.conf", "f %t/kwin-animation-start-step 0644 - - - 1\n"));
        /* another's: left alone */
        QString out = installScript({"revert", "kwin=" + hex("hm4")}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("leaves it alone"));
        QCOMPARE(installed("kwin"), hex("v-kwin"));
        QVERIFY(QFileInfo::exists(m_state + "/locks/kwin"));
        QVERIFY(QFileInfo::exists(m_state + "/tmpfiles/vitrine-host-kwin.conf"));
        /* not a package and a digest, or not one of these packages */
        out = installScript({"revert", hex("v-kwin")}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("PACKAGE=SHA256"));
        out = installScript({"revert", "mesa=" + hex("v-kwin")}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("mesa is not a package of"));

        /* the repositories out of reach: locked again */
        QVERIFY(write(m_state + "/fail", ""));
        out = installScript({"revert", "kwin=" + hex("other"), "kwin=" + hex("v-kwin")}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("could not install the distribution's packages"));
        QCOMPARE(read(m_state + "/locks/kwin").trimmed(), "6.7.91-1.hl39.fc44");

        QFile::remove(m_state + "/fail");
        /* debug information an install left alone: not reverted either */
        QVERIFY(write(m_state + "/installed/kwin-debuginfo", pkg("kwin-debuginfo", "6.7.91-1.fc44", "dbg")));
        QFile::remove(m_state + "/calls");
        out = installScript({"revert", "kwin=" + hex("v-kwin")}, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY2(read(m_state + "/calls").contains("distro-sync kwin kwin-common kwin-libs\n"),
                 read(m_state + "/calls").constData());
        QCOMPARE(installed("kwin"), "distro");
        QCOMPARE(installed("kwin-libs"), "distro");
        QVERIFY(!QFileInfo::exists(m_state + "/locks/kwin"));
        QVERIFY(out.contains("vitrine-kde: installed: kwin-6.7.91-1.fc44.x86_64"));
        /* the switches with it */
        QVERIFY(out.contains("vitrine-kde: step: removing the set's run-time switches"));
        QVERIFY(!QFileInfo::exists(m_state + "/tmpfiles/vitrine-host-kwin.conf"));
    }

    /* Several packages reverted in one run, one transaction; one the repositories have no other of stays locked */
    void revertScriptSeveral()
    {
        int status = -1;
        resetState();
        for (const char *name : {"qt6-qtbase", "qt6-qtbase-gui"}) {
            QVERIFY(write(m_state + "/installed/" + name,
                          pkg(name, "6.11.2-3.q4.fc44", hex(QString("q4-%1").arg(name)), "@commandline")));
            QVERIFY(write(m_state + "/locks/" + name, "6.11.2-3.q4.fc44"));
            QVERIFY(write(m_state + "/repo/" + name, pkg(name, "6.11.2-3.fc44", "distro")));
        }
        QVERIFY(write(m_state + "/installed/kf6-kirigami",
                      pkg("kf6-kirigami", "6.31.0-1.f1.fc44", hex("f1"), "@commandline")));
        QVERIFY(write(m_state + "/locks/kf6-kirigami", "6.31.0-1.f1.fc44"));
        QVERIFY(write(m_state + "/repo/kf6-kirigami", pkg("kf6-kirigami", "6.31.0-1.fc44", "distro")));
        QString out = installScript({"revert", "qt6-qtbase=" + hex("q4-qt6-qtbase"), "kf6-kirigami=" + hex("f1")},
                                    &status);
        QVERIFY2(status == 0, qPrintable(out));
        QCOMPARE(read(m_state + "/transactions"), "distro-sync qt6-qtbase qt6-qtbase-gui kf6-kirigami\n");
        QCOMPARE(installed("qt6-qtbase-gui"), "distro");
        QCOMPARE(installed("kf6-kirigami"), "distro");
        QCOMPARE(QDir(m_state + "/locks").entryList(QDir::Files), QStringList());
        QVERIFY(!out.contains("switches"));

        /* the repositories have no other Qt base: it stays, locked again; Kirigami goes */
        resetState();
        for (const char *name : {"qt6-qtbase", "qt6-qtbase-gui"}) {
            QVERIFY(write(m_state + "/installed/" + name,
                          pkg(name, "6.11.2-3.q4.fc44", hex(QString("q4-%1").arg(name)), "@commandline")));
            QVERIFY(write(m_state + "/locks/" + name, "6.11.2-3.q4.fc44"));
        }
        QVERIFY(write(m_state + "/installed/kf6-kirigami",
                      pkg("kf6-kirigami", "6.31.0-1.f1.fc44", hex("f1"), "@commandline")));
        QVERIFY(write(m_state + "/locks/kf6-kirigami", "6.31.0-1.f1.fc44"));
        QVERIFY(write(m_state + "/repo/kf6-kirigami", pkg("kf6-kirigami", "6.31.0-1.fc44", "distro")));
        out = installScript({"revert", "qt6-qtbase=" + hex("q4-qt6-qtbase"), "kf6-kirigami=" + hex("f1")},
                            &status);
        QCOMPARE(status, 1);
        QVERIFY2(out.contains("the repositories have no other qt6-qtbase"), qPrintable(out));
        QCOMPARE(read(m_state + "/locks/qt6-qtbase-gui").trimmed(), "6.11.2-3.q4.fc44");
        QVERIFY(!QFileInfo::exists(m_state + "/locks/kf6-kirigami"));
        QCOMPARE(installed("kf6-kirigami"), "distro");
    }

    /* The monitor: read with the stand-ins; pkexec's answers */
    void monitorInstalls()
    {
        resetState();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            QVERIFY(write(m_state + "/installed/" + name, pkg(name, "6.7.91-1.fc44", "distro", kCopr)));
        }
        QVERIFY(write(m_state + "/installed/plasma-workspace", pkg("plasma-workspace", "6.7.91-1.fc44", "pw", kCopr)));
        QVERIFY(write(m_state + "/installed/qt6-qtbase", pkg("qt6-qtbase", "6.11.2-3.fc44", "qt")));
        QVERIFY(write(m_tmp.filePath("os-release"), "ID=fedora\nVERSION_ID=44\n"));
        const QString data = m_tmp.filePath("monitor-data");
        const QString out = data + "/host-kwin";
        Build b = build();
        b.dir = out + "/6.7.91-1.hl39.fc44";
        QByteArray conf = "evr=6.7.91-1.hl39.fc44\nversion=6.7.91\nset=hl39\ntag=hl39\nplasma=beta\n"
                          "fedora=44\nkey=k\nbuilt=2026-10-14T20:00:00Z\n";
        for (const Build::Rpm &rpm : b.rpms) {
            conf += "rpm=" + rpm.name.toUtf8() + " x86_64 " + rpm.digest.toUtf8() + ' ' + rpm.file.toUtf8() + '\n';
            QVERIFY(write(b.dir + '/' + rpm.file, rpm.name.toUtf8()));
        }
        QVERIFY(write(b.dir + "/build.conf", conf));
        /* pkexec as the stand-in: its arguments, then what $PKEXEC_EXIT says */
        QVERIFY(write(m_bin + "/pkexec",
                      "#!/bin/bash\necho \"$*\" > \"$STATE/pkexec\"\n"
                      "case ${PKEXEC_EXIT:-0} in 0) echo 'vitrine-kde: done' ;; 1) echo 'vitrine-kde: error: no room left' ;; esac\n"
                      "exit ${PKEXEC_EXIT:-0}\n"));
        QFile::setPermissions(m_bin + "/pkexec", QFile::ReadOwner | QFile::ExeOwner);
        qputenv("PATH", m_bin.toUtf8() + ":/usr/bin:/bin");
        qputenv("STATE", m_state.toUtf8());

        HostKdeMonitor monitor;
        monitor.setSourceDir(hostKdeDir());
        monitor.setDataDir(data);
        monitor.setOsRelease(m_tmp.filePath("os-release"));
        monitor.setProc(m_tmp.filePath("noproc"));
        QCOMPARE(monitor.outDir("host-kwin"), out);
        QSignalSpy changed(&monitor, &HostKdeMonitor::changed);
        monitor.refresh();
        QVERIFY(changed.wait(10000));
        QVERIFY(monitor.isRead());
        QCOMPARE(monitor.targets().size(), 4);
        Status s = monitor.status("host-kwin");
        QCOMPARE(s.offer, Offer::Offered);
        /* the recipe's set; the build is of an older one */
        QCOMPARE(s.series, "hl39y");
        QVERIFY(s.build.isValid());
        /* the recipe's key differs from "k": still installable */
        QVERIFY(s.recipeChanged);
        QVERIFY(s.canInstall());
        /* Qt base installed, KSvg and Kirigami not */
        QCOMPARE(monitor.status("host-qtbase").offer, Offer::Offered);
        QCOMPARE(monitor.status("host-qtbase").series, "q4");
        QCOMPARE(monitor.status("host-ksvg").offer, Offer::Hidden);
        QCOMPARE(monitor.statuses().size(), 4);

        QSignalSpy finished(&monitor, &HostKdeMonitor::finished);
        qputenv("PKEXEC_EXIT", "126");
        monitor.install("host-kwin", s.build);
        QVERIFY(monitor.isBusy());
        QCOMPARE(monitor.busyWith(), QStringList{"host-kwin"});
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(1).toBool(), true);
        QVERIFY(!QFileInfo::exists(Record::path(out)));
        QVERIFY(read(m_state + "/pkexec").startsWith("/usr/bin/bash " + hostKdeDir().toUtf8() +
                                                     "/install.sh install " + b.dir.toUtf8()));

        qputenv("PKEXEC_EXIT", "1");
        monitor.install("host-kwin", s.build);
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), "no room left");
        QCOMPARE(finished.last().value(1).toBool(), false);

        qputenv("PKEXEC_EXIT", "0");
        monitor.install("host-kwin", s.build);
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), QString());
        QVERIFY(monitor.log().contains("vitrine-kde: done"));
        /* the record: Vitrine's now, by its digests */
        QCOMPARE(Record::read(Record::path(out)).digests.value("kwin"), "6.7.91-1.hl39.fc44-kwin");
        QCOMPARE(Record::read(Record::path(out)).tag, "hl39");
        QVERIFY(changed.wait(10000));

        monitor.revert({"host-kwin"});
        QVERIFY(finished.wait(10000));
        QVERIFY(read(m_state + "/pkexec").contains("install.sh revert kwin=6.7.91-1.hl39.fc44-kwin"));
        QVERIFY(!QFileInfo::exists(Record::path(out)));
        qunsetenv("PKEXEC_EXIT");
        QFile::remove(m_bin + "/pkexec");
    }

    /* The builder: build.sh's stand-in, a wait for memory before a heavy one, a stop */
    void builderRuns()
    {
        const QString src = m_tmp.filePath("src");
        QVERIFY(write(src + "/build.sh",
                      "#!/bin/bash\necho \"$*\" > \"$(dirname \"$0\")/args\"\n"
                      "echo 'vitrine-build: phase: preparing the container'\n"
                      "echo 'Executing(%build): /bin/sh -e /var/tmp/rpm-tmp.b'\n"
                      "echo '[10/40] Building CXX object a.o'\n"
                      "[ -n \"${GATE:-}\" ] && while [ ! -e \"$GATE\" ]; do sleep 0.05; done\n"
                      "[ -n \"${FAIL:-}\" ] && { echo 'vitrine-build: error: no host set for kwin 6.7.5' >&2; exit 1; }\n"
                      "echo 'vitrine-build: built: /out/6.7.91-1.hl39.fc44'\n"));
        HostKdeBuilder builder;
        builder.setSourceDir(src);
        builder.setDataDir(m_tmp.filePath("bdata"));
        builder.setWorkDir(m_tmp.filePath("bwork"));
        builder.setJobs(3);
        QVERIFY(write(m_tmp.filePath("meminfo"), "MemTotal: 33554432 kB\nMemAvailable: 33554432 kB\n"));
        builder.setMeminfo(m_tmp.filePath("meminfo"));
        QSignalSpy finished(&builder, &HostKdeBuilder::finished);
        QSignalSpy progress(&builder, &HostKdeBuilder::progress);
        builder.start(kwin(), "beta");
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), QString());
        QCOMPARE(builder.built(), "/out/6.7.91-1.hl39.fc44");
        QCOMPARE(read(src + "/args").trimmed(),
                 QString("--package kwin --out %1 --work %2 -j 3 --plasma beta")
                     .arg(m_tmp.filePath("bdata/host-kwin"), m_tmp.filePath("bwork")).toUtf8());
        QVERIFY(std::any_of(progress.begin(), progress.end(), [](const QList<QVariant> &a) {
            return a.value(0).toInt() == 10 && a.value(1).toInt() == 40;
        }));

        qputenv("FAIL", "1");
        builder.start(kwin(), "fedora");
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), "No host set for kwin 6.7.5");
        qunsetenv("FAIL");

        /* short of memory: a heavy one waits, then a stop */
        QVERIFY(write(m_tmp.filePath("meminfo"), "MemTotal: 33554432 kB\nMemAvailable: 4194304 kB\n"));
        builder.setMemoryPoll(50);
        builder.start(qtbase(), "beta");
        QVERIFY(builder.isWaiting());
        QCOMPARE(builder.availableMiB(), 4096);
        QVERIFY(builder.neededMiB() >= GuestToolsBuilder::memoryCapMiB());
        QVERIFY(builder.log().contains("Qt base's build takes up to"));
        builder.cancel();
        QVERIFY(builder.wasStopped());
        QVERIFY(!builder.isRunning());
        /* a small one does not wait */
        builder.start(ksvg(), "fedora");
        QVERIFY(!builder.isWaiting());
        QVERIFY(finished.wait(10000));
        QVERIFY(read(src + "/args").startsWith("--package kf6-ksvg --out " + m_tmp.filePath("bdata/host-ksvg").toUtf8()));
        /* memory comes back: the heavy one starts by itself */
        builder.start(kwin(), "beta");
        QVERIFY(builder.isWaiting());
        QVERIFY(write(m_tmp.filePath("meminfo"), "MemTotal: 33554432 kB\nMemAvailable: 33554432 kB\n"));
        QVERIFY(finished.wait(10000));
        QVERIFY(!builder.wasStopped());

        /* a stop while it builds */
        qputenv("GATE", m_tmp.filePath("gate").toUtf8());
        builder.start(kwin(), "beta");
        QTRY_VERIFY(builder.phase() == "compiling");
        builder.cancel();
        QVERIFY(finished.wait(10000));
        QVERIFY(builder.wasStopped());
        QCOMPARE(builder.error(), QString());
        qunsetenv("GATE");
    }

    /*
     * A real system, when HOSTKDE_SYSTEM names a folder of rpm and dnf
     * that run on it (a test VM's, through ssh) and its os-release: what
     * HostKde makes of each package, against the builds of HOSTKDE_DATA
     * (<data>/vitrine).  Skipped otherwise.  HOSTKDE_EXPECT: the summaries
     * expected, "id=summary" separated by "|".
     */
    void aRealSystem()
    {
        const QString dir = qEnvironmentVariable("HOSTKDE_SYSTEM");
        const QString data = qEnvironmentVariable("HOSTKDE_DATA");
        if (dir.isEmpty()) {
            QSKIP("HOSTKDE_SYSTEM not set: no real system to read");
        }
        qputenv("PATH", dir.toUtf8() + ":/usr/bin:/bin");
        const System system = System::read(m_targets, dir + "/os-release", m_tmp.filePath("noproc"));
        QHash<QString, QString> versions;
        for (const Target &t : std::as_const(m_targets)) {
            const QString out = t.outDir(data);
            versions.insert(t.package, versionOf(t, system, digests(t, builds(out), Record::read(Record::path(out)))));
        }
        const Recipe r = Recipe::read(hostKdeDir(), system.fedora, versions);
        qInfo().noquote() << "fedora" << system.fedora << "plasma" << system.plasma() << "from"
                          << system.package("plasma-workspace").repo;
        qInfo().noquote() << "locks:" << system.locks;
        qInfo().noquote() << "newest:" << system.newest;
        QHash<QString, QString> expect;
        for (const QString &item : qEnvironmentVariable("HOSTKDE_EXPECT").split('|', Qt::SkipEmptyParts)) {
            expect.insert(item.section('=', 0, 0), item.section('=', 1));
        }
        for (const Target &t : std::as_const(m_targets)) {
            const QString out = t.outDir(data);
            const Status s = status(t, system, r, builds(out), Record::read(Record::path(out)));
            QStringList installed;
            for (const Package &p : system.of(t.package)) {
                installed << p.nevra() + " from " + p.repo;
            }
            QString install, revert;
            const bool canInstall = s.canInstall(&install);
            const bool canRevert = s.canRevert(&revert);
            qInfo().noquote() << t.id << "|" << s.summary() << "| version" << s.version << "set" << s.series
                              << "owner" << int(s.owner) << "offer" << int(s.offer) << "differs" << s.differs
                              << "outdated" << s.outdated << "held back" << s.heldBack;
            qInfo().noquote() << "   installed:" << installed.join(", ");
            qInfo().noquote() << "   canInstall" << canInstall << install << "| canRevert" << canRevert << revert;
            if (expect.contains(t.id)) {
                QCOMPARE(s.summary(), expect.value(t.id));
            }
        }
        QVERIFY(system.isPlasmaHost());
        QCOMPARE(system.fedora, "44");
    }

    /* In BuildManager's runs: a part each, and Update All only while Vitrine's is installed */
    void inTheBuilds()
    {
        const QString src = m_tmp.filePath("kdesrc");
        QVERIFY(QDir().mkpath(src));
        for (const char *name : {"install.sh", "components.conf"}) {
            QVERIFY(QFile::copy(hostKdeDir() + '/' + name, src + '/' + name));
        }
        QVERIFY(write(src + "/build.sh", "#!/bin/bash\necho \"$*\" >> \"$(dirname \"$0\")/builds\"\n"
                                         "echo 'vitrine-build: phase: building the packages'\n"
                                         "echo 'vitrine-build: built: /out/x'\n"));
        Builds::Places places;
        places.systemDir = src;
        for (const Target &t : std::as_const(m_targets)) {
            places.system.insert(t.id, status(t, host(), recipe(), {}, {}));
        }
        HostKdeBuilder builder;
        builder.setSourceDir(src);
        QVERIFY(write(m_tmp.filePath("meminfo2"), "MemTotal: 33554432 kB\nMemAvailable: 33554432 kB\n"));
        builder.setMeminfo(m_tmp.filePath("meminfo2"));
        BuildManager m;
        m.setPlaces(places);
        m.setBuilders(StackBuilder::instance(), GuestToolsBuilder::instance(), &builder);
        m.setHistoryFile(m_tmp.filePath("history.json"));
        m.setLogDir(m_tmp.filePath("logs"));
        QCOMPARE(m.components().size(), 4);
        QCOMPARE(m.status("host-kwin").state, Builds::Status::State::NotBuilt);
        QCOMPARE(m.systemStatus("host-qtbase").target.title, "Qt base");
        /* optional: not in Update All */
        QVERIFY(m.outdated().isEmpty());
        QCOMPARE(m.plan({"host-kwin"}), QStringList{"host-kwin"});

        QSignalSpy finished(&m, &BuildManager::runFinished);
        m.build({"host-kwin"});
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), QString());
        QCOMPARE(m.progress("host-kwin").state, BuildManager::State::Built);
        QVERIFY(m.log("host-kwin").contains("building the packages"));
        /* two at once: one run of build.sh each, in their order */
        QFile::remove(src + "/builds");
        m.build({"host-qtbase", "host-ksvg"});
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), QString());
        const QList<QByteArray> runs = read(src + "/builds").trimmed().split('\n');
        QCOMPARE(runs.size(), 2);
        QVERIFY(runs[0].startsWith("--package kf6-ksvg "));
        QVERIFY(runs[1].startsWith("--package qt6-qtbase "));
        QCOMPARE(m.progress("host-ksvg").state, BuildManager::State::Built);
        QCOMPARE(m.progress("host-qtbase").state, BuildManager::State::Built);

        /* Vitrine's installed: kept up to date with the rest */
        const Build b = build();
        places.system.insert("host-kwin", status(kwin(), withVitrines(host("6.8.0", "6.8.0-1.fc44"), b),
                                                 recipe("hl39-68", "h18"), {b},
                                                 Record::of(b, QDateTime::currentDateTimeUtc(), kwin().against)));
        m.setPlaces(places);
        QCOMPARE(m.status("host-kwin").state, Builds::Status::State::Outdated);
        QCOMPARE(m.outdated(), QStringList{"host-kwin"});
        QCOMPARE(Builds::summary(m.status("host-kwin").changes), "built for KWin 6.7.91");
        QCOMPARE(Builds::describe(m.status("host-kwin").changes.first()),
                 "Built for KWin 6.7.91, and this computer has Plasma 6.8.0");
        /* no set for this Plasma: nothing to build */
        places.system.insert("host-kwin", status(kwin(), host("6.7.5", "6.7.5-1.fc44", "fedora"),
                                                 recipe(QString(), QString()), {}, {}));
        m.setPlaces(places);
        QCOMPARE(m.status("host-kwin").state, Builds::Status::State::NoSources);
        /* the packages' outputs: their locks are a build's from a terminal */
        places.systemOut.insert("host-ksvg", m_tmp.filePath("locked-out"));
        m.setPlaces(places);
        m.setCacheDirs({m_tmp.filePath("cache-a")});
        QVERIFY(QDir().mkpath(m_tmp.filePath("locked-out")));
        QVERIFY(write(m_tmp.filePath("locked-out/.lock"), ""));
        QProcess holder;
        /* -o: the lock is flock's own, not its command's, and goes with it */
        holder.start("flock", {"-o", m_tmp.filePath("locked-out/.lock"), "sleep", "5"});
        QVERIFY(holder.waitForStarted());
        QTest::qWait(300);
        QString error;
        QVERIFY(!m.clearCache(&error));
        QVERIFY(error.contains("terminal"));
        holder.kill();
        holder.waitForFinished();
        QVERIFY(m.clearCache(&error));
    }
};

QTEST_GUILESS_MAIN(TestHostKde)
#include "test_hostkde.moc"
