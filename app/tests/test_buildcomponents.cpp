// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include "core/buildcomponents.h"
#include "core/guesttoolsbuilder.h"
#include "core/stackbuilder.h"

using namespace Builds;

class TestBuildComponents : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

    static bool write(const QString &path, const QByteArray &data, bool executable = false)
    {
        QFile f(path);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
            !f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
            return false;
        }
        f.close();
        return !executable || f.setPermissions(f.permissions() | QFileDevice::ExeUser);
    }

    static QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    static QString run(const QStringList &args, int *status = nullptr)
    {
        QProcess p;
        p.start("bash", args);
        p.waitForFinished(60000);
        if (status) {
            *status = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        }
        return QString::fromUtf8(p.readAllStandardOutput());
    }

    static Places real()
    {
        Places places;
        places.hostDir = StackBuilder::hostDir();
        places.guestDir = GuestToolsBuilder::guestDir();
        return places;
    }

    /* A host/ of the real build.sh and components.conf, these pins, these patches */
    QString hostFixture(const QString &name, const QByteArray &versions,
                        const QHash<QString, QByteArray> &patches)
    {
        const QString dir = m_tmp.filePath(name);
        const QString host = StackBuilder::hostDir();
        if (!QDir().mkpath(dir) || !QFile::copy(host + "/build.sh", dir + "/build.sh") ||
            !QFile::copy(host + "/components.conf", dir + "/components.conf") ||
            !write(dir + "/versions.conf", versions)) {
            return {};
        }
        for (auto it = patches.begin(); it != patches.end(); ++it) {
            if (!write(dir + "/patches/" + it.key(), it.value())) {
                return {};
            }
        }
        return dir;
    }

    /* @stack's `current` as build.sh leaves it for @host's inputs now: a
       QEMU and the virglrenderer it is built against, with their inputs */
    static bool builtStack(const QString &host, const QString &stack)
    {
        Places places;
        places.hostDir = host;
        const QList<Component> all = load(places);
        const Component virgl = find(all, "virglrenderer");
        const Component qemu = find(all, "qemu");
        const QString vkey = key(virgl, all);
        const QString stamp = key(qemu, all);
        const QString library = stack + "/virglrenderer/" + vkey.left(16);
        const QString prefix = stack + '/' + stamp.left(16);
        QFile::remove(stack + "/current");
        return write(library + "/share/vitrine/virglrenderer.conf",
                     "KEY=" + vkey.toUtf8() + "\nBUILT=2026-10-12T08:00:00Z\nVERSION=1.3.0\n") &&
               write(library + "/share/vitrine/virglrenderer.inputs", inputLines(virgl, all)) &&
               write(prefix + "/share/vitrine/stack.conf",
                     "STAMP=" + stamp.toUtf8() +
                         "\nBUILT=2026-10-12T09:00:00Z\nQEMU_VERSION=11.1.50\nVIRGL_KEY=" +
                         vkey.toUtf8() + "\nVIRGL_PREFIX=" + library.toUtf8() + '\n') &&
               write(prefix + "/share/vitrine/qemu.inputs", inputLines(qemu, all)) &&
               QFile::link(stamp.left(16), stack + "/current");
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qunsetenv("VITRINE_HOST_DIR");
        qunsetenv("VITRINE_GUEST_DIR");
        QVERIFY(!StackBuilder::hostDir().isEmpty());
        QVERIFY(!GuestToolsBuilder::guestDir().isEmpty());
    }

    void parsesConf()
    {
        const auto sections = parseConf("# a comment\n"
                                        "NAME=outside any section\n"
                                        "[one]\n"
                                        "title=One = 1\n"
                                        "  origin.fork = The fork  \n"
                                        "\n"
                                        "   # indented comment\n"
                                        "[ two ]\n"
                                        "deps=one\n"
                                        "not a line\n");
        QCOMPARE(sections.size(), 2);
        QCOMPARE(sections[0].first, "one");
        QCOMPARE(sections[0].second.value("title"), "One = 1");
        QCOMPARE(sections[0].second.value("origin.fork"), "The fork");
        QVERIFY(!sections[0].second.contains("NAME"));
        QCOMPARE(sections[1].first, "two");
        QCOMPARE(sections[1].second.value("deps"), "one");
        QCOMPARE(sections[1].second.size(), 1);
    }

    /* The real components.conf files: what the window shows */
    void loadsTheRealOnes()
    {
        const QList<Component> all = load(real());
        QStringList ids;
        for (const Component &c : all) {
            ids << c.id;
        }
        QCOMPARE(ids, QStringList({"virglrenderer", "qemu", "tools", "mesa", "kwin", "medium"}));
        QCOMPARE(find(all, "qemu").kind, Kind::Host);
        QCOMPARE(find(all, "qemu").title, "QEMU");
        QCOMPARE(find(all, "qemu").deps, QStringList({"virglrenderer"}));
        QCOMPARE(find(all, "mesa").kind, Kind::Package);
        QVERIFY(find(all, "mesa").heavy);
        QVERIFY(!find(all, "tools").heavy);
        QCOMPARE(find(all, "medium").kind, Kind::Medium);
        QCOMPARE(find(all, "medium").deps, QStringList({"tools", "mesa", "kwin"}));
        QVERIFY(find(all, "qemu").minutes > 0);
        QVERIFY(!find(all, "nothing").isValid());

        /* none of a folder that is not there; a package without its spec neither */
        Places places;
        places.guestDir = m_tmp.filePath("specless");
        QVERIFY(write(places.guestDir + "/mesa/mesa.spec", "Name: mesa\n"));
        ids.clear();
        for (const Component &c : load(places)) {
            ids << c.id;
        }
        /* no components.conf: what an older Vitrine's guest/ builds */
        QCOMPARE(ids, QStringList({"mesa", "medium"}));
        QVERIFY(load(Places()).isEmpty());
    }

    /* The keys and their lines are the scripts' */
    void keysAreTheScripts()
    {
        const Places places = real();
        const QList<Component> all = load(places);
        int status = -1;

        const QString keys = run({places.hostDir + "/build.sh", "--print-keys"}, &status);
        QCOMPARE(status, 0);
        QCOMPARE(keys, QString("virglrenderer %1\nqemu %2\n")
                           .arg(key(find(all, "virglrenderer"), all), key(find(all, "qemu"), all)));
        for (const char *id : {"virglrenderer", "qemu"}) {
            QCOMPARE(run({places.hostDir + "/build.sh", "--print-inputs", id}).toUtf8(),
                     inputLines(find(all, id), all));
        }
        /* the stamp, which the app compares with the build's */
        QCOMPARE(key(find(all, "qemu"), all), StackBuilder::inputStamp(places.hostDir));

        const QString inputs = run({places.guestDir + "/build-rpms.sh", "--print-inputs"}, &status);
        QCOMPARE(status, 0);
        for (const char *id : {"tools", "mesa", "kwin"}) {
            QVERIFY2(inputs.contains(QString("%1 %2\n").arg(id, key(find(all, id), all))),
                     qPrintable(inputs));
            QCOMPARE(key(find(all, id), all), GuestToolsBuilder::inputsHash(places.guestDir, id));
        }
        /* the medium's: its packages' keys and its script */
        const QByteArray medium = inputLines(find(all, "medium"), all);
        QVERIFY(medium.startsWith("dep tools " + key(find(all, "tools"), all).toLatin1() + '\n'));
        QVERIFY(medium.endsWith("  build-medium.sh\n"));
    }

    /* A series file names the patches in its order; the folders otherwise */
    void patchesInOrder()
    {
        const QString dir = m_tmp.filePath("order");
        QVERIFY(write(dir + "/patches/qemu/series", "# the series\n0002-b.patch\n\n 0001-a.patch # first\n"));
        QVERIFY(write(dir + "/patches/qemu/0001-a.patch", "a\n"));
        QVERIFY(write(dir + "/patches/qemu/0002-b.patch", "b\n"));
        QVERIFY(write(dir + "/patches/qemu/0003-unlisted.patch", "c\n"));
        QCOMPARE(patchPaths(dir, "patches/qemu"),
                 QStringList({"patches/qemu/0002-b.patch", "patches/qemu/0001-a.patch"}));
        QVERIFY(QFile::remove(dir + "/patches/qemu/series"));
        QCOMPARE(patchPaths(dir, "patches/qemu"),
                 QStringList({"patches/qemu/0001-a.patch", "patches/qemu/0002-b.patch",
                              "patches/qemu/0003-unlisted.patch"}));
        QVERIFY(patchPaths(dir, "patches/none").isEmpty());
        QVERIFY(patchPaths({}, "patches/qemu").isEmpty());

        /* an order file of a guest folder: the names in the order it names them */
        Component c;
        c.id = "tools";
        c.kind = Kind::Package;
        c.dir = dir;
        c.values = {{"patches", "dkms/patches"}, {"order", "dkms/prepare.sh"}, {"origin", "R"}};
        QVERIFY(write(dir + "/dkms/prepare.sh", "apply patches/z.patch\napply patches/a.patch\n"));
        for (const char *name : {"a.patch", "m.patch", "z.patch"}) {
            QVERIFY(write(dir + "/dkms/patches/" + name, "x\n"));
        }
        QStringList names;
        for (const Patch &p : patches(c)) {
            names << p.name();
            QCOMPARE(p.origin, "R");
        }
        QCOMPARE(names, QStringList({"z.patch", "a.patch", "m.patch"}));
    }

    /* The real patches: by origin, in their order, each with its subject */
    void realPatches()
    {
        const QList<Component> all = load(real());

        const Component qemu = find(all, "qemu");
        const QList<Patch> qemuPatches = patches(qemu);
        QCOMPARE(qemuPatches.size(), StackBuilder::patches(qemu.dir, "qemu").size());
        QCOMPARE(qemuPatches.first().origin, "qemu-gui fork");
        QCOMPARE(qemuPatches.last().origin, "Vitrine");
        QStringList origins;
        for (const Patch &p : qemuPatches) {
            if (!origins.contains(p.origin)) {
                origins << p.origin;
            }
        }
        QCOMPARE(origins, QStringList({"qemu-gui fork", "Research", "Vitrine"}));
        /* a git format-patch Subject, its [PATCH] tag gone */
        QCOMPARE(subject(qemu, qemuPatches.first()),
                 "ui/sdl2: follow the host refresh rate, tolerate grab border");
        QVERIFY(text(qemu, qemuPatches.first()).startsWith("From 0000000000000000000000000000000000000000"));

        /* a plain diff's description: its first line */
        const Component virgl = find(all, "virglrenderer");
        const QList<Patch> virglPatches = patches(virgl);
        QCOMPARE(virglPatches.size(), 4);
        QCOMPARE(virglPatches[0].origin, "Research");
        QCOMPARE(virglPatches[3].origin, "Vitrine");
        QCOMPARE(subject(virgl, virglPatches[1]),
                 "amdgpu: force write-combining for host-visible blob mappings");
        QCOMPARE(subject(virgl, virglPatches[3]), "xe: sync-only execs (no batch buffer) pass again");

        /* a spec's Patch lines, a diff without a description: the comment over its line */
        const Component kwin = find(all, "kwin");
        const QList<Patch> kwinPatches = patches(kwin);
        QVERIFY(kwinPatches.size() >= 10);
        QCOMPARE(kwinPatches.first().path, "kwin/kwin-vm-frame-timing.patch");
        QCOMPARE(kwinPatches.first().number, "100");
        QCOMPARE(subject(kwin, kwinPatches.first()),
                 "In virtual machines: CPU-only render timing and a 2.5 ms commit safety margin");
        for (const Patch &p : kwinPatches) {
            QVERIFY2(QFileInfo::exists(kwin.dir + '/' + p.path), qPrintable(p.path));
        }
        const QList<Patch> mesaPatches = patches(find(all, "mesa"));
        QCOMPARE(mesaPatches.size(), 4);
        QCOMPARE(subject(find(all, "mesa"), mesaPatches[2]),
                 "iris: keep buffers in the reuse cache longer under virtio-gpu");

        /* the driver's patches, in the order prepare.sh applies them */
        const QList<Patch> toolsPatches = patches(find(all, "tools"));
        QCOMPARE(toolsPatches.size(), 12);
        QCOMPARE(toolsPatches.first().name(), "linux-drm-virtio-attach-imported-dmabufs.patch");
        QCOMPARE(toolsPatches.last().name(), "linux-drm-virtio-tiled-scanout.patch");
        QVERIFY(patches(find(all, "medium")).isEmpty());
    }

    void sources()
    {
        const QList<Component> all = load(real());

        const Source qemu = source(find(all, "qemu"));
        QCOMPARE(qemu.url, "https://gitlab.com/qemu-project/qemu.git");
        QCOMPARE(qemu.revision.size(), 40);
        const Source mesa = source(find(all, "mesa"));
        QCOMPARE(mesa.archive, "mesa-" + GuestToolsBuilder::version(find(all, "mesa").dir, "mesa") +
                                   ".tar.xz");
        QCOMPARE(mesa.archiveHost, "archive.mesa3d.org");
        QVERIFY(mesa.revision.startsWith(GuestToolsBuilder::version(find(all, "mesa").dir, "mesa") + '-'));
        QCOMPARE(mesa.url, "https://mesa3d.org");
        const Source kwin = source(find(all, "kwin"));
        QVERIFY(kwin.archive.startsWith("kwin-") && kwin.archive.endsWith(".tar.xz"));
        QCOMPARE(kwin.archiveHost, "download.kde.org");
        const Source tools = source(find(all, "tools"));
        QVERIFY(tools.archiveHost.isEmpty());
        QVERIFY(tools.revision.startsWith("0.1.0-"));
    }

    /* What is built of the host's components, and why it is out of date */
    void hostStatus()
    {
        const QByteArray pins = "QEMU_URL=https://q/qemu.git\nQEMU_COMMIT=%1\n"
                                "VIRGL_URL=https://v/virgl.git\nVIRGL_COMMIT=%2\n";
        const QString host =
            hostFixture("status-host", QString(pins).arg("1111", "2222").toUtf8(),
                        {{"qemu/fork/0001-a.patch", "a\n"},
                         {"qemu/research/0001-b.patch", "b\n"},
                         {"virglrenderer/0001-v.patch", "v\n"}});
        QVERIFY(!host.isEmpty());
        Places places;
        places.hostDir = host;
        places.stackDir = m_tmp.filePath("status-stack");
        const auto statusOf = [&places](const char *id) {
            const QList<Component> all = load(places);
            return status(find(all, id), all, places);
        };

        QCOMPARE(statusOf("qemu").state, Status::State::NotBuilt);
        QCOMPARE(statusOf("virglrenderer").state, Status::State::NotBuilt);
        QVERIFY(builtStack(host, places.stackDir));
        Status qemu = statusOf("qemu");
        QCOMPARE(qemu.state, Status::State::UpToDate);
        QCOMPARE(qemu.version, "11.1.50");
        QCOMPARE(qemu.built, QDateTime(QDate(2026, 10, 12), QTime(9, 0), QTimeZone::UTC));
        QCOMPARE(QFileInfo(qemu.output).fileName().size(), 16);
        Status virgl = statusOf("virglrenderer");
        QCOMPARE(virgl.state, Status::State::UpToDate);
        QCOMPARE(virgl.version, "1.3.0");

        /* a QEMU patch changed: QEMU, not virglrenderer */
        QVERIFY(write(host + "/patches/qemu/research/0001-b.patch", "b, rebased\n"));
        qemu = statusOf("qemu");
        QCOMPARE(qemu.state, Status::State::Outdated);
        QCOMPARE(qemu.changes.size(), 1);
        QCOMPARE(qemu.changes[0].what, Change::What::Patch);
        QCOMPARE(qemu.changes[0].how, Change::How::Changed);
        QCOMPARE(describe(qemu.changes[0]), "Patch research/0001-b.patch changed");
        QCOMPARE(summary(qemu.changes), "1 patch changed");
        QCOMPARE(statusOf("virglrenderer").state, Status::State::UpToDate);

        /* one more, one less */
        QVERIFY(write(host + "/patches/qemu/vitrine/0001-c.patch", "c\n"));
        QVERIFY(QFile::remove(host + "/patches/qemu/fork/0001-a.patch"));
        qemu = statusOf("qemu");
        QStringList said;
        for (const Change &change : qemu.changes) {
            said << describe(change);
        }
        QCOMPARE(said, QStringList({"Patch research/0001-b.patch changed",
                                    "New patch vitrine/0001-c.patch",
                                    "Patch fork/0001-a.patch removed"}));
        QCOMPARE(summary(qemu.changes), "1 patch changed, 1 new patch, 1 patch removed");

        /* virglrenderer's commit moved: it, and QEMU built against it */
        QVERIFY(builtStack(host, places.stackDir));
        QVERIFY(write(host + "/versions.conf", QString(pins).arg("1111", "3333").toUtf8()));
        virgl = statusOf("virglrenderer");
        QCOMPARE(virgl.state, Status::State::Outdated);
        QCOMPARE(virgl.changes.size(), 1);
        QCOMPARE(virgl.changes[0].what, Change::What::Source);
        QCOMPARE(virgl.changes[0].was, "2222");
        QCOMPARE(virgl.changes[0].now, "3333");
        QCOMPARE(describe(virgl.changes[0]), "Pinned to another commit: 3333, 2222 before");
        qemu = statusOf("qemu");
        QCOMPARE(qemu.changes.size(), 1);
        QCOMPARE(qemu.changes[0].what, Change::What::Dependency);
        QCOMPARE(describe(qemu.changes[0]), "virglrenderer changed");
        QCOMPARE(summary(qemu.changes), "virglrenderer changed");
        /* the script changed: all of them */
        QVERIFY(builtStack(host, places.stackDir));
        QVERIFY(write(host + "/build.sh", read(host + "/build.sh") + "\n# changed\n"));
        QCOMPARE(summary(statusOf("virglrenderer").changes), "build script changed");
        QCOMPARE(summary(statusOf("qemu").changes), "build script changed, virglrenderer changed");

        /* a build of an older Vitrine, which kept no inputs */
        QVERIFY(QFile::remove(QFileInfo(places.stackDir + "/current").canonicalFilePath() +
                              "/share/vitrine/qemu.inputs"));
        qemu = statusOf("qemu");
        QCOMPARE(qemu.state, Status::State::Outdated);
        QCOMPARE(qemu.changes.size(), 1);
        QCOMPARE(qemu.changes[0].what, Change::What::Unknown);
        QCOMPARE(describe(qemu.changes[0]), "Built from other sources");

        /* no pins: nothing to build from */
        QVERIFY(write(host + "/versions.conf", "QEMU_URL=x\n"));
        QCOMPARE(statusOf("qemu").state, Status::State::NoSources);
    }

    /* A package: its recorded inputs; the medium: its packages' */
    void guestStatus()
    {
        const QString guest = m_tmp.filePath("status-guest");
        Places places;
        places.guestDir = guest;
        places.rpmsDir = m_tmp.filePath("status-rpms");
        places.mediumImage = m_tmp.filePath("status-media/vitrine-guest-tools-fc44.img");
        QVERIFY(write(guest + "/rpm-build-inside.sh", "#!/bin/bash\n"));
        QVERIFY(write(guest + "/build-medium.sh", "#!/bin/bash\n"));
        QVERIFY(write(guest + "/tools/vitrine-guest-tools.spec", "Name: vitrine-guest-tools\nVersion: 0.1.0\n"));
        QVERIFY(write(guest + "/tools/install", "one\n"));
        QVERIFY(write(guest + "/mesa/mesa.spec", "Name: mesa\nVersion: 26.2.3\n"));
        const auto statusOf = [&places](const char *id) {
            const QList<Component> all = load(places);
            return status(find(all, id), all, places);
        };

        QCOMPARE(statusOf("tools").state, Status::State::NotBuilt);
        const QByteArray lines = GuestToolsBuilder::inputLines(guest, "tools");
        QVERIFY(write(places.rpmsDir + "/tools/.inputs",
                      GuestToolsBuilder::inputsHash(guest, "tools").toUtf8() + '\n'));
        QVERIFY(write(places.rpmsDir + "/tools/.inputs.list", lines));
        QVERIFY(write(places.rpmsDir + "/tools/vitrine-guest-tools-0.1.0-15.fc44.noarch.rpm", "x"));
        Status tools = statusOf("tools");
        QCOMPARE(tools.state, Status::State::UpToDate);
        QCOMPARE(tools.version, "0.1.0");
        QCOMPARE(tools.output, places.rpmsDir + "/tools");

        QVERIFY(write(guest + "/tools/install", "two\n"));
        QVERIFY(write(guest + "/tools/agent", "new\n"));
        QVERIFY(write(guest + "/rpm-build-inside.sh", "#!/bin/bash\n# ccache\n"));
        tools = statusOf("tools");
        QCOMPARE(tools.state, Status::State::Outdated);
        QStringList said;
        for (const Change &change : tools.changes) {
            said << describe(change);
        }
        QCOMPARE(said, QStringList({"Its build script rpm-build-inside.sh changed",
                                    "New file tools/agent", "tools/install changed"}));
        QCOMPARE(summary(tools.changes), "build script changed, 2 files changed");

        /* the medium, made of these packages */
        const QList<Component> all = load(places);
        const QString toolsKey = key(find(all, "tools"), all);
        const QString mesaKey = key(find(all, "mesa"), all);
        const QByteArray script = QCryptographicHash::hash(read(guest + "/build-medium.sh"),
                                                           QCryptographicHash::Sha256).toHex();
        QVERIFY(write(places.mediumImage, "image\n"));
        QVERIFY(write(GuestToolsBuilder::manifestOf(places.mediumImage),
                      QString(R"({"mediumId": "m", "fedora": "44", "tools": "0.1.0-15.fc44",
                                  "built": "2026-10-13T10:00:00Z",
                                  "inputs": {"tools": "%1", "mesa": "%2", "medium": "%3"}})")
                          .arg(toolsKey, mesaKey, QString::fromLatin1(script))
                          .toUtf8()));
        Status medium = statusOf("medium");
        QCOMPARE(medium.state, Status::State::UpToDate);
        QCOMPARE(medium.version, "0.1.0-15.fc44");
        QVERIFY(write(guest + "/mesa/mesa-fix.patch", "fix\n"));
        medium = statusOf("medium");
        QCOMPARE(medium.state, Status::State::Outdated);
        QCOMPARE(medium.changes.size(), 1);
        QCOMPARE(describe(medium.changes[0]), "Mesa changed");
        QVERIFY(write(guest + "/build-medium.sh", "#!/bin/bash\n# other\n"));
        QCOMPARE(summary(statusOf("medium").changes), "build script changed, Mesa changed");
        /* a package it was made without: not built then */
        QVERIFY(write(guest + "/kwin/kwin.spec", "Name: kwin\nVersion: 6.7.5\n"));
        medium = statusOf("medium");
        QCOMPARE(describe(medium.changes.last()), "Built without KWin");
        QCOMPARE(summary(medium.changes), "build script changed, Mesa changed, without KWin");
    }
};

QTEST_GUILESS_MAIN(TestBuildComponents)
#include "test_buildcomponents.moc"
