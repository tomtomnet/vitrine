// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <csignal>
#include <sys/file.h>

#include "core/paths.h"
#include "core/stackbuilder.h"

/* A manifest as host/build.sh writes it */
static const char kManifest[] = R"(# What host/build.sh built here
STAMP=%1
BUILT=2026-10-03T11:30:26Z
QEMU_URL=https://gitlab.com/qemu-project/qemu.git
QEMU_COMMIT=3876503faff51ce3a132dbbd0ab42c10f2269319
QEMU_VERSION=11.1.50
QEMU_PATCHES=fork/0001-a.patch fork/0002-b.patch research/0001-c.patch research/0002-d.patch research/0003-e.patch research/0004-f.patch vitrine/0001-g.patch
VIRGL_URL=https://gitlab.freedesktop.org/virgl/virglrenderer.git
VIRGL_COMMIT=cf6c62da2a1384b194f463e6221371962fe99575
VIRGL_VERSION=1.3.0
VIRGL_PATCHES=0001-x.patch 0002-y.patch 0003-z.patch
)";

/*
 * Stand-ins for the components, which the real build.sh builds: a
 * virglrenderer meson project taking build.sh's options, and a QEMU whose
 * configure writes a Makefile linking a "qemu-system-x86_64" against it
 * with configure's --extra-ldflags, as QEMU's does
 */
static const char kVirglMeson[] = R"(project('virglrenderer', 'c', version : '1.3.0')
lib = shared_library('virglrenderer', 'virgl.c', version : '1.9.0', install : true)
import('pkgconfig').generate(lib, name : 'virglrenderer', description : 'test')
)";
static const char kVirglOptions[] = R"(option('drm-renderers', type : 'array', value : [],
       choices : ['amdgpu-experimental', 'i915-experimental', 'xe-experimental', 'msm',
                  'asahi'])
option('video', type : 'boolean', value : false)
option('venus', type : 'boolean', value : false)
)";
static const char kQemuConfigure[] = R"sh(#!/bin/sh
src=$(cd "$(dirname "$0")" && pwd)
for a; do
    case "$a" in
    --prefix=*) prefix=${a#*=} ;;
    --extra-ldflags=*) ldflags=${a#*=} ;;
    esac
done
echo "#define HAVE_VIRGL_RESOURCE_SET_GUEST_DMABUF 1" > config-host.h
: > build.ninja
t=$(printf '\t')
cat > Makefile <<EOF
all: qemu-system-x86_64 qemu-img
qemu-system-x86_64: $src/qemu.c
${t}cc -o \$@ $src/qemu.c $(pkg-config --cflags --libs virglrenderer) $ldflags
qemu-img: $src/qemu.c
${t}cc -o \$@ $src/qemu.c $(pkg-config --cflags --libs virglrenderer) $ldflags
install: all
${t}mkdir -p $prefix/bin $prefix/share/qemu
${t}cp qemu-system-x86_64 qemu-img $prefix/bin/
EOF
)sh";
static const char kQemuMain[] = R"c(#include <stdio.h>
#include <string.h>
int virgl_renderer_init(void);
int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-version") || !strcmp(argv[1], "--version"))) {
        printf("QEMU emulator version 9.9.9\n");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-device")) {
        printf("virtio-gpu-gl-pci options:\n  drm_native_context=<bool>\n  x-host-vblank=<bool>\n");
        return 0;
    }
    return virgl_renderer_init();
}
)c";

