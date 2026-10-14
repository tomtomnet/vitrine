// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "core/buildcomponents.h"
#include "core/buildmanager.h"
#include "core/guesttoolsbuilder.h"
#include "core/stackbuilder.h"
#include "core/tasks.h"

using State = BuildManager::State;

/*
 * A stand-in for host/build.sh: its lines, per component, as the real one
 * says them, and a QEMU, a virglrenderer and a rutabaga_gfx where it says.
 * HOST_HOLD holds virglrenderer's build until that file is there,
 * QEMU_HOLD QEMU's compile half-way; HOST_FAIL=qemu fails it.  (No line of
 * these scripts starts with a #, nor has a \": moc would take them for a
 * directive or a string's end.)
 */
static const char kBuildSh[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
stack=
target=qemu
while [ $# -gt 0 ]; do
	case "$1" in
	--stack) stack=$2; shift ;;
	--work|-j) shift ;;
	--clean) echo clean >> "$here/calls" ;;
	--*) ;;
	*) target=$1 ;;
	esac
	shift
done
echo "$target" >> "$here/calls"
echo "vitrine-build: step 1/4: checking the build dependencies"
echo "vitrine-build: component: virglrenderer"
echo "vitrine-build: step 2/4: building virglrenderer"
[ -z "${HOST_HOLD:-}" ] || while [ ! -e "$HOST_HOLD" ]; do sleep 0.05; done
echo "[1/2] cc a.c"
echo "[2/2] cc b.c"
lib=$stack/virglrenderer/aaaaaaaaaaaaaaaa
mkdir -p "$lib/share/vitrine"
echo KEY=k > "$lib/share/vitrine/virglrenderer.conf"
echo "vitrine-build: done: virglrenderer built $lib"
if [ "$target" = virglrenderer ]; then
	echo "vitrine-build: built: $lib"
	exit 0
fi
echo "vitrine-build: component: rutabaga_gfx"
echo "vitrine-build: step 3/4: building rutabaga_gfx"
echo "[1/1] rustc lib.rs"
ffi=$stack/rutabaga_gfx/bbbbbbbbbbbbbbbb
mkdir -p "$ffi/share/vitrine"
echo KEY=k > "$ffi/share/vitrine/rutabaga_gfx.conf"
echo "vitrine-build: done: rutabaga_gfx built $ffi"
if [ "$target" = rutabaga_gfx ]; then
	echo "vitrine-build: built: $ffi"
	exit 0
fi
echo "vitrine-build: component: qemu"
echo "vitrine-build: step 4/4: compiling QEMU"
if [ "${HOST_FAIL:-}" = qemu ]; then
	echo "vitrine-build: error: compiling QEMU failed (status 2)"
	exit 2
fi
echo "[1/4] cc q.c"
echo "[2/4] cc r.c"
[ -z "${QEMU_HOLD:-}" ] || while [ ! -e "$QEMU_HOLD" ]; do sleep 0.05; done
echo "[4/4] link qemu-system-x86_64"
prefix=$stack/0123456789abcdef
mkdir -p "$prefix/share/vitrine" "$prefix/bin"
echo STAMP=s > "$prefix/share/vitrine/stack.conf"
ln -sfn 0123456789abcdef "$stack/current"
echo "vitrine-build: done: qemu built $prefix"
echo "vitrine-build: built: $prefix"
)sh";

/* A stand-in for guest/build-rpms.sh: GUEST_FAIL=PACKAGE fails it,
   GUEST_RESTORE=PACKAGE brings its build before back */
