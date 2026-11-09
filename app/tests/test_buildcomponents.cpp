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

    /* bash with @args, @env's NAME=value pairs on top of this process's environment */
    static QString run(const QStringList &args, int *status = nullptr, const QStringList &env = {})
    {
        QProcess p;
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        for (const QString &pair : env) {
            e.insert(pair.section('=', 0, 0), pair.section('=', 1));
        }
        p.setProcessEnvironment(e);
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
       QEMU, the libraries it is built against and the passt it runs, with
       their inputs; with @rutabaga or @passt false, as an older Vitrine
       left it, without rutabaga_gfx or passt */
    static bool builtStack(const QString &host, const QString &stack, bool rutabaga = true,
                           bool passt = true)
    {
        Places places;
        places.hostDir = host;
        const QList<Component> all = load(places);
        const Component virgl = find(all, "virglrenderer");
        const Component ffi = find(all, "rutabaga_gfx");
        const Component net = find(all, "passt");
        const Component qemu = find(all, "qemu");
        const QString vkey = key(virgl, all);
        const QString rkey = key(ffi, all);
        const QString pkey = key(net, all);
        const QString library = stack + "/virglrenderer/" + vkey.left(16);
        const QString rlibrary = stack + "/rutabaga_gfx/" + rkey.left(16);
        const QString plibrary = stack + "/passt/" + pkey.left(16);
        QByteArray inputs = inputLines(qemu, all);
        QFile::remove(stack + "/current");
        QDir(rlibrary).removeRecursively();
        QDir(plibrary).removeRecursively();
        /* an older one's inputs had no such line, and so another stamp */
        if (!rutabaga) {
            inputs.replace("dep rutabaga_gfx " + rkey.toLatin1() + '\n', "");
        }
        if (!passt) {
            inputs.replace("dep passt " + pkey.toLatin1() + '\n', "");
        }
        const QString stamp = QString::fromLatin1(
            QCryptographicHash::hash(inputs, QCryptographicHash::Sha256).toHex());
        const QString prefix = stack + '/' + stamp.left(16);
        QByteArray manifest = "STAMP=" + stamp.toUtf8() +
                              "\nBUILT=2026-10-12T09:00:00Z\nQEMU_VERSION=11.1.50\nVIRGL_KEY=" +
                              vkey.toUtf8() + "\nVIRGL_PREFIX=" + library.toUtf8() + '\n';
        if (rutabaga) {
            manifest += "RUTABAGA_KEY=" + rkey.toUtf8() + "\nRUTABAGA_PREFIX=" +
                        rlibrary.toUtf8() + '\n';
            if (!write(rlibrary + "/share/vitrine/rutabaga_gfx.conf",
                       "KEY=" + rkey.toUtf8() +
                           "\nBUILT=2026-10-12T08:30:00Z\nVERSION=0.1.85\n") ||
                !write(rlibrary + "/share/vitrine/rutabaga_gfx.inputs", inputLines(ffi, all))) {
                return false;
            }
        }
        if (passt) {
            manifest += "PASST_KEY=" + pkey.toUtf8() + "\nPASST_PREFIX=" + plibrary.toUtf8() + '\n';
            if (!write(plibrary + "/share/vitrine/passt.conf",
                       "KEY=" + pkey.toUtf8() +
                           "\nBUILT=2026-10-12T08:40:00Z\nVERSION=6aa4f88bd984+vitrine.1\n") ||
                !write(plibrary + "/share/vitrine/passt.inputs", inputLines(net, all))) {
                return false;
            }
        }
        return write(library + "/share/vitrine/virglrenderer.conf",
                     "KEY=" + vkey.toUtf8() + "\nBUILT=2026-10-12T08:00:00Z\nVERSION=1.3.0\n") &&
               write(library + "/share/vitrine/virglrenderer.inputs", inputLines(virgl, all)) &&
               write(prefix + "/share/vitrine/stack.conf", manifest) &&
               write(prefix + "/share/vitrine/qemu.inputs", inputs) &&
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
        QCOMPARE(ids, QStringList({"virglrenderer", "rutabaga_gfx", "passt", "qemu", "tools", "mesa",
                                   "kwayland", "kwin", "proxy", "kpipewire", "qtbase", "ksvg",
                                   "kirigami", "mutter", "medium"}));
        QCOMPARE(find(all, "qemu").kind, Kind::Host);
        QCOMPARE(find(all, "qemu").title, "QEMU");
        QCOMPARE(find(all, "qemu").deps, QStringList({"virglrenderer", "rutabaga_gfx"}));
        /* not built against passt: it runs it */
        QCOMPARE(find(all, "qemu").runs, QStringList({"passt"}));
        QCOMPARE(find(all, "rutabaga_gfx").kind, Kind::Host);
        QCOMPARE(find(all, "rutabaga_gfx").deps, QStringList({"virglrenderer"}));
        QVERIFY(find(all, "rutabaga_gfx").runs.isEmpty());
        QCOMPARE(find(all, "passt").kind, Kind::Host);
        QCOMPARE(find(all, "passt").title, "passt");
        QVERIFY(find(all, "passt").deps.isEmpty());
        QCOMPARE(patches(find(all, "passt")).size(), 1);
        QCOMPARE(patches(find(all, "passt")).first().origin, "Vitrine");
        QCOMPARE(find(all, "mesa").kind, Kind::Package);
        QVERIFY(find(all, "mesa").heavy);
        QVERIFY(!find(all, "tools").heavy);
        QCOMPARE(find(all, "medium").kind, Kind::Medium);
        QCOMPARE(find(all, "medium").deps,
                 QStringList({"tools", "mesa", "kwayland", "kwin", "proxy", "kpipewire", "qtbase",
                              "ksvg", "kirigami", "mutter"}));
        /* KWin built against vitrine's KWayland, whose signals its nested desktop uses */
        QCOMPARE(find(all, "kwayland").kind, Kind::Package);
        QCOMPARE(find(all, "kwayland").title, "KWayland");
        QVERIFY(!find(all, "kwayland").heavy);
        QVERIFY(find(all, "kwayland").deps.isEmpty());
        QCOMPARE(find(all, "kwin").deps, QStringList({"kwayland"}));
        QVERIFY(find(all, "kwin").heavy);
        QVERIFY(!find(all, "proxy").heavy);
        QVERIFY(!find(all, "kpipewire").heavy);
        QCOMPARE(find(all, "qtbase").title, "Qt Base");
        QVERIFY(find(all, "qtbase").heavy);
        QVERIFY(!find(all, "ksvg").heavy);
        QVERIFY(!find(all, "kirigami").heavy);
        /* Mutter: Fedora 45's, for its GNOME guests */
        QCOMPARE(find(all, "mutter").kind, Kind::Package);
        QCOMPARE(find(all, "mutter").title, "Mutter");
        QVERIFY(!find(all, "mutter").heavy);
        QVERIFY(find(all, "mutter").deps.isEmpty());
        QVERIFY(builtFor(find(all, "mutter"), "45"));
        QVERIFY(!builtFor(find(all, "mutter"), "44"));
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
        QCOMPARE(keys, QString("virglrenderer %1\nrutabaga_gfx %2\npasst %3\nqemu %4\n")
                           .arg(key(find(all, "virglrenderer"), all),
                                key(find(all, "rutabaga_gfx"), all), key(find(all, "passt"), all),
                                key(find(all, "qemu"), all)));
        /* QEMU's: those it is built against, then passt */
        QVERIFY(inputLines(find(all, "qemu"), all)
                    .endsWith("dep rutabaga_gfx " + key(find(all, "rutabaga_gfx"), all).toLatin1() +
                              "\ndep passt " + key(find(all, "passt"), all).toLatin1() + '\n'));
        for (const char *id : {"virglrenderer", "rutabaga_gfx", "passt", "qemu"}) {
            QCOMPARE(run({places.hostDir + "/build.sh", "--print-inputs", id}).toUtf8(),
                     inputLines(find(all, id), all));
        }
        /* the stamp, which the app compares with the build's */
        QCOMPARE(key(find(all, "qemu"), all), StackBuilder::inputStamp(places.hostDir));

        const QString inputs = run({places.guestDir + "/build-rpms.sh", "--print-inputs"}, &status);
        QCOMPARE(status, 0);
        for (const char *id : {"tools", "mesa", "kwayland", "kwin", "proxy", "kpipewire", "qtbase",
                               "ksvg", "kirigami"}) {
            QVERIFY2(inputs.contains(QString("%1 %2\n").arg(id, key(find(all, id), all))),
                     qPrintable(inputs));
            QCOMPARE(key(find(all, id), all), GuestToolsBuilder::inputsHash(places.guestDir, id));
        }
        /* KWin's: KWayland's key last */
        QVERIFY(inputLines(find(all, "kwin"), all)
                    .endsWith("\ndep kwayland " + key(find(all, "kwayland"), all).toLatin1() + '\n'));
        /* Mutter's, built for Fedora 45 only */
        QVERIFY2(!inputs.contains("mutter "), qPrintable(inputs));
        const QString inputs45 = run({places.guestDir + "/build-rpms.sh", "--print-inputs", "mutter"},
                                     &status, {"FEDORA_RELEASE=45"});
        QCOMPARE(status, 0);
        QCOMPARE(inputs45, QString("mutter %1\n").arg(key(find(all, "mutter"), all)));
        QCOMPARE(key(find(all, "mutter"), all),
                 GuestToolsBuilder::inputsHash(places.guestDir, "mutter"));
        /* the medium's: its packages' keys and its script */
        const QByteArray medium = inputLines(find(all, "medium"), all);
        QVERIFY(medium.startsWith("dep tools " + key(find(all, "tools"), all).toLatin1() + '\n'));
        QVERIFY(medium.endsWith("  build-medium.sh\n"));
    }

    /*
     * A distribution's spec: what rpm reads of it (the branch of a switch it
     * defines, both of any other condition), its %global words, its tags in
     * any case, and whose its patches are by their numbers
     */
    void distributionSpec()
    {
        const QString dir = m_tmp.filePath("distribution");
        QVERIFY(write(dir + "/qtfoo/qt6-foo.spec",
                      "%global qt_module qtfoo\n"
                      "#global unstable 0\n"
                      "%global examples 1\n"
                      "%if 0%{?unstable}\n%global prerelease rc\n%endif\n"
                      "Name:    qt6-foo\nVersion: 6.11.2\nRelease: 3.1.q4%{?dist}\n"
                      "Url:     http://qt-project.org/\n"
                      "%if 0%{?unstable}\n"
                      "Source0: https://d/development/%{qt_module}-src-%{version}-%{prerelease}.tar.xz\n"
                      "%else\n"
                      "Source0: https://d/official/%{qt_module}-src-%{version}.tar.xz\n"
                      "%endif\n"
                      "Patch1:  qtfoo-fedora.patch\n"
                      "%if 0%{?fedora}\nPatch50: qtfoo-fedora-only.patch\n%endif\n"
                      "%if 0%{?unstable}\nPatch60: qtfoo-unstable.patch\n%endif\n"
                      "%if 0%{?examples}\nPatch70: qtfoo-examples.patch\n%endif\n"
                      "# the research's\nPatch1001: qtfoo-research.patch\n"));
        Component c;
        c.id = "qtfoo";
        c.kind = Kind::Package;
        c.dir = dir;
        c.values = {{"spec", "qtfoo/qt6-foo.spec"}, {"patches", "spec"},
                    {"origin", "Research"}, {"origin.fedora", "Fedora"}};
        const Source s = source(c);
        QCOMPARE(s.archive, "qtfoo-src-6.11.2.tar.xz");
        QCOMPARE(s.archiveHost, "d");
        QCOMPARE(s.url, "http://qt-project.org/");
        QCOMPARE(s.revision, "6.11.2-3.1.q4");
        QStringList names, origins;
        for (const Patch &p : patches(c)) {
            names << p.name();
            origins << p.origin;
        }
        QCOMPARE(names, QStringList({"qtfoo-fedora.patch", "qtfoo-fedora-only.patch",
                                     "qtfoo-examples.patch", "qtfoo-research.patch"}));
        QCOMPARE(origins, QStringList({"Fedora", "Fedora", "Fedora", "Research"}));
        QCOMPARE(patches(c).last().note, "the research's");
        /* without origin.fedora: all of them the section's */
        c.values.remove("origin.fedora");
        for (const Patch &p : patches(c)) {
            QCOMPARE(p.origin, "Research");
        }

        /* GNOME's macros: a development release's tilde is a dot in its tarball's name */
        QVERIFY(write(dir + "/gnomefoo/gnome-foo.spec",
                      "Name: gnome-foo\nVersion: 52~rc\nRelease: 1.1.x1%{?dist}\n"
                      "Source0: http://download.gnome.org/sources/%{name}/%{gnome_major_version}/"
                      "%{name}-%{gnome_tarball_version}.tar.xz\n"));
        Component g;
        g.id = "gnomefoo";
        g.kind = Kind::Package;
        g.dir = dir;
        g.values = {{"spec", "gnomefoo/gnome-foo.spec"}};
        QCOMPARE(source(g).archive, "gnome-foo-52.rc.tar.xz");
        QCOMPARE(source(g).revision, "52~rc-1.1.x1");
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

        /* named without .patch (the driver's "apply NAME"), as a whole name only; a
           version rebased for a later kernel right after its patch */
        QVERIFY(write(dir + "/dkms/prepare.sh", "apply za\napply z\napply a\n"));
        for (const char *name : {"za.patch", "z-7.3.patch"}) {
            QVERIFY(write(dir + "/dkms/patches/" + name, "x\n"));
        }
        names.clear();
        for (const Patch &p : patches(c)) {
            names << p.name();
        }
        QCOMPARE(names, QStringList({"za.patch", "z.patch", "z-7.3.patch", "a.patch", "m.patch"}));
    }

    /* The real patches: by origin, in their order, each with its subject */
    void realPatches()
    {
        const QList<Component> all = load(real());

        const Component qemu = find(all, "qemu");
        const QList<Patch> qemuPatches = patches(qemu);
        QCOMPARE(qemuPatches.size(), StackBuilder::patches(qemu.dir, "qemu").size());
        QCOMPARE(qemuPatches.first().origin, "Research");
        QCOMPARE(qemuPatches.last().origin, "Vitrine");
        QStringList origins;
        for (const Patch &p : qemuPatches) {
            if (!origins.contains(p.origin)) {
                origins << p.origin;
            }
        }
        QCOMPARE(origins, QStringList({"Research", "Vitrine"}));
        /* the series' order, not the names': the blob scanouts come first */
        QStringList names;
        for (const Patch &p : qemuPatches) {
            names << p.name();
        }
        QVERIFY(names.indexOf("virtio-gpu-rutabaga-scanout-blob.patch") >= 0);
        QVERIFY(names.indexOf("virtio-gpu-rutabaga-scanout-blob.patch") <
                names.indexOf("virtio-gpu-rutabaga-gl-context-scope.patch"));
        /* a git format-patch Subject, its [PATCH 01/19] tag gone */
        QCOMPARE(subject(qemu, qemuPatches.first()),
                 "ui/sdl2: follow the host refresh rate, tolerate grab border");
        QVERIFY(text(qemu, qemuPatches.first()).startsWith("From "));
        /* the copies carry no links to the sessions that wrote them */
        for (const Component &c : all) {
            for (const Patch &p : patches(c)) {
                QVERIFY2(!text(c, p).contains("Claude-Session:") &&
                             !text(c, p).contains("claude.ai/code"),
                         qPrintable(p.path));
            }
        }

        /* a plain diff's description: its first line */
        const Component virgl = find(all, "virglrenderer");
        const QList<Patch> virglPatches = patches(virgl);
        QCOMPARE(virglPatches.size(), 4);
        QCOMPARE(virglPatches[0].origin, "Research");
        QCOMPARE(virglPatches[3].origin, "Vitrine");
        QCOMPARE(subject(virgl, virglPatches[1]),
                 "amdgpu: force write-combining for host-visible blob mappings");
        QCOMPARE(subject(virgl, virglPatches[3]), "xe: sync-only execs (no batch buffer) pass again");

        /* the research's diff, under a description of Vitrine's */
        const Component ffi = find(all, "rutabaga_gfx");
        const QList<Patch> ffiPatches = patches(ffi);
        QCOMPARE(ffiPatches.size(), 1);
        QCOMPARE(ffiPatches[0].origin, "Research");
        QVERIFY(subject(ffi, ffiPatches[0]).startsWith("rutabaga_gfx: native contexts through the FFI"));

        /* a spec's Patch lines: Fedora's own fix (6.8.2-3's Patch0), then the research's guest
           set g28 from Patch1001, each patch with its own subject */
        const Component kwin = find(all, "kwin");
        const QList<Patch> kwinPatches = patches(kwin);
        QCOMPARE(kwinPatches.size(), 62);
        QCOMPARE(kwinPatches.first().number, "0");
        QCOMPARE(kwinPatches.first().origin, "Fedora");
        QCOMPARE(subject(kwin, kwinPatches.first()), "compositor: handle possibly missing render device");
        QCOMPARE(kwinPatches[1].path, "kwin/0001-Patch100-kwin-vm-frame-timing-ported-to-6.8.v2.patch");
        QCOMPARE(kwinPatches[1].number, "1001");
        QCOMPARE(kwinPatches[1].origin, "Research");
        QCOMPARE(subject(kwin, kwinPatches[1]), "Patch100 kwin-vm-frame-timing (ported to 6.8)");
        QCOMPARE(kwinPatches[57].number, "1057");
        QCOMPARE(subject(kwin, kwinPatches[57]),
                 "EXP wayland backend: the cursor handed to the host as wl_shm (KWIN_WAYLAND_SHM_CURSOR)");
        QCOMPARE(kwinPatches[59].path,
                 "kwin/0096-EXP-opengl-the-GPU-s-clock-read-once-a-second-for-render-time-queries.patch");
        /* a Subject folded over two lines, read as one */
        QCOMPARE(kwinPatches[60].number, "1060");
        QCOMPARE(kwinPatches[60].path,
                 "kwin/0097-FIX-wayland-backend-frame-callback-and-feedback-on-the-content-layer.patch");
        QCOMPARE(subject(kwin, kwinPatches[60]),
                 "FIX wayland backend: the frame callback and the presentation feedback on the content layer");
        QCOMPARE(kwinPatches.last().number, "1061");
        QCOMPARE(kwinPatches.last().path,
                 "kwin/0098-FIX-wayland-backend-scroll-source-steps-and-direction-from-the-host.patch");
        QCOMPARE(subject(kwin, kwinPatches.last()),
                 "FIX wayland backend: the scroll source, wheel steps and direction from the host");
        QCOMPARE(kwinPatches.last().origin, "Research");
        for (const Patch &p : kwinPatches) {
            QVERIFY2(QFileInfo::exists(kwin.dir + '/' + p.path), qPrintable(p.path));
        }
        /* KWayland: Fedora's spec without patches of its own, the research's w1 */
        const Component kwayland = find(all, "kwayland");
        const QList<Patch> kwaylandPatches = patches(kwayland);
        QCOMPARE(kwaylandPatches.size(), 1);
        QCOMPARE(kwaylandPatches[0].number, "1001");
        QCOMPARE(kwaylandPatches[0].origin, "Research");
        QCOMPARE(kwaylandPatches[0].path,
                 "kwayland/0001-client-pointer-and-touch-events-of-wl_seat-versions-6-to-9.patch");
        QCOMPARE(subject(kwayland, kwaylandPatches[0]),
                 "client: the pointer and touch events of wl_seat versions 6 to 9");
        QVERIFY(QFileInfo::exists(kwayland.dir + '/' + kwaylandPatches[0].path));
        /* Mutter: Fedora's spec as its source RPM has it (rpmautospec's header, no patches of
           its own), the research's m1 as Patch1001 with the comment over it, its Subject
           folded over two lines */
        const Component mutter = find(all, "mutter");
        const QList<Patch> mutterPatches = patches(mutter);
        QCOMPARE(mutterPatches.size(), 1);
        QCOMPARE(mutterPatches[0].number, "1001");
        QCOMPARE(mutterPatches[0].origin, "Research");
        QCOMPARE(mutterPatches[0].path,
                 "mutter/0001-clutter-frame-clock-Seed-the-update-duration-estimat.patch");
        QCOMPARE(subject(mutter, mutterPatches[0]),
                 "clutter/frame-clock: Seed the update duration estimates with at most a refresh "
                 "cycle");
        QVERIFY2(mutterPatches[0].note.startsWith("A new frame clock seeded its update-duration "
                                                  "estimates with its first presented frame"),
                 qPrintable(mutterPatches[0].note));
        QVERIFY(QFileInfo::exists(mutter.dir + '/' + mutterPatches[0].path));
        const QList<Patch> mesaPatches = patches(find(all, "mesa"));
        QCOMPARE(mesaPatches.size(), 5);
        QCOMPARE(subject(find(all, "mesa"), mesaPatches[2]),
                 "iris: keep buffers in the reuse cache longer under virtio-gpu");
        /* the floor of big buffers, on the longer cache (the research's intel/0003) */
        QCOMPARE(mesaPatches[4].number, "106");
        QCOMPARE(subject(find(all, "mesa"), mesaPatches[4]),
                 "iris: under virtio-gpu, keep the newest big cached buffers past the timeout");

        /* the driver's patches, in the order prepare.sh applies them ("apply NAME"), a
           version rebased for a later kernel right after its patch (flip-grace-once is
           a patch of its own, not a version of flip-grace) */
        const QList<Patch> toolsPatches = patches(find(all, "tools"));
        QCOMPARE(toolsPatches.size(), 20);
        QCOMPARE(toolsPatches.first().name(), "linux-drm-virtio-attach-imported-dmabufs.patch");
        QCOMPARE(toolsPatches[1].name(), "linux-drm-virtio-import-3d.patch");
        QCOMPARE(toolsPatches[2].name(), "linux-drm-virtio-host-vblank.patch");
        QCOMPARE(toolsPatches[3].name(), "linux-drm-virtio-host-vblank-7.3.patch");
        QCOMPARE(toolsPatches[4].name(), "linux-drm-virtio-flip-grace.patch");
        QCOMPARE(toolsPatches[5].name(), "linux-drm-virtio-skip-unchanged-flush.patch");
        QCOMPARE(toolsPatches[16].name(), "linux-drm-virtio-deferred-map-error.patch");
        QCOMPARE(toolsPatches[17].name(), "linux-drm-virtio-scanout-modifier.patch");
        QCOMPARE(toolsPatches[18].name(), "linux-drm-virtio-scanout-modifier-7.3.patch");
        QCOMPARE(toolsPatches.last().name(), "linux-drm-virtio-flip-grace-once.patch");
        /* the nested desktop's: the research's work on the proxy as one patch, KPipeWire's k2 */
        const QList<Patch> proxyPatches = patches(find(all, "proxy"));
        QCOMPARE(proxyPatches.size(), 1);
        QCOMPARE(proxyPatches[0].path, "proxy/wl-cross-domain-proxy-research.patch");
        const QList<Patch> kpipewirePatches = patches(find(all, "kpipewire"));
        QCOMPARE(kpipewirePatches.size(), 2);
        /* Fedora's own, without a description nor a comment over its line: its name */
        QCOMPARE(kpipewirePatches.first().origin, "Fedora");
        QCOMPARE(subject(find(all, "kpipewire"), kpipewirePatches.first()), "kpipewire-ffmpeg8");
        QCOMPARE(kpipewirePatches.last().origin, "Research");
        QCOMPARE(kpipewirePatches.last().number, "1001");
        QCOMPARE(kpipewirePatches.last().path, "kpipewire/kpipewire-dmabuf-frame-fds-and-size.patch");
        /* Fedora's spec with the research's set after its own patches: Fedora's, by their
           numbers below 1000, apart from the research's */
        const Component qtbase = find(all, "qtbase");
        const QList<Patch> qtPatches = patches(qtbase);
        QCOMPARE(qtPatches.size(), 10);
        for (qsizetype i = 0; i < qtPatches.size() - 1; i++) {
            QCOMPARE(qtPatches[i].origin, "Fedora");
        }
        QCOMPARE(qtPatches.last().origin, "Research");
        QCOMPARE(qtPatches.last().number, "1001");
        QCOMPARE(qtPatches.last().path, "qtbase/qtbase-wayland-shm-reuse-pools-v3.patch");
        QVERIFY(subject(qtbase, qtPatches.last()).startsWith("Qt Wayland: a resized window reuses its shm pools"));
        const QList<Patch> ksvgPatches = patches(find(all, "ksvg"));
        QCOMPARE(ksvgPatches.size(), 2);
        QCOMPARE(ksvgPatches[0].origin, "Research");
        QCOMPARE(subject(find(all, "ksvg"), ksvgPatches[1]),
                 "FrameSvgItem: release the previous nodes after the new ones took their textures");
        const QList<Patch> kirigamiPatches = patches(find(all, "kirigami"));
        QCOMPARE(kirigamiPatches.size(), 1);
        QCOMPARE(kirigamiPatches[0].origin, "Research");
        for (const char *id : {"qtbase", "ksvg", "kirigami"}) {
            for (const Patch &p : patches(find(all, id))) {
                QVERIFY2(QFileInfo::exists(qtbase.dir + '/' + p.path), qPrintable(p.path));
            }
        }
        QVERIFY(patches(find(all, "medium")).isEmpty());
    }

    void sources()
    {
        const QList<Component> all = load(real());

        const Source qemu = source(find(all, "qemu"));
        QCOMPARE(qemu.url, "https://gitlab.com/qemu-project/qemu.git");
        QCOMPARE(qemu.revision.size(), 40);
        const Source ffi = source(find(all, "rutabaga_gfx"));
        QCOMPARE(ffi.url, "https://github.com/magma-gpu/rutabaga_gfx.git");
        QCOMPARE(ffi.revision.size(), 40);
        const Source passt = source(find(all, "passt"));
        QCOMPARE(passt.url, "https://passt.top/passt");
        QCOMPARE(passt.revision.size(), 40);
        const Source mesa = source(find(all, "mesa"));
        QCOMPARE(mesa.archive, "mesa-" + GuestToolsBuilder::version(find(all, "mesa").dir, "mesa") +
                                   ".tar.xz");
        QCOMPARE(mesa.archiveHost, "archive.mesa3d.org");
        QVERIFY(mesa.revision.startsWith(GuestToolsBuilder::version(find(all, "mesa").dir, "mesa") + '-'));
        QCOMPARE(mesa.url, "https://mesa3d.org");
        const Source kwin = source(find(all, "kwin"));
        QVERIFY(kwin.archive.startsWith("kwin-") && kwin.archive.endsWith(".tar.xz"));
        QCOMPARE(kwin.archiveHost, "download.kde.org");
        QCOMPARE(kwin.revision, "6.8.2-3.3.g28");
        const Source kwayland = source(find(all, "kwayland"));
        QCOMPARE(kwayland.archive, "kwayland-6.8.2.tar.xz");
        QCOMPARE(kwayland.archiveHost, "download.kde.org");
        QCOMPARE(kwayland.url, "https://invent.kde.org/plasma/kwayland");
        QCOMPARE(kwayland.revision, "6.8.2-1.1.w1");
        const Source proxy = source(find(all, "proxy"));
        QCOMPARE(proxy.archive, "wl-cross-domain-proxy-c6ce1ca.tar.gz");
        QCOMPARE(proxy.archiveHost, "codeberg.org");
        QVERIFY(proxy.revision.startsWith("0.1.0^"));
        const Source kpipewire = source(find(all, "kpipewire"));
        QVERIFY(kpipewire.archive.startsWith("kpipewire-") && kpipewire.archive.endsWith(".tar.xz"));
        QCOMPARE(kpipewire.archiveHost, "download.kde.org");
        /* their %global words (%{qt_module}, %{framework}), their tags in any case (Url:) */
        const Source qtbase = source(find(all, "qtbase"));
        QCOMPARE(qtbase.archive, "qtbase-everywhere-src-6.11.2.tar.xz");
        QCOMPARE(qtbase.archiveHost, "download.qt.io");
        QCOMPARE(qtbase.url, "http://qt-project.org/");
        QCOMPARE(qtbase.revision, "6.11.2-3.1.q4");
        const Source ksvg = source(find(all, "ksvg"));
        QCOMPARE(ksvg.archive, "ksvg-6.31.0.tar.xz");
        QCOMPARE(ksvg.archiveHost, "download.kde.org");
        QCOMPARE(ksvg.url, "https://invent.kde.org/frameworks/ksvg");
        QCOMPARE(ksvg.revision, "6.31.0-1.1.k2");
        const Source kirigami = source(find(all, "kirigami"));
        QCOMPARE(kirigami.archive, "kirigami-6.31.0.tar.xz");
        QCOMPARE(kirigami.revision, "6.31.0-1.1.f1");
        /* GNOME's %{gnome_tarball_version}; the release set by hand in place of %autorelease */
        const Source mutter = source(find(all, "mutter"));
        QCOMPARE(mutter.archive, "mutter-51.1.tar.xz");
        QCOMPARE(mutter.archiveHost, "download.gnome.org");
        QCOMPARE(mutter.url, "http://www.gnome.org");
        QCOMPARE(mutter.revision, "51.1-1.1.m1");
        QCOMPARE(GuestToolsBuilder::version(find(all, "mutter").dir, "mutter"), "51.1");
        const Source tools = source(find(all, "tools"));
        QVERIFY(tools.archiveHost.isEmpty());
        QVERIFY(tools.revision.startsWith("0.1.0-"));
    }

    /* What is built of the host's components, and why it is out of date */
    void hostStatus()
    {
        const QByteArray pins = "QEMU_URL=https://q/qemu.git\nQEMU_COMMIT=%1\n"
                                "VIRGL_URL=https://v/virgl.git\nVIRGL_COMMIT=%2\n"
                                "RUTABAGA_URL=https://r/rutabaga.git\nRUTABAGA_COMMIT=4444\n"
                                "PASST_URL=https://p/passt\nPASST_COMMIT=5555\n"
                                "PASST_SHA256=0000\n";
        const QString host =
            hostFixture("status-host", QString(pins).arg("1111", "2222").toUtf8(),
                        {{"qemu/fork/0001-a.patch", "a\n"},
                         {"qemu/research/0001-b.patch", "b\n"},
                         {"virglrenderer/0001-v.patch", "v\n"},
                         {"rutabaga_gfx/changes.patch", "r\n"},
                         {"passt/0001-p.patch", "p\n"}});
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
        Status ffi = statusOf("rutabaga_gfx");
        QCOMPARE(ffi.state, Status::State::UpToDate);
        QCOMPARE(ffi.version, "0.1.85");
        Status passt = statusOf("passt");
        QCOMPARE(passt.state, Status::State::UpToDate);
        QCOMPARE(passt.version, "6aa4f88bd984+vitrine.1");
        QCOMPARE(QFileInfo(passt.output).dir().dirName(), "passt");

        /* a passt patch changed: it, and QEMU, which runs it (a new folder
           naming it, nothing compiled: build.sh's QEMU configure line has
           no passt) */
        QVERIFY(write(host + "/patches/passt/0001-p.patch", "p, again\n"));
        passt = statusOf("passt");
        QCOMPARE(passt.state, Status::State::Outdated);
        QCOMPARE(summary(passt.changes), "1 patch changed");
        QCOMPARE(describe(passt.changes[0]), "Patch 0001-p.patch changed");
        qemu = statusOf("qemu");
        QCOMPARE(qemu.state, Status::State::Outdated);
        QCOMPARE(summary(qemu.changes), "passt changed");
        QCOMPARE(statusOf("virglrenderer").state, Status::State::UpToDate);
        QCOMPARE(statusOf("rutabaga_gfx").state, Status::State::UpToDate);
        QVERIFY(write(host + "/patches/passt/0001-p.patch", "p\n"));
        QCOMPARE(statusOf("qemu").state, Status::State::UpToDate);
        /* a QEMU of an older Vitrine, without it: the system's passt ran */
        QVERIFY(builtStack(host, places.stackDir, true, false));
        QCOMPARE(statusOf("passt").state, Status::State::NotBuilt);
        qemu = statusOf("qemu");
        QCOMPARE(qemu.state, Status::State::Outdated);
        QCOMPARE(describe(qemu.changes.value(0)), "Built without passt");
        QVERIFY(builtStack(host, places.stackDir));
        QCOMPARE(statusOf("qemu").state, Status::State::UpToDate);

        /* a rutabaga_gfx patch changed: it, and QEMU built against it */
        QVERIFY(write(host + "/patches/rutabaga_gfx/changes.patch", "r, rebased\n"));
        ffi = statusOf("rutabaga_gfx");
        QCOMPARE(ffi.state, Status::State::Outdated);
        QCOMPARE(summary(ffi.changes), "1 patch changed");
        QCOMPARE(describe(ffi.changes[0]), "Patch changes.patch changed");
        QCOMPARE(summary(statusOf("qemu").changes), "rutabaga_gfx changed");
        QCOMPARE(statusOf("virglrenderer").state, Status::State::UpToDate);
        QVERIFY(write(host + "/patches/rutabaga_gfx/changes.patch", "r\n"));
        QCOMPARE(statusOf("qemu").state, Status::State::UpToDate);

        /* a QEMU of an older Vitrine, built without it: it is not built yet,
           and that QEMU is out of date */
        QVERIFY(builtStack(host, places.stackDir, false));
        QCOMPARE(statusOf("rutabaga_gfx").state, Status::State::NotBuilt);
        const Status without = statusOf("qemu");
        QCOMPARE(without.state, Status::State::Outdated);
        QCOMPARE(without.changes.size(), 1);
        QCOMPARE(describe(without.changes[0]), "Built without rutabaga_gfx");
        QCOMPARE(summary(without.changes), "without rutabaga_gfx");
        QVERIFY(builtStack(host, places.stackDir));
        QCOMPARE(statusOf("qemu").state, Status::State::UpToDate);

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
        QCOMPARE(qemu.changes.size(), 2);
        QCOMPARE(qemu.changes[0].what, Change::What::Dependency);
        QCOMPARE(describe(qemu.changes[0]), "virglrenderer changed");
        QCOMPARE(summary(qemu.changes), "virglrenderer changed, rutabaga_gfx changed");
        /* passt is built against nothing */
        QCOMPARE(statusOf("passt").state, Status::State::UpToDate);
        /* rutabaga_gfx too, built against it */
        QCOMPARE(summary(statusOf("rutabaga_gfx").changes), "virglrenderer changed");
        /* the script changed: all of them */
        QVERIFY(builtStack(host, places.stackDir));
        QVERIFY(write(host + "/build.sh", read(host + "/build.sh") + "\n# changed\n"));
        QCOMPARE(summary(statusOf("virglrenderer").changes), "build script changed");
        QCOMPARE(summary(statusOf("qemu").changes),
                 "build script changed, virglrenderer changed, rutabaga_gfx changed, "
                 "passt changed");
        QCOMPARE(summary(statusOf("passt").changes), "build script changed");

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

    /* A package built against another: that one's key among its inputs, and
       what changed when that one did */
    void packageDeps()
    {
        const QString guest = m_tmp.filePath("deps-guest");
        Places places;
        places.guestDir = guest;
        places.rpmsDir = m_tmp.filePath("deps-rpms");
        QVERIFY(write(guest + "/rpm-build-inside.sh", "#!/bin/bash\n"));
        QVERIFY(write(guest + "/build-medium.sh", "#!/bin/bash\n"));
        QVERIFY(write(guest + "/kwayland/kwayland.spec", "Name: kwayland\nVersion: 6.8.2\n"));
        QVERIFY(write(guest + "/kwin/kwin.spec", "Name: kwin\nVersion: 6.8.2\n"));
        const QByteArray conf = "[kwayland]\ntitle=KWayland\nspec=kwayland/kwayland.spec\n"
                                "[kwin]\ntitle=KWin\nspec=kwin/kwin.spec\ndeps=kwayland\n"
                                "[medium]\nkind=medium\ndeps=kwayland kwin\n";
        QVERIFY(write(guest + "/components.conf", conf));
        const auto statusOf = [&places](const char *id) {
            const QList<Component> all = load(places);
            return status(find(all, id), all, places);
        };

        QList<Component> all = load(places);
        QCOMPARE(find(all, "kwin").kind, Kind::Package);
        QCOMPARE(find(all, "kwin").deps, QStringList({"kwayland"}));
        const QByteArray lines = inputLines(find(all, "kwin"), all);
        QVERIFY(lines.endsWith("\ndep kwayland " + key(find(all, "kwayland"), all).toLatin1() +
                               '\n'));
        QCOMPARE(lines, GuestToolsBuilder::inputLines(guest, "kwin"));
        QCOMPARE(key(find(all, "kwin"), all), GuestToolsBuilder::inputsHash(guest, "kwin"));
        QVERIFY(write(places.rpmsDir + "/kwin/.inputs", key(find(all, "kwin"), all).toUtf8()));
        QVERIFY(write(places.rpmsDir + "/kwin/.inputs.list", lines));
        QVERIFY(write(places.rpmsDir + "/kwin/kwin-6.8.2-3.3.g28.fc44.x86_64.rpm", "x"));
        QCOMPARE(statusOf("kwin").state, Status::State::UpToDate);

        /* KWayland's patch: KWin out of date, by it alone */
        QVERIFY(write(guest + "/kwayland/0001-x.patch", "x\n"));
        Status kwin = statusOf("kwin");
        QCOMPARE(kwin.state, Status::State::Outdated);
        QCOMPARE(kwin.changes.size(), 1);
        QCOMPARE(kwin.changes[0].what, Change::What::Dependency);
        QCOMPARE(describe(kwin.changes[0]), "KWayland changed");
        QCOMPARE(summary(kwin.changes), "KWayland changed");
        /* a build of before KWayland: built without it */
        QVERIFY(write(places.rpmsDir + "/kwin/.inputs.list", lines.left(lines.indexOf("dep "))));
        kwin = statusOf("kwin");
        QCOMPARE(kwin.changes.size(), 1);
        QCOMPARE(describe(kwin.changes[0]), "Built without KWayland");

        /* one built against no package here: no key, nothing to build from */
        QVERIFY(write(guest + "/components.conf",
                      QByteArray(conf).replace("deps=kwayland\n", "deps=nothing\n")));
        all = load(places);
        QVERIFY(key(find(all, "kwin"), all).isEmpty());
        QCOMPARE(statusOf("kwin").state, Status::State::NoSources);
    }
};

QTEST_GUILESS_MAIN(TestBuildComponents)
#include "test_buildcomponents.moc"