class TestStackBuilder : public QObject
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

    static QString run(const QString &program, const QStringList &args, const QString &dir = {},
                       int *status = nullptr)
    {
        QProcess p;
        p.setWorkingDirectory(dir);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(program, args);
        p.waitForFinished(120000);
        if (status) {
            *status = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        }
        return QString::fromUtf8(p.readAll());
    }

    static QString git(const QString &dir, const QStringList &args)
    {
        return run("git", QStringList{"-c", "user.name=t", "-c", "user.email=t@t"} + args, dir)
            .trimmed();
    }

    /* A host/ with build.sh and versions.conf as given, and patches */
    QString hostDir(const QString &name, const QByteArray &buildSh,
                    const QByteArray &versions = "QEMU_URL=a\nQEMU_COMMIT=b\nVIRGL_URL=c\n"
                                                 "VIRGL_COMMIT=d\n")
    {
        const QString dir = m_tmp.filePath(name);
        if (!write(dir + "/build.sh", buildSh, true) || !write(dir + "/versions.conf", versions)) {
            return {};
        }
        return dir;
    }

    static QString log(const QSignalSpy &output)
    {
        QString text;
        for (const QList<QVariant> &args : output) {
            text += args[0].toString();
        }
        return text;
    }

    /* Runs @builder to its end: the error, "timeout" if it did not end */
    static QString build(StackBuilder &builder, int timeout = 60000)
    {
        QSignalSpy finished(&builder, &StackBuilder::finished);
        builder.start();
        if (finished.isEmpty() && !finished.wait(timeout)) {
            return "timeout";
        }
        return finished[0][0].toString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        /* Paths' stack and settings in here */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
    }

    void parsesVersions()
    {
        const StackBuilder::Versions v = StackBuilder::Versions::parse(
            "# QEMU_URL=commented\n"
            "\n"
            "QEMU_URL=https://example.org/qemu.git   \n"
            "QEMU_COMMIT=1111\n"
            "QEMU_COMMIT=2222\n"
            "  VIRGL_URL=https://example.org/virgl.git\n"
            "VIRGL_COMMIT=3333\n"
            "OTHER=x\n");
        QCOMPARE(v.qemuUrl, "https://example.org/qemu.git");
        /* the last one, as build.sh reads it */
        QCOMPARE(v.qemuCommit, "2222");
        QCOMPARE(v.virglUrl, "https://example.org/virgl.git");
        QCOMPARE(v.virglCommit, "3333");
        QVERIFY(v.isValid());
        QVERIFY(!StackBuilder::Versions::parse("QEMU_URL=x\nQEMU_COMMIT=y\nVIRGL_URL=z\n")
                     .isValid());

        /* the real one */
        const QString host = StackBuilder::hostDir();
        QVERIFY2(!host.isEmpty(), "host/ of the source tree");
        const StackBuilder::Versions real = StackBuilder::versions(host);
        QVERIFY(real.isValid());
        /* upstream QEMU: the fork's commits are patches */
        QCOMPARE(real.qemuUrl, "https://gitlab.com/qemu-project/qemu.git");
        QCOMPARE(real.qemuCommit.size(), 40);
        QCOMPARE(real.virglCommit.size(), 40);
    }

    void parsesLines()
    {
        using Line = StackBuilder::Line;
        Line l = Line::parse("vitrine-build: step 3/8: building virglrenderer\n");
        QCOMPARE(l.kind, Line::Step);
        QCOMPARE(l.done, 3);
        QCOMPARE(l.total, 8);
        QCOMPARE(l.text, "building virglrenderer");

        l = Line::parse("[1038/1939] Compiling C object libblock.a.p/block_stream.c.o");
        QCOMPARE(l.kind, Line::Progress);
        QCOMPARE(l.done, 1038);
        QCOMPARE(l.total, 1939);

        const QList<std::pair<QString, Line::Kind>> tagged = {
            {"missing: pkg-config sdl2", Line::Missing},
            {"install: sudo dnf install SDL2-devel", Line::Install},
            {"warning: no passt on this host", Line::Warning},
            {"up to date: /s/0123456789abcdef", Line::UpToDate},
            {"built: /s/0123456789abcdef", Line::Built},
            {"error: compiling QEMU failed (status 2)", Line::Error},
        };
        for (const auto &[text, kind] : tagged) {
            l = Line::parse("vitrine-build: " + text + "\r");
            QCOMPARE(l.kind, kind);
            QCOMPARE(l.text, text.section(": ", 1));
        }
        QCOMPARE(Line::parse("Compiling C object foo.o").kind, Line::Other);
        /* tagged lines only: another tool's "step" is not one */
        QCOMPARE(Line::parse("step 1/2: something").kind, Line::Other);
        QCOMPARE(Line::parse("vitrine-build: what").kind, Line::Other);
    }

    void parsesManifest()
    {
        using Build = StackBuilder::Build;
        const QString stamp(64, 'a');
        Build b = Build::parse("/p", QString(kManifest).arg(stamp).toUtf8());

        QVERIFY(b.isValid());
        QCOMPARE(b.stamp, stamp);
        QCOMPARE(b.prefix, "/p");
        QCOMPARE(b.qemuBinary(), "/p/bin/qemu-system-x86_64");
        QCOMPARE(b.built, QDateTime(QDate(2026, 10, 3), QTime(11, 30, 26), QTimeZone::UTC));
        QCOMPARE(b.qemuVersion, "11.1.50");
        QCOMPARE(b.qemuPatches.size(), 7);
        QCOMPARE(b.virglPatches, QStringList({"0001-x.patch", "0002-y.patch", "0003-z.patch"}));
        QCOMPARE(b.virglVersion, "1.3.0");
        QCOMPARE(b.summary(), "QEMU 11.1.50 + 7 patches");
        b.qemuPatches = {"0001-a.patch"};
        QCOMPARE(b.summary(), "QEMU 11.1.50 + 1 patch");
        b.qemuPatches.clear();
        QCOMPARE(b.summary(), "QEMU 11.1.50");

        /* none without a stamp: not a complete build */
        QVERIFY(!Build::parse("/p", "QEMU_VERSION=1\n").isValid());
        QVERIFY(!Build::read(m_tmp.filePath("nowhere")).isValid());
        QVERIFY(!Build().isValid());
        QVERIFY(Build().qemuBinary().isEmpty());
    }

    /* The app's stamp is build.sh's: for the real host/, and with patch names
       whose order differs between locales */
    void stampIsBuildShs()
    {
        int status = 0;
        const QString host = StackBuilder::hostDir();
        const QString stamp = StackBuilder::inputStamp(host);

        QCOMPARE(stamp.size(), 64);
        QCOMPARE(run("bash", {host + "/build.sh", "--print-stamp"}, {}, &status).trimmed(), stamp);
        QCOMPARE(status, 0);

        const QString fixture = m_tmp.filePath("stamp-host");
        QVERIFY(QDir().mkpath(fixture + "/patches/qemu"));
        QVERIFY(QFile::copy(host + "/build.sh", fixture + "/build.sh"));
        QVERIFY(QFile::copy(host + "/versions.conf", fixture + "/versions.conf"));
        for (const QString name : {"b.patch", "B.patch", "0010-x.patch", "0002-y.patch", "_z.patch"}) {
            QVERIFY(write(fixture + "/patches/qemu/" + name, name.toUtf8() + '\n'));
        }
        /* no virglrenderer patches at all */
        const QString fixtureStamp = StackBuilder::inputStamp(fixture);
        QCOMPARE(run("bash", {fixture + "/build.sh", "--print-stamp"}, {}, &status).trimmed(),
                 fixtureStamp);
        QVERIFY(fixtureStamp != stamp);

        /* what build.sh does not read changes nothing */
        QVERIFY(write(fixture + "/patches/README.md", "notes\n"));
        QVERIFY(write(fixture + "/patches/qemu/notes.txt", "notes\n"));
        QVERIFY(write(fixture + "/patches/qemu/.hidden.patch", "x\n"));
        /* bash's glob matches with case */
        QVERIFY(write(fixture + "/patches/qemu/0008-extra.PATCH", "x\n"));
        QVERIFY(write(fixture + "/patches/qemu/0009-mixed.Patch", "x\n"));
        QCOMPARE(StackBuilder::inputStamp(fixture), fixtureStamp);
        QCOMPARE(run("bash", {fixture + "/build.sh", "--print-stamp"}, {}, &status).trimmed(),
                 fixtureStamp);
        /* a patch does, a new one too, and versions.conf */
        QVERIFY(write(fixture + "/patches/qemu/b.patch", "changed\n"));
        const QString changed = StackBuilder::inputStamp(fixture);
        QVERIFY(changed != fixtureStamp);
        QVERIFY(write(fixture + "/patches/virglrenderer/0001-v.patch", "v\n"));
        QVERIFY(StackBuilder::inputStamp(fixture) != changed);
        QCOMPARE(run("bash", {fixture + "/build.sh", "--print-stamp"}, {}, &status).trimmed(),
                 StackBuilder::inputStamp(fixture));

        QVERIFY(StackBuilder::inputStamp(m_tmp.filePath("nowhere")).isEmpty());
        QVERIFY(StackBuilder::inputStamp({}).isEmpty());
    }

    /* The patches of a component's folders too (QEMU's research/, vitrine/), in
       the C order of their paths, as build.sh applies them */
    void stampOfPatchFolders()
    {
        int status = 0;
        const QString host = StackBuilder::hostDir();
        const QString fixture = m_tmp.filePath("folders-host");
        QVERIFY(QDir().mkpath(fixture));
        QVERIFY(QFile::copy(host + "/build.sh", fixture + "/build.sh"));
        QVERIFY(QFile::copy(host + "/versions.conf", fixture + "/versions.conf"));
        for (const QString name : {"vitrine/0001-v.patch", "fork/0002-f.patch", "fork/0010-f.patch",
                                   "research/0001-r.patch", "fork/0001-f.patch", "B/b.patch",
                                   "a/a.patch", "_c/c.patch", "0001-top.patch",
                                   "fork-x/0001-x.patch"}) {
            QVERIFY(write(fixture + "/patches/qemu/" + name, name.toUtf8() + '\n'));
        }
        /* a folder's patches together, whatever the locale: sort's C order of
           the paths, where "fork-x/" comes before "fork/" ('-' < '/') */
        const QStringList expected = {
            "patches/qemu/0001-top.patch",       "patches/qemu/B/b.patch",
            "patches/qemu/_c/c.patch",           "patches/qemu/a/a.patch",
            "patches/qemu/fork-x/0001-x.patch",  "patches/qemu/fork/0001-f.patch",
            "patches/qemu/fork/0002-f.patch",    "patches/qemu/fork/0010-f.patch",
            "patches/qemu/research/0001-r.patch", "patches/qemu/vitrine/0001-v.patch"};
        QCOMPARE(StackBuilder::patches(fixture, "qemu"), expected);
        QVERIFY(StackBuilder::patches(fixture, "virglrenderer").isEmpty());
        const QString stamp = StackBuilder::inputStamp(fixture);
        QCOMPARE(run("bash", {fixture + "/build.sh", "--print-stamp"}, {}, &status).trimmed(),
                 stamp);
        QCOMPARE(status, 0);

        /* not applied: deeper folders, hidden ones, hidden files, other names */
        for (const QString name : {"fork/old/0001-f.patch", ".hidden/0001-h.patch",
                                   "fork/.0003-h.patch", "fork/README.md", "fork/0004-f.PATCH"}) {
            QVERIFY(write(fixture + "/patches/qemu/" + name, name.toUtf8() + '\n'));
        }
        QCOMPARE(StackBuilder::patches(fixture, "qemu"), expected);
        QCOMPARE(StackBuilder::inputStamp(fixture), stamp);
        QCOMPARE(run("bash", {fixture + "/build.sh", "--print-stamp"}, {}, &status).trimmed(),
                 stamp);

        /* a patch moved to another folder is another input */
        QVERIFY(QFile::rename(fixture + "/patches/qemu/research/0001-r.patch",
                              fixture + "/patches/qemu/vitrine/0000-r.patch"));
        QVERIFY(StackBuilder::inputStamp(fixture) != stamp);
        QCOMPARE(run("bash", {fixture + "/build.sh", "--print-stamp"}, {}, &status).trimmed(),
                 StackBuilder::inputStamp(fixture));

        QVERIFY(StackBuilder::patches({}, "qemu").isEmpty());
        QVERIFY(StackBuilder::patches(m_tmp.filePath("nowhere"), "qemu").isEmpty());
    }

    void detectsVersionChange()
    {
        using State = StackBuilder::State;
        const QString host = m_tmp.filePath("state-host");
        const QString stack = m_tmp.filePath("state-stack");

        QVERIFY(write(host + "/build.sh", "#!/bin/bash\n"));
        QVERIFY(write(host + "/versions.conf", "QEMU_COMMIT=1\n"));
        QVERIFY(write(host + "/patches/qemu/0001-a.patch", "a\n"));
        QVERIFY(QDir().mkpath(stack));

        QCOMPARE(StackBuilder::state(m_tmp.filePath("nowhere"), stack), State::NoSources);
        QCOMPARE(StackBuilder::state({}, stack), State::NoSources);
        QCOMPARE(StackBuilder::state(host, stack), State::NotBuilt);

        /* a prefix without its manifest: a build that did not end */
        QVERIFY(QDir().mkpath(stack + "/0123456789abcdef/bin"));
        QVERIFY(QFile::link("0123456789abcdef", stack + "/current"));
        QCOMPARE(StackBuilder::state(host, stack), State::NotBuilt);
        QVERIFY(!StackBuilder::current(stack).isValid());

        QVERIFY(write(stack + "/0123456789abcdef/share/vitrine/stack.conf",
                      QString(kManifest).arg(StackBuilder::inputStamp(host)).toUtf8()));
        QCOMPARE(StackBuilder::state(host, stack), State::UpToDate);
        const StackBuilder::Build current = StackBuilder::current(stack);
        QVERIFY(current.isValid());
        /* the link resolved: the next build is another path */
        QCOMPARE(current.prefix, QFileInfo(stack + "/0123456789abcdef").canonicalFilePath());

        /* this Vitrine pins something else */
        QVERIFY(write(host + "/patches/qemu/0001-a.patch", "a, rebased\n"));
        QCOMPARE(StackBuilder::state(host, stack), State::Outdated);
        QVERIFY(write(host + "/patches/qemu/0001-a.patch", "a\n"));
        QCOMPARE(StackBuilder::state(host, stack), State::UpToDate);
        QVERIFY(write(host + "/versions.conf", "QEMU_COMMIT=2\n"));
        QCOMPARE(StackBuilder::state(host, stack), State::Outdated);

        /* VITRINE_HOST_DIR chooses, even none */
        qputenv("VITRINE_HOST_DIR", host.toUtf8());
        QCOMPARE(StackBuilder::hostDir(), QFileInfo(host).canonicalFilePath());
        qputenv("VITRINE_HOST_DIR", m_tmp.filePath("nowhere").toUtf8());
        QVERIFY(StackBuilder::hostDir().isEmpty());
        qunsetenv("VITRINE_HOST_DIR");
    }

    /* The VMs' QEMU: the preferences', else the stack's, else PATH's */
    void defaultQemuIsTheStacks()
    {
        const QString prefix = Paths::stackDir() + "/fedcba9876543210";
        const QString binary = prefix + "/bin/" + Paths::qemuSystemName();

        QVERIFY(Paths::stackDir().startsWith(m_tmp.path()));
        QVERIFY(Paths::stackQemu().isEmpty());
        QCOMPARE(Paths::defaultQemuBinary(), QStandardPaths::findExecutable(Paths::qemuSystemName()));

        QVERIFY(write(binary, "#!/bin/sh\n", true));
        QVERIFY(QFile::link("fedcba9876543210", Paths::stackDir() + "/current"));
        QCOMPARE(Paths::stackQemu(), QFileInfo(binary).canonicalFilePath());
        QCOMPARE(Paths::qemuBinary(), Paths::stackQemu());
        QVERIFY(!Paths::qemuBinary().contains("/current/"));

        Paths::setQemuBinary("/opt/qemu/bin/qemu-system-x86_64");
        QCOMPARE(Paths::customQemuBinary(), "/opt/qemu/bin/qemu-system-x86_64");
        QCOMPARE(Paths::qemuBinary(), "/opt/qemu/bin/qemu-system-x86_64");
        QCOMPARE(Paths::defaultQemuBinary(), Paths::stackQemu());
        Paths::setQemuBinary({});
        QCOMPARE(Paths::qemuBinary(), Paths::stackQemu());
        /* qemu-img comes from the same prefix */
        QVERIFY(write(prefix + "/bin/qemu-img", "#!/bin/sh\n", true));
        QCOMPARE(QFileInfo(Paths::qemuImg()).canonicalFilePath(),
                 QFileInfo(prefix + "/bin/qemu-img").canonicalFilePath());

        QVERIFY(QFile::remove(Paths::stackDir() + "/current"));
    }

    /* What the app makes of build.sh's lines, and the hook after a build */
    void runsBuildSh()
    {
        const QString host = hostDir("run-host", R"sh(#!/bin/bash
echo "$@" > "$(dirname "$0")/args"
stack=$2
echo "vitrine-build: step 1/2: checking the build dependencies"
echo "vitrine-build: warning: no passt on this host"
echo "vitrine-build: step 2/2: compiling QEMU"
printf '[1/4] Compiling a\n[4/4] Linking qemu-system-x86_64\n'
mkdir -p "$stack/0123456789abcdef/bin" "$stack/0123456789abcdef/share/vitrine"
echo STAMP=abc > "$stack/0123456789abcdef/share/vitrine/stack.conf"
ln -sfn 0123456789abcdef "$stack/current"
echo -n "vitrine-build: built: $stack/0123456789abcdef"
)sh");
        const QString stack = m_tmp.filePath("run-stack");
        StackBuilder b;
        b.setHostDir(host);
        b.setStackDir(stack);
        b.setWorkDir(m_tmp.filePath("run-work"));
        b.setJobs(3);
        QSignalSpy started(&b, &StackBuilder::started);
        QSignalSpy steps(&b, &StackBuilder::stepStarted);
        QSignalSpy progress(&b, &StackBuilder::progress);
        QSignalSpy built(&b, &StackBuilder::built);
        QSignalSpy output(&b, &StackBuilder::output);

        QCOMPARE(build(b), "");
        QCOMPARE(started.size(), 1);
        QFile args(host + "/args");
        QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(args.readAll().trimmed(),
                 QString("--stack %1 --work %2 -j 3").arg(stack, m_tmp.filePath("run-work")).toUtf8());
        QCOMPARE(steps.size(), 2);
        QCOMPARE(steps[1][0].toInt(), 2);
        QCOMPARE(steps[1][1].toInt(), 2);
        QCOMPARE(steps[1][2].toString(), "compiling QEMU");
        QCOMPARE(b.step(), 2);
        QCOMPARE(b.stepText(), "compiling QEMU");
        QCOMPARE(progress.size(), 2);
        QCOMPARE(progress[1][0].toInt(), 4);
        QCOMPARE(b.warnings(), QStringList({"no passt on this host"}));
        /* the last line had no end: read all the same */
        QCOMPARE(built.size(), 1);
        QCOMPARE(built[0][0].toString(),
                 QFileInfo(stack + "/0123456789abcdef").canonicalFilePath() +
                     "/bin/qemu-system-x86_64");
        QVERIFY(b.log().contains("[4/4] Linking"));
        QVERIFY(log(output).contains("[4/4] Linking"));
        QVERIFY(!b.isRunning());
        QVERIFY(!b.wasStopped());
    }

    void reportsFailures()
    {
        StackBuilder b;
        b.setStackDir(m_tmp.filePath("fail-stack"));
        b.setWorkDir(m_tmp.filePath("fail-work"));
        QSignalSpy built(&b, &StackBuilder::built);

        b.setHostDir(hostDir("missing-host", R"sh(#!/bin/bash
echo "vitrine-build: step 1/8: checking the build dependencies"
echo "vitrine-build: missing: pkg-config sdl2"
echo "vitrine-build: missing: command meson"
echo "vitrine-build: install: sudo dnf install SDL2-devel meson"
echo "vitrine-build: error: build dependencies are missing"
exit 1
)sh"));
        QCOMPARE(build(b), "Build dependencies are missing");
        QCOMPARE(b.missing(), QStringList({"pkg-config sdl2", "command meson"}));
        QCOMPARE(b.installCommand(), "sudo dnf install SDL2-devel meson");

        b.setHostDir(hostDir("error-host", R"sh(#!/bin/bash
echo "vitrine-build: error: compiling QEMU failed (status 2)"
exit 2
)sh"));
        QCOMPARE(build(b), "Compiling QEMU failed (status 2)");
        QVERIFY(b.missing().isEmpty());

        b.setHostDir(hostDir("silent-host", "#!/bin/bash\nexit 3\n"));
        QCOMPARE(build(b), "The build failed (status 3)");

        /* success, but no complete build where it said */
        b.setHostDir(hostDir("liar-host", "#!/bin/bash\necho 'vitrine-build: built: /nowhere'\n"));
        QCOMPARE(build(b), "build.sh ended without a complete build");

        b.setHostDir(m_tmp.filePath("nowhere"));
        QVERIFY(build(b).contains("no host/build.sh"));
        QVERIFY(built.isEmpty());
    }

    /* Stop ends the compilers too, not bash alone (which waits for them) */
    void stopsTheProcessGroup()
    {
        const QString pidFile = m_tmp.filePath("stop-pid");
        StackBuilder b;
        b.setHostDir(hostDir("stop-host", QString(R"sh(#!/bin/bash
echo "vitrine-build: step 6/8: compiling QEMU"
sh -c 'echo $$ > %1; exec sleep 300'
echo "vitrine-build: built: /nowhere"
)sh").arg(pidFile).toUtf8()));
        b.setStackDir(m_tmp.filePath("stop-stack"));
        b.setWorkDir(m_tmp.filePath("stop-work"));
        QSignalSpy finished(&b, &StackBuilder::finished);
        QSignalSpy steps(&b, &StackBuilder::stepStarted);

        b.start();
        QVERIFY(b.isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0 && !steps.isEmpty(), 10000);
        QFile f(pidFile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const pid_t sleeper = pid_t(f.readAll().trimmed().toLongLong());
        QVERIFY(sleeper > 0);
        QCOMPARE(::kill(sleeper, 0), 0);

        b.cancel();
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][0].toString(), "Stopped");
        QVERIFY(b.wasStopped());
        /* gone, and reaped by whoever adopted it */
        QTRY_VERIFY_WITH_TIMEOUT(::kill(sleeper, 0) != 0 && errno == ESRCH, 10000);
    }

    /* The builds nothing runs or names any more go */
    void prunesOldBuilds()
    {
        const QString stack = m_tmp.filePath("prune-stack");
        const auto prefix = [&stack](const char *name, bool complete = true) {
            const QString dir = stack + '/' + name;
            const QString binary = dir + "/bin/qemu-system-x86_64";
            if (!write(binary, "x\n", true) ||
                (complete && !write(dir + "/share/vitrine/stack.conf", "STAMP=x\n"))) {
                return QString();
            }
            return binary;
        };
        QVERIFY(!prefix("aaaaaaaaaaaaaaaa").isEmpty());
        QVERIFY(!prefix("bbbbbbbbbbbbbbbb").isEmpty());
        const QString named = prefix("cccccccccccccccc");
        const QString running = prefix("dddddddddddddddd");
        QVERIFY(!prefix("eeeeeeeeeeeeeeee", false).isEmpty());
        QVERIFY(QDir().mkpath(stack + "/other"));
        QVERIFY(QFile::link("aaaaaaaaaaaaaaaa", stack + "/current"));
        /* a QEMU of an older build, still running */
        QVERIFY(QFile::remove(running));
        QVERIFY(QFile::copy(QStandardPaths::findExecutable("sleep"), running));
        QVERIFY(QFile::setPermissions(running, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QProcess vm;
        vm.start(running, {"60"});
        QVERIFY(vm.waitForStarted());

        /* not while a build holds the stack */
        QFile lock(stack + "/.lock");
        QVERIFY(lock.open(QIODevice::WriteOnly));
        QVERIFY(::flock(lock.handle(), LOCK_EX | LOCK_NB) == 0);
        QVERIFY(StackBuilder::prune(stack, {}).isEmpty());
        ::flock(lock.handle(), LOCK_UN);
        lock.close();

        const QStringList removed = StackBuilder::prune(stack, {named});
        vm.kill();
        vm.waitForFinished();
        const QString canonical = QFileInfo(stack).canonicalFilePath();
        QCOMPARE(removed, QStringList({canonical + "/bbbbbbbbbbbbbbbb",
                                       canonical + "/eeeeeeeeeeeeeeee"}));
        for (const char *kept : {"aaaaaaaaaaaaaaaa", "cccccccccccccccc", "dddddddddddddddd",
                                 "other"}) {
            QVERIFY2(QFileInfo::exists(stack + '/' + kept), kept);
        }
        QVERIFY(StackBuilder::current(stack).isValid());
    }

    /* A Stop that came once `current` switched: the build is done all the same */
    void stopAfterTheSwitch()
    {
        const QString pidFile = m_tmp.filePath("late-pid");
        const QString stack = m_tmp.filePath("late-stack");
        StackBuilder b;
        b.setHostDir(hostDir("late-host", QString(R"sh(#!/bin/bash
stack=$2
echo "vitrine-build: step 8/8: checking the installation"
mkdir -p "$stack/0123456789abcdef/bin" "$stack/0123456789abcdef/share/vitrine"
echo STAMP=abc > "$stack/0123456789abcdef/share/vitrine/stack.conf"
ln -sfn 0123456789abcdef "$stack/current"
sh -c 'echo $$ > %1; exec sleep 300'
echo "vitrine-build: built: $stack/0123456789abcdef"
)sh").arg(pidFile).toUtf8()));
        b.setStackDir(stack);
        b.setWorkDir(m_tmp.filePath("late-work"));
        QSignalSpy finished(&b, &StackBuilder::finished);
        QSignalSpy built(&b, &StackBuilder::built);

        b.start();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 10000);
        b.cancel();
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][0].toString(), "");
        QCOMPARE(built.size(), 1);
        QCOMPARE(built[0][0].toString(), StackBuilder::current(stack).qemuBinary());
        QVERIFY(!b.wasStopped());

        /* stopped before it switched: stopped, `current` as it was */
        QFile::remove(pidFile);
        b.setHostDir(hostDir("early-host", QString(R"sh(#!/bin/bash
sh -c 'echo $$ > %1; exec sleep 300'
)sh").arg(pidFile).toUtf8()));
        finished.clear();
        b.start();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 10000);
        b.cancel();
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][0].toString(), "Stopped");
        QCOMPARE(built.size(), 1);
        QVERIFY(b.wasStopped());
    }

    /*
     * The real build.sh with stand-ins for the components: fetch at the
     * pinned commits, patches, prefix per stamp, RUNPATH and ldd checks,
     * `current` flipped only after a complete build, nothing redone when
     * nothing changed, and nothing left of a failed build
     */
    void buildShEndToEnd()
    {
        for (const char *tool : {"git", "meson", "ninja", "cc", "make", "readelf", "ldd",
                                 "pkg-config", "flock"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty()) {
                QSKIP(qPrintable(QString("needs %1").arg(tool)));
            }
        }
        const QString realHost = StackBuilder::hostDir();
        int status = 0;
        /* the stand-ins run no configure of QEMU's: the Python modules only it
           needs may be stand-ins too */
        const QString python = m_tmp.filePath("e2e/python");
        for (const char *module : {"yaml", "wheel", "setuptools", "pip"}) {
            run("python3", {"-c", QString("import %1").arg(module)}, {}, &status);
            if (status != 0) {
                QVERIFY(write(QString("%1/%2/__init__.py").arg(python, module), ""));
            }
        }
        const QByteArray pythonPath = qgetenv("PYTHONPATH");
        qputenv("PYTHONPATH", python.toUtf8() + (pythonPath.isEmpty() ? "" : ":" + pythonPath));
        const QString deps = run("bash", {realHost + "/build.sh", "--print-deps"}, {}, &status);
        if (status != 0) {
            QSKIP(qPrintable("needs the build dependencies of build.sh:\n" + deps));
        }
        const auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        };

        /* the components' repositories, QEMU's with a meson subproject of its own */
        const QString virglRepo = m_tmp.filePath("e2e/virgl-repo");
        const QString qemuRepo = m_tmp.filePath("e2e/qemu-repo");
        const QString subRepo = m_tmp.filePath("e2e/keycodemapdb-repo");
        QVERIFY(write(virglRepo + "/meson.build", kVirglMeson));
        QVERIFY(write(virglRepo + "/meson_options.txt", kVirglOptions));
        QVERIFY(write(virglRepo + "/virgl.c", "int virgl_renderer_init(void) { return 0; }\n"));
        QVERIFY(write(subRepo + "/README", "v1\n"));
        QVERIFY(write(qemuRepo + "/configure", kQemuConfigure, true));
        QVERIFY(write(qemuRepo + "/qemu.c", kQemuMain));
        QVERIFY(write(qemuRepo + "/qemu-options.hx", "DEF(\"m\", HAS_ARG, QEMU_OPTION_m, \"\", QEMU_ARCH_ALL)\n"));
        QVERIFY(write(qemuRepo + "/meson.build", "project('qemu', 'c')\n"));
        QVERIFY(write(qemuRepo + "/subprojects/.gitignore", "/keycodemapdb\n"));
        QVERIFY(write(qemuRepo + "/subprojects/packagefiles/keycodemapdb/meson.build", "pf1\n"));
        for (const QString &repo : {virglRepo, subRepo}) {
            git(repo, {"init", "-q", "-b", "main"});
            git(repo, {"add", "."});
            git(repo, {"commit", "-q", "-m", "first"});
        }
        const auto wrap = [&]() {
            return QString("[wrap-git]\nurl = file://%1\nrevision = %2\ndepth = 1\n"
                           "patch_directory = keycodemapdb\n")
                .arg(subRepo, git(subRepo, {"rev-parse", "HEAD"}))
                .toUtf8();
        };
        QVERIFY(write(qemuRepo + "/subprojects/keycodemapdb.wrap", wrap()));
        git(qemuRepo, {"init", "-q", "-b", "main"});
        git(qemuRepo, {"add", "."});
        git(qemuRepo, {"commit", "-q", "-m", "first"});

        /* host/: the real build.sh, its versions and patches */
        const QString host = m_tmp.filePath("e2e/host");
        QVERIFY(QDir().mkpath(host));
        QVERIFY(QFile::copy(realHost + "/build.sh", host + "/build.sh"));
        const auto versions = [&]() {
            return QString("QEMU_URL=file://%1\nQEMU_COMMIT=%2\nVIRGL_URL=file://%3\n"
                           "VIRGL_COMMIT=%4\n")
                .arg(qemuRepo, git(qemuRepo, {"rev-parse", "HEAD"}), virglRepo,
                     git(virglRepo, {"rev-parse", "HEAD"}))
                .toUtf8();
        };
        QVERIFY(write(host + "/versions.conf", versions()));
        const QByteArray newFile = "diff --git a/%1 b/%1\nnew file mode 100644\n"
                                   "--- /dev/null\n+++ b/%1\n@@ -0,0 +1 @@\n+%2\n";
        QVERIFY(write(host + "/patches/qemu/fork/0001-mark.patch",
                      QString(newFile).arg("MARK", "one").toUtf8()));
        QVERIFY(write(host + "/patches/virglrenderer/0001-mark.patch",
                      QString(newFile).arg("MARK", "virgl").toUtf8()));
        /* folder after folder: by their names alone, research's would come
           before the file it changes is there */
        QVERIFY(write(host + "/patches/qemu/fork/0002-base.patch",
                      QString(newFile).arg("BASE", "base").toUtf8()));
        QVERIFY(write(host + "/patches/qemu/research/0001-on-base.patch",
                      "diff --git a/BASE b/BASE\n--- a/BASE\n+++ b/BASE\n"
                      "@@ -1 +1 @@\n-base\n+base, then research\n"));

        const QString stack = m_tmp.filePath("e2e/stack");
        const QString work = m_tmp.filePath("e2e/work");
        StackBuilder b;
        b.setHostDir(host);
        b.setStackDir(stack);
        b.setWorkDir(work);
        b.setJobs(4);
        QSignalSpy output(&b, &StackBuilder::output);
        QSignalSpy built(&b, &StackBuilder::built);
        QSignalSpy steps(&b, &StackBuilder::stepStarted);

        /* 1. from nothing */
        QString error = build(b, 300000);
        QVERIFY2(error.isEmpty(), qPrintable(error + "\n" + b.log()));
        QCOMPARE(steps.size(), 8);
        const StackBuilder::Build first = StackBuilder::current(stack);
        QVERIFY(first.isValid());
        const QString stamp = StackBuilder::inputStamp(host);
        QCOMPARE(first.stamp, stamp);
        QCOMPARE(QFileInfo(first.prefix).fileName(), stamp.left(16));
        QCOMPARE(first.qemuVersion, "9.9.9");
        QCOMPARE(first.qemuPatches, QStringList({"fork/0001-mark.patch", "fork/0002-base.patch",
                                                 "research/0001-on-base.patch"}));
        QCOMPARE(first.virglPatches, QStringList({"0001-mark.patch"}));
        QCOMPARE(first.virglVersion, "1.3.0");
        QCOMPARE(StackBuilder::state(host, stack), StackBuilder::State::UpToDate);
        QCOMPARE(built.size(), 1);
        QCOMPARE(built[0][0].toString(), first.qemuBinary());
        QVERIFY(QFileInfo::exists(first.prefix + "/share/qemu/qemu-options.hx"));
        QVERIFY(QFileInfo::exists(work + "/src/qemu/MARK"));
        QCOMPARE(read(work + "/src/qemu/BASE"), "base, then research\n");
        QVERIFY(QFileInfo::exists(work + "/src/virglrenderer/MARK"));
        /* the subproject, downloaded with the sources, its patch files in */
        const QString sub = work + "/src/qemu/subprojects/keycodemapdb";
        QCOMPARE(read(sub + "/README"), "v1\n");
        QCOMPARE(read(sub + "/meson.build"), "pf1\n");
        QVERIFY(write(sub + "/KEPT", "\n"));
        /* the run path build.sh checked */
        const QString dynamic = run("readelf", {"-d", first.qemuBinary()});
        QVERIFY2(dynamic.contains("RUNPATH") && dynamic.contains("[" + first.prefix + "/lib64]"),
                 qPrintable(dynamic));
        QVERIFY(b.log().contains("loads " + first.prefix + "/lib64/libvirglrenderer.so.1"));

        /* 2. nothing changed: nothing done */
        output.clear();
        QCOMPARE(build(b), "");
        QVERIFY2(log(output).contains("vitrine-build: up to date: " + first.prefix),
                 qPrintable(log(output)));
        QVERIFY(!log(output).contains("step 2/8"));
        QCOMPARE(built.size(), 2);

        /* 3. a QEMU patch changed: a new prefix, virglrenderer's sources as they were */
        QVERIFY(write(host + "/patches/qemu/fork/0001-mark.patch",
                      QString(newFile).arg("MARK", "two").toUtf8()));
        QCOMPARE(StackBuilder::state(host, stack), StackBuilder::State::Outdated);
        output.clear();
        error = build(b, 300000);
        QVERIFY2(error.isEmpty(), qPrintable(error + "\n" + log(output)));
        const StackBuilder::Build second = StackBuilder::current(stack);
        QVERIFY(second.isValid());
        QVERIFY(second.prefix != first.prefix);
        QVERIFY2(log(output).contains("virglrenderer at ") &&
                     log(output).contains("with 1 patches already"),
                 qPrintable(log(output)));
        QVERIFY(log(output).contains("applying patches/qemu/fork/0001-mark.patch\n"
                                     "applying patches/qemu/fork/0002-base.patch\n"
                                     "applying patches/qemu/research/0001-on-base.patch\n"));
        QFile mark(work + "/src/qemu/MARK");
        QVERIFY(mark.open(QIODevice::ReadOnly));
        QCOMPARE(mark.readAll(), "two\n");
        /* what the subproject comes from did not change: kept, not downloaded again */
        QVERIFY(QFileInfo::exists(sub + "/KEPT"));
        /* the VMs running keep theirs */
        QVERIFY(StackBuilder::Build::read(first.prefix).isValid());
        QVERIFY(QFileInfo(first.qemuBinary()).isExecutable());

        /* 4. a patch that does not apply: an error, `current` as it was, no half prefix */
        QVERIFY(write(host + "/patches/qemu/0002-broken.patch",
                      "--- a/nothing\n+++ b/nothing\n@@ -1 +1 @@\n-a\n+b\n"));
        output.clear();
        error = build(b, 300000);
        QVERIFY2(error.contains("0002-broken.patch does not apply"), qPrintable(error));
        QCOMPARE(StackBuilder::current(stack).prefix, second.prefix);
        QVERIFY(!QFileInfo::exists(stack + '/' + StackBuilder::inputStamp(host).left(16)));
        QCOMPARE(StackBuilder::state(host, stack), StackBuilder::State::Outdated);
        QCOMPARE(built.size(), 3);

        /* 5. into a prefix of the caller's, which is installed over */
        QVERIFY(QFile::remove(host + "/patches/qemu/0002-broken.patch"));
        const QString prefix = m_tmp.filePath("e2e/prefix");
        const QString manifest = prefix + "/share/vitrine/stack.conf";
        const auto buildInto = [&]() {
            return run("bash", {host + "/build.sh", "--prefix", prefix, "--work", work, "-j", "4"},
                       {}, &status);
        };
        QString out = buildInto();
        QVERIFY2(status == 0 && out.contains("vitrine-build: built: " + prefix), qPrintable(out));
        QVERIFY(StackBuilder::Build::read(prefix).isValid());
        /* virglrenderer changed: QEMU's configure probes it again */
        QVERIFY(write(host + "/patches/virglrenderer/0001-mark.patch",
                      QString(newFile).arg("MARK", "virgl two").toUtf8()));
        out = buildInto();
        QVERIFY2(status == 0 && !out.contains("configured already"), qPrintable(out));
        const QString built2 = StackBuilder::Build::read(prefix).stamp;
        /* failed half-way: no manifest, nothing taken for complete */
        QVERIFY(write(host + "/patches/qemu/0002-broken.patch",
                      "--- a/nothing\n+++ b/nothing\n@@ -1 +1 @@\n-a\n+b\n"));
        out = buildInto();
        QVERIFY2(status != 0 && out.contains("does not apply"), qPrintable(out));
        QVERIFY(!QFileInfo::exists(manifest));
        QVERIFY(QFile::remove(host + "/patches/qemu/0002-broken.patch"));
        out = buildInto();
        QVERIFY2(status == 0 && !out.contains("up to date"), qPrintable(out));
        QCOMPARE(StackBuilder::Build::read(prefix).stamp, built2);

        /* 6. the subproject follows what it comes from */
        const auto buildOk = [&]() {
            output.clear();
            const QString error = build(b, 300000);
            return error.isEmpty() ? QString() : error + "\n" + log(output);
        };
        /* its patch files, changed by a patch */
        QVERIFY(write(host + "/patches/qemu/0002-packagefiles.patch",
                      "diff --git a/subprojects/packagefiles/keycodemapdb/meson.build "
                      "b/subprojects/packagefiles/keycodemapdb/meson.build\n"
                      "--- a/subprojects/packagefiles/keycodemapdb/meson.build\n"
                      "+++ b/subprojects/packagefiles/keycodemapdb/meson.build\n"
                      "@@ -1 +1 @@\n-pf1\n+pf2\n"));
        QString failure = buildOk();
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(read(sub + "/meson.build"), "pf2\n");
        QVERIFY(!QFileInfo::exists(sub + "/KEPT"));
        /* its revision, in a new QEMU commit */
        QVERIFY(write(subRepo + "/README", "v2\n"));
        git(subRepo, {"commit", "-q", "-am", "second"});
        QVERIFY(write(qemuRepo + "/subprojects/keycodemapdb.wrap", wrap()));
        git(qemuRepo, {"commit", "-q", "-am", "new keycodemapdb"});
        QVERIFY(write(host + "/versions.conf", versions()));
        failure = buildOk();
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(read(sub + "/README"), "v2\n");
        QCOMPARE(read(sub + "/meson.build"), "pf2\n");
        /* a download cut short, then a build of other inputs: downloaded again */
        QVERIFY(QDir(sub).removeRecursively());
        QVERIFY(QDir().mkpath(sub + "/.git"));
        QVERIFY(QFile::remove(work + "/src/qemu.subprojects"));
        QVERIFY(write(host + "/patches/virglrenderer/0001-mark.patch",
                      QString(newFile).arg("MARK", "virgl three").toUtf8()));
        failure = buildOk();
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(read(sub + "/README"), "v2\n");
        /* nothing else of the last build stays in the sources: nested repositories neither */
        QVERIFY(QDir().mkpath(work + "/src/qemu/subprojects/stale/.git"));
        QVERIFY(write(host + "/patches/qemu/fork/0001-mark.patch",
                      QString(newFile).arg("MARK", "three").toUtf8()));
        failure = buildOk();
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(!QFileInfo::exists(work + "/src/qemu/subprojects/stale"));
        QCOMPARE(read(sub + "/README"), "v2\n");
    }
};

QTEST_GUILESS_MAIN(TestStackBuilder)
#include "test_stackbuilder.moc"