static const char kRpmsSh[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
pkg=${@: -1}
echo "$pkg" >> "$here/rpms-calls"
echo "vitrine-build: building: $pkg (log: $RPMS_DIR/$pkg.log)"
echo "vitrine-build: phase: installing rpmbuild"
echo "vitrine-build: phase: building the packages"
echo "Executing(%build): /bin/sh -e /var/tmp/rpm-tmp.b"
echo "[1/10] cc a.c"
echo "[10/10] cc z.c"
echo "Executing(%install): /bin/sh -e /var/tmp/rpm-tmp.i"
if [ "${GUEST_FAIL:-}" = "$pkg" ]; then
	echo "vitrine-build: error: building $pkg failed (status 1)"
	exit 1
fi
if [ "${GUEST_RESTORE:-}" = "$pkg" ]; then
	echo "vitrine-build: restored: $RPMS_DIR/$pkg"
	exit 0
fi
mkdir -p "$RPMS_DIR/$pkg"
echo x > "$RPMS_DIR/$pkg/.inputs"
echo "vitrine-build: built: $RPMS_DIR/$pkg"
)sh";

/* A stand-in for guest/build-medium.sh: a medium, with its manifest */
static const char kMediumSh[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
echo medium >> "$here/medium-calls"
echo "vitrine-build: phase: making the repository"
mkdir -p "$(dirname "$OUT")"
echo image > "$OUT"
printf '{"mediumId": "m1", "fedora": "44", "tools": "0.1.0-15.fc44", "built": "2026-10-14T10:00:00Z", "inputs": {}, "packages": []}\n' > "${OUT%.img}.json"
echo "vitrine-build: built: $OUT"
)sh";

class TestBuildManager : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_path;

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

    /*
     * @m with a host/ and a guest/ of stand-ins in folders of @name, its
     * builders, history and logs there too; the components.conf files the
     * real ones
     */
    Builds::Places setUp(BuildManager &m, const QString &name)
    {
        const QString dir = m_tmp.filePath(name);
        Builds::Places places;
        places.hostDir = dir + "/host";
        places.guestDir = dir + "/guest";
        places.stackDir = dir + "/stack";
        places.rpmsDir = dir + "/rpms";
        places.mediumImage = dir + "/media/vitrine-guest-tools-fc44.img";
        const bool ok =
            write(places.hostDir + "/build.sh", kBuildSh, true) &&
            write(places.hostDir + "/versions.conf",
                  "QEMU_URL=https://q/qemu.git\nQEMU_COMMIT=1\nVIRGL_URL=https://v/v.git\n"
                  "VIRGL_COMMIT=2\nRUTABAGA_URL=https://r/r.git\nRUTABAGA_COMMIT=3\n") &&
            write(places.hostDir + "/patches/qemu/0001-a.patch", "Subject: [PATCH] a\n") &&
            QFile::copy(StackBuilder::hostDir() + "/components.conf",
                        places.hostDir + "/components.conf") &&
            write(places.guestDir + "/build-rpms.sh", kRpmsSh, true) &&
            write(places.guestDir + "/build-medium.sh", kMediumSh, true) &&
            write(places.guestDir + "/rpm-build-inside.sh", "#!/bin/bash\n", true) &&
            write(places.guestDir + "/tools/vitrine-guest-tools.spec",
                  "Name: vitrine-guest-tools\nVersion: 0.1.0\n") &&
            write(places.guestDir + "/mesa/mesa.spec", "Name: mesa\nVersion: 26.2.3\n") &&
            write(places.guestDir + "/kwin/kwin.spec", "Name: kwin\nVersion: 6.7.5\n") &&
            write(places.guestDir + "/proxy/wl-cross-domain-proxy.spec",
                  "Name: wl-cross-domain-proxy\nVersion: 0.1.0\n") &&
            write(places.guestDir + "/kpipewire/kpipewire.spec", "Name: kpipewire\nVersion: 6.7.5\n") &&
            QFile::copy(GuestToolsBuilder::guestDir() + "/components.conf",
                        places.guestDir + "/components.conf");
        if (!ok) {
            return {};
        }
        auto *stack = new StackBuilder(&m);
        stack->setHostDir(places.hostDir);
        stack->setStackDir(places.stackDir);
        stack->setWorkDir(dir + "/work");
        stack->setJobs(2);
        auto *tools = new GuestToolsBuilder(&m);
        tools->setGuestDir(places.guestDir);
        tools->setRpmsDir(places.rpmsDir);
        tools->setCacheDir(dir + "/cache");
        tools->setMediumImage(places.mediumImage);
        tools->setMeminfo(m_tmp.filePath("meminfo"));
        tools->setMemoryPoll(50);
        m.setPlaces(places);
        m.setBuilders(stack, tools);
        m.setHistoryFile(dir + "/history.json");
        m.setLogDir(dir + "/logs");
        return places;
    }

    /* Builds @ids with @m to its end: the error, "timeout" if it did not end */
    static QString build(BuildManager &m, const QStringList &ids, int timeout = 30000)
    {
        QSignalSpy finished(&m, &BuildManager::runFinished);
        m.build(ids);
        if (finished.isEmpty() && !finished.wait(timeout)) {
            return "timeout";
        }
        return finished[0][0].toString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        qunsetenv("MEMORY");
        /* what the guest tools' build needs, whatever this host has */
        const QString bin = m_tmp.filePath("bin");
        for (const char *tool : {"podman", "mkfs.fat", "mcopy", "rpm"}) {
            QVERIFY(write(bin + '/' + tool, "#!/bin/sh\nexit 0\n", true));
        }
        m_path = qEnvironmentVariable("PATH");
        qputenv("PATH", (bin + ':' + m_path).toUtf8());
        QVERIFY(write(m_tmp.filePath("meminfo"),
                      "MemTotal:       67108864 kB\nMemFree:          100000 kB\n"
                      "MemAvailable:   60000000 kB\nSwapFree:       1048576 kB\n"));
    }

    void cleanup()
    {
        for (const char *name : {"HOST_HOLD", "QEMU_HOLD", "HOST_FAIL", "GUEST_FAIL",
                                 "GUEST_RESTORE"}) {
            qunsetenv(name);
        }
        TaskList::instance()->clearFinished();
    }

    /* What a build of some takes: what they need that is not built, the
       medium after a package, in build order */
    void plans()
    {
        BuildManager m;
        const Builds::Places places = setUp(m, "plan");
        QVERIFY(!places.hostDir.isEmpty());
        const QStringList all = {"virglrenderer", "rutabaga_gfx", "qemu",      "tools", "mesa",
                                 "kwin",          "proxy",        "kpipewire", "medium"};

        QCOMPARE(m.components().size(), 9);
        QCOMPARE(m.plan({"qemu"}), QStringList({"virglrenderer", "rutabaga_gfx", "qemu"}));
        QCOMPARE(m.plan({"virglrenderer"}), QStringList({"virglrenderer"}));
        QCOMPARE(m.plan({"rutabaga_gfx"}), QStringList({"virglrenderer", "rutabaga_gfx"}));
        QCOMPARE(m.plan({"mesa"}), QStringList({"mesa", "medium"}));
        QCOMPARE(m.plan({"medium"}),
                 QStringList({"tools", "mesa", "kwin", "proxy", "kpipewire", "medium"}));
        QCOMPARE(m.plan({"kwin", "virglrenderer"}),
                 QStringList({"virglrenderer", "kwin", "medium"}));
        QCOMPARE(m.outdated(), all);
        QVERIFY(m.plan({"nothing"}).isEmpty());

        /* virglrenderer built from its sources: what is built against it */
        const QList<Builds::Component> components = Builds::load(places);
        const QString vkey = Builds::key(Builds::find(components, "virglrenderer"), components);
        QVERIFY(write(places.stackDir + "/virglrenderer/" + vkey.left(16) +
                          "/share/vitrine/virglrenderer.conf",
                      "KEY=" + vkey.toUtf8() + '\n'));
        m.refresh();
        QCOMPARE(m.status("virglrenderer").state, Builds::Status::State::UpToDate);
        QCOMPARE(m.plan({"qemu"}), QStringList({"rutabaga_gfx", "qemu"}));
        QCOMPARE(m.outdated(), all.mid(1));
    }

    /* A run, component by component: their states, logs and durations, one task */
    void runs()
    {
        BuildManager m;
        const Builds::Places places = setUp(m, "run");
        QStringList states;
        connect(&m, &BuildManager::progressChanged, this, [&m, &states](const QString &id) {
            const QString entry = id + ' ' + QString::number(int(m.progress(id).state));
            if (states.isEmpty() || states.last() != entry) {
                states << entry;
            }
        });
        QSignalSpy started(&m, &BuildManager::runStarted);
        QSignalSpy status(&m, &BuildManager::statusChanged);
        QStringList titles;
        connect(TaskList::instance(), &TaskList::taskChanged, this, [&titles](Task *task) {
            if (titles.isEmpty() || titles.last() != task->title()) {
                titles << task->title();
            }
        });

        QCOMPARE(build(m, m.outdated()), "");
        disconnect(TaskList::instance(), &TaskList::taskChanged, this, nullptr);
        QCOMPARE(started.size(), 1);
        QVERIFY(!m.isRunning());
        QVERIFY(m.error().isEmpty());
        QVERIFY(!status.isEmpty());
        for (const QString &id : m.queue()) {
            QCOMPARE(m.progress(id).state, State::Built);
            /* each through its states, in order */
            const qsizetype queued = states.indexOf(id + ' ' + QString::number(int(State::Queued)));
            const qsizetype building = states.indexOf(id + ' ' + QString::number(int(State::Building)));
            const qsizetype built = states.indexOf(id + ' ' + QString::number(int(State::Built)));
            QVERIFY2(queued >= 0 && building > queued && built > building,
                     qPrintable(id + ": " + states.join(", ")));
            /* how long it took */
            QCOMPARE(m.durations(id).size(), 1);
        }
        QCOMPARE(m.queue(), QStringList({"virglrenderer", "rutabaga_gfx", "qemu", "tools", "mesa",
                                         "kwin", "proxy", "kpipewire", "medium"}));
        /* the components one after the other, each script once */
        QCOMPARE(read(places.hostDir + "/calls"), "qemu\n");
        QCOMPARE(read(places.guestDir + "/rpms-calls"), "tools\nmesa\nkwin\nproxy\nkpipewire\n");
        QCOMPARE(read(places.guestDir + "/medium-calls"), "medium\n");

        /* each its log, kept */
        QVERIFY(m.log("virglrenderer").contains("[2/2] cc b.c"));
        QVERIFY(!m.log("virglrenderer").contains("link qemu-system-x86_64"));
        QVERIFY(m.log("rutabaga_gfx").contains("[1/1] rustc lib.rs"));
        QVERIFY(!m.log("rutabaga_gfx").contains("cc b.c"));
        QVERIFY(!m.log("qemu").contains("rustc lib.rs"));
        QVERIFY(m.log("qemu").contains("[4/4] link qemu-system-x86_64"));
        QVERIFY(m.log("mesa").contains("building: mesa"));
        QVERIFY(!m.log("mesa").contains("building: kwin"));
        QVERIFY(m.log("medium").contains("making the repository"));
        QVERIFY(read(m_tmp.filePath("run/logs/qemu.log")).contains("[4/4] link"));
        /* and the durations */
        QVERIFY(read(m_tmp.filePath("run/history.json")).contains("\"medium\""));
        BuildManager other;
        other.setHistoryFile(m_tmp.filePath("run/history.json"));
        QCOMPARE(other.durations("qemu"), m.durations("qemu"));

        /* one task: the component building, then what was built */
        QVERIFY2(titles.contains("Building virglrenderer") && titles.contains("Building QEMU") &&
                     titles.contains("Building Mesa") &&
                     titles.contains("Building Guest tools medium"),
                 qPrintable(titles.join(", ")));
        Task *task = m.task();
        QVERIFY(task);
        QCOMPARE(task->kind(), Task::Kind::Build);
        QCOMPARE(task->state(), Task::State::Done);
        QCOMPARE(task->title(), "9 components built");
        QCOMPARE(task->text(), "Built: virglrenderer, rutabaga_gfx, QEMU, vitrine-guest-tools, "
                               "Mesa, KWin, wl-cross-domain-proxy, KPipeWire, Guest tools medium");
        QCOMPARE(m.focus(), "medium");

        /* the next run's task replaces it */
        const QPointer<Task> first(task);
        QCOMPARE(build(m, {"virglrenderer"}), "");
        QTRY_VERIFY(!first);
        QCOMPARE(m.task()->title(), "virglrenderer built");
        QCOMPARE(read(places.hostDir + "/calls"), "qemu\nvirglrenderer\n");
    }

    /* A component that fails: the rest is not built, the run's task says why */
    void failure()
    {
        BuildManager m;
        setUp(m, "fail");
        qputenv("GUEST_FAIL", "mesa");

        const QString error = build(m, {"medium"});
        QCOMPARE(error, "Mesa: Building mesa failed (status 1)");
        QCOMPARE(m.error(), error);
        QCOMPARE(m.progress("tools").state, State::Built);
        QCOMPARE(m.progress("mesa").state, State::Failed);
        QCOMPARE(m.progress("mesa").error, "Building mesa failed (status 1)");
        QCOMPARE(m.progress("kwin").state, State::Skipped);
        QCOMPARE(m.progress("kpipewire").state, State::Skipped);
        QCOMPARE(m.progress("medium").state, State::Skipped);
        QVERIFY(m.durations("mesa").isEmpty());
        QCOMPARE(m.focus(), "mesa");
        Task *task = m.task();
        QCOMPARE(task->state(), Task::State::Failed);
        QCOMPARE(task->title(), "Building Mesa");
        QCOMPARE(task->text(), "Building mesa failed (status 1)");
        QVERIFY(m.log("mesa").contains("error: building mesa failed"));

        /* the host's: the same */
        qputenv("HOST_FAIL", "qemu");
        QCOMPARE(build(m, {"qemu"}), "QEMU: Compiling QEMU failed (status 2)");
        QCOMPARE(m.progress("virglrenderer").state, State::Built);
        QCOMPARE(m.progress("rutabaga_gfx").state, State::Built);
        QCOMPARE(m.progress("qemu").state, State::Failed);
        /* the window shows what failed */
        QCOMPARE(m.focus(), "qemu");
    }

    /* Stop: the component building stops, those after it do not start */
    void stop()
    {
        BuildManager m;
        const Builds::Places places = setUp(m, "stop");
        QSignalSpy finished(&m, &BuildManager::runFinished);
        qputenv("HOST_HOLD", m_tmp.filePath("stop/never").toUtf8());

        m.build({"qemu", "tools"});
        QVERIFY(m.isRunning());
        QTRY_COMPARE_WITH_TIMEOUT(m.progress("virglrenderer").state, State::Building, 10000);
        QCOMPARE(m.current(), "virglrenderer");
        QCOMPARE(m.progress("virglrenderer").phase, "building virglrenderer");
        QCOMPARE(m.progress("qemu").state, State::Queued);
        m.cancel();
        QVERIFY(finished.wait(15000));
        QCOMPARE(finished[0][0].toString(), "Stopped");
        QVERIFY(m.error().isEmpty());
        for (const char *id : {"virglrenderer", "rutabaga_gfx", "qemu", "tools", "medium"}) {
            QCOMPARE(m.progress(id).state, State::Stopped);
        }
        QVERIFY(!QFileInfo::exists(places.guestDir + "/rpms-calls"));
        QCOMPARE(m.task()->state(), Task::State::Stopped);
        QVERIFY(m.durations("virglrenderer").isEmpty());
    }

    /* How far the run is, in its task: the components done, and the
       compiler's count of the one compiling; never back */
    void taskProgress()
    {
        BuildManager m;
        setUp(m, "count");
        QVERIFY(write(m_tmp.filePath("count/history.json"),
                      R"({"qemu": {"durations": [60000, 120000, 90000]}})"));
        m.setHistoryFile(m_tmp.filePath("count/history.json"));

        /* virglrenderer and rutabaga_gfx built, QEMU half compiled: 2.5 of 3 */
        qputenv("QEMU_HOLD", m_tmp.filePath("count/go").toUtf8());
        QSignalSpy finished(&m, &BuildManager::runFinished);
        m.build({"qemu"});
        QTRY_VERIFY_WITH_TIMEOUT(m.progress("qemu").done == 2, 10000);
        const BuildManager::Progress p = m.progress("qemu");
        QCOMPARE(p.state, State::Building);
        QCOMPARE(p.total, 4);
        QCOMPARE(m.task()->text(), "3 of 3: compiling QEMU");
        QCOMPARE(m.task()->title(), "Building QEMU");
        QCOMPARE(m.task()->done(), 833);
        QCOMPARE(m.task()->total(), 1000);
        QVERIFY(write(m_tmp.filePath("count/go"), "\n"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][0].toString(), "");
        /* one more duration of each */
        QCOMPARE(m.durations("qemu").size(), 4);
        QCOMPARE(m.durations("virglrenderer").size(), 1);
        QCOMPARE(m.durations("rutabaga_gfx").size(), 1);

        /* a package installs after it compiled, counting nothing: the
           run's progress stays */
        QList<qint64> shown;
        finished.clear();
        m.build({"mesa"});
        connect(m.task(), &Task::changed, this,
                [&shown, task = m.task()]() { shown << task->done(); });
        QVERIFY(finished.wait(30000));
        QCOMPARE(finished[0][0].toString(), "");
        QStringList values;
        for (const qint64 value : std::as_const(shown)) {
            values << QString::number(value);
        }
        QVERIFY2(shown.contains(500) && std::is_sorted(shown.begin(), shown.end()),
                 qPrintable(values.join(", ")));
    }

    /* A package brought back from its build before took no time to build */
    void restored()
    {
        BuildManager m;
        setUp(m, "restore");
        qputenv("GUEST_RESTORE", "tools");

        QCOMPARE(build(m, {"tools"}), "");
        QCOMPARE(m.progress("tools").state, State::Restored);
        QVERIFY(m.durations("tools").isEmpty());
        QCOMPARE(m.progress("medium").state, State::Built);
        QCOMPARE(m.task()->title(), "vitrine-guest-tools and Guest tools medium built");
    }

    /* The cache goes, but not under a build, from a terminal either */
    void clearsTheCache()
    {
        BuildManager m;
        const Builds::Places places = setUp(m, "cache");
        const QString one = m_tmp.filePath("cache/one");
        const QString two = m_tmp.filePath("cache/two");
        m.setCacheDirs({one, two});
        QCOMPARE(m.cacheDirs(), QStringList({one, two}));
        QVERIFY(write(one + "/src/qemu/a.c", "a\n"));
        QVERIFY(write(one + "/readonly/b", "b\n"));
        QVERIFY(QFile::setPermissions(one + "/readonly", QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QVERIFY(write(two + "/.lock", ""));

        /* a build.sh running in a terminal holds its lock */
        const int fd = ::open(QFile::encodeName(two + "/.lock").constData(), O_WRONLY);
        QVERIFY(fd >= 0);
        QCOMPARE(::flock(fd, LOCK_EX | LOCK_NB), 0);
        QString error;
        QVERIFY(!m.clearCache(&error));
        QVERIFY2(error.contains("from a terminal"), qPrintable(error));
        QVERIFY(QFileInfo::exists(one + "/src/qemu/a.c"));
        ::flock(fd, LOCK_UN);
        ::close(fd);

        /* nor while ours runs */
        qputenv("HOST_HOLD", m_tmp.filePath("cache/go").toUtf8());
        QSignalSpy finished(&m, &BuildManager::runFinished);
        m.build({"virglrenderer"});
        QVERIFY(m.isRunning());
        QVERIFY(!m.clearCache(&error));
        QVERIFY(error.contains("A build is running"));
        QVERIFY(write(m_tmp.filePath("cache/go"), "\n"));
        QVERIFY(finished.wait(10000));

        QVERIFY2(m.clearCache(&error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(one));
        QVERIFY(!QFileInfo::exists(two));
        /* what is built stays */
        QVERIFY(QFileInfo::exists(places.stackDir + "/virglrenderer"));
        /* nothing there: nothing to do */
        QVERIFY(m.clearCache(&error));
    }
};

QTEST_GUILESS_MAIN(TestBuildManager)
#include "test_buildmanager.moc"
