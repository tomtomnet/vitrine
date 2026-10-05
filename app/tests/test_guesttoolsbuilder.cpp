// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCryptographicHash>
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

#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/stackbuilder.h"

using State = GuestToolsBuilder::State;

/* What build-rpms.sh hashes, for the stand-ins */
static const char kInputsHash[] = R"sh(
inputs_hash() {
	(cd "$here" && find "$1" rpm-build-inside.sh -type f -print0 | LC_ALL=C sort -z |
		xargs -0 sha256sum) | sha256sum | cut -d' ' -f1
}
)sh";

/*
 * A stand-in for build-rpms.sh: the lines of a package's build as the real
 * one passes them on (its own, the container's, rpmbuild's, ninja's...),
 * and the package's inputs recorded; GATE=PACKAGE:FILE holds PACKAGE's
 * until FILE is there.  (No line of these scripts starts with a #, nor has
 * a \": moc would take them for a directive or a string's end, and see no
 * class.)
 */
static const char kRpmsStandIn[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
@HASH@
echo "$*" >> "$here/rpms-args"
env | grep -E '^(RPMS_DIR|CACHE_DIR|OUT|FEDORA_RELEASE)=' | sort > "$here/rpms-env"
pkg=${@: -1}
echo "vitrine-build: building: $pkg (log: $RPMS_DIR/$pkg.log)"
echo "vitrine-build: phase: installing rpmbuild"
echo "[ 1/82] emacs-filesystem-1:30.2-2.fc44. 100% | 297.9 KiB/s |   8.0 KiB |  00m00s"
echo "[82/82] rpmdevtools-0:9.6-14.fc44.noarc 100% |   1.6 MiB/s |  94.4 KiB |  00m00s"
echo "vitrine-build: phase: downloading the sources"
echo "[3/9] not a count of this phase"
echo "vitrine-build: phase: building the packages"
echo "[5/9] nor of this one"
echo "Executing(%prep): /bin/sh -e /var/tmp/rpm-tmp.a"
echo "Executing(%build): /bin/sh -e /var/tmp/rpm-tmp.b"
echo "[1/5808] /usr/bin/python3 ../src/nir_intrinsics_h.py"
echo "[5808/5808] g++ -o src/libvulkan_nouveau.so"
echo "[ 13%] Building CXX object a.o"
echo "[  9%] Building CXX object b.o"
echo "Executing(%install): /bin/sh -e /var/tmp/rpm-tmp.c"
echo "Processing files: $pkg-1.0-1.fc44.x86_64"
echo "Processing files: $pkg-devel-1.0-1.fc44.x86_64"
echo "vitrine-build: warning: a warning of $pkg"
if [ -n "${GATE:-}" ] && [ "$pkg" = "${GATE%%:*}" ]; then
	while [ ! -e "${GATE#*:}" ]; do sleep 0.05; done
fi
mkdir -p "$RPMS_DIR/$pkg"
inputs_hash "$pkg" > "$RPMS_DIR/$pkg/.inputs"
printf 'vitrine-build: built: %s' "$RPMS_DIR/$pkg"
)sh";

/* A stand-in for build-medium.sh: a medium, with the manifest the real one writes */
static const char kMediumStandIn[] = R"sh(#!/bin/bash
here=$(cd "$(dirname "$0")" && pwd)
echo "$*" >> "$here/medium-args"
env | grep -E '^(RPMS_DIR|CACHE_DIR|OUT|FEDORA_RELEASE)=' | sort > "$here/medium-env"
echo "vitrine-build: phase: making the repository"
[ "${FAKE_MEDIUM_FAIL:-0}" = 1 ] && { echo "vitrine-build: error: no room left" >&2; exit 1; }
inputs=
for t in tools mesa kwin; do
	[ -s "$RPMS_DIR/$t/.inputs" ] && inputs+=$(printf '"%s": "%s", ' "$t" "$(cat "$RPMS_DIR/$t/.inputs")")
done
inputs+=$(printf '"medium": "%s"' "$(sha256sum < "$here/build-medium.sh" | cut -d' ' -f1)")
mkdir -p "$(dirname "$OUT")"
echo image > "$OUT"
cat > "${OUT%.img}.json" <<EOF
{"mediumId": "m$RANDOM", "fedora": "44", "tools": "0.1.0-14.fc44", "mesa": "26.2.3-1.xe.fc44",
 "kwin": "6.7.5-1.22.fc44", "built": "2026-10-05T10:00:00Z", "inputs": {$inputs}, "packages": []}
EOF
echo "vitrine-build: built: $OUT"
)sh";

/*
 * A stand-in for podman, for the real build-rpms.sh: `run` plays the
 * container, a process that ignores SIGTERM as bash as a container's init
 * does, and passes the client's SIGTERM on to it as podman does; `rm` and
 * `container exists` find it by its name.  Each call goes to $FAKE_PODMAN/calls.
 */
static const char kPodman[] = R"sh(#!/bin/bash
dir=$FAKE_PODMAN
echo "$*" >> "$dir/calls"
case "$1" in
container)
	[ -f "$dir/$3.pid" ] && kill -0 "$(cat "$dir/$3.pid")" 2> /dev/null
	exit ;;
rm)
	name=${*: -1}
	[ -f "$dir/$name.pid" ] && kill -KILL "$(cat "$dir/$name.pid")" 2> /dev/null
	rm -f "$dir/$name.pid"
	exit 0 ;;
run)
	shift
	while [ $# -gt 0 ]; do
		case "$1" in
		--name) name=$2; shift ;;
		-v) case "$2" in *:/out) out=${2%:/out} ;; esac; shift ;;
		-e|--security-opt) shift ;;
		--memory=*) echo "${1#*=}" > "$dir/memory" ;;
		--rm) ;;
		*) break ;;
		esac
		shift
	done
	package=${*: -1}
	( trap '' TERM INT; exec "$dir/container" "$package" "$out" ) &
	child=$!
	echo "$child" > "$dir/$name.pid"
	trap 'kill -TERM "$child" 2> /dev/null' TERM
	rc=0
	while kill -0 "$child" 2> /dev/null; do
		wait "$child"
		rc=$?
	done
	rm -f "$dir/$name.pid"
	exit "$rc" ;;
esac
exit 0
)sh";

/* What the container does, as $FAKE_CONTAINER says: ok, fail, hang */
static const char kContainer[] = R"sh(#!/bin/bash
package=$1
out=$2
echo "vitrine-build: phase: installing rpmbuild"
echo "[1/2] rpm-build 100%"
echo "vitrine-build: phase: building the packages"
echo "Executing(%build): /bin/sh -e /var/tmp/rpm-tmp.b"
echo "[1/2] cc a.c"
mode=${FAKE_CONTAINER:-ok}
case "$mode" in
fail|fail-$package) echo "error: something broke in $package"; exit 1 ;;
hang|hang-$package) echo $$ > "$FAKE_PODMAN/hang.pid"; exec sleep 300 ;;
esac
echo "[2/2] cc b.c"
touch "$out/$package-1.0-1.fc44.noarch.rpm"
echo "built: 1 packages"
)sh";

class TestGuestToolsBuilder : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_bin;
    QString m_podman;
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

    static QString run(const QString &program, const QStringList &args, int *status = nullptr,
                       const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment())
    {
        QProcess p;
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start(program, args);
        p.waitForFinished(60000);
        if (status) {
            *status = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        }
        return QString::fromUtf8(p.readAll());
    }

    /*
     * A guest/ with specs for the three packages and rpm-build-inside.sh;
     * build-rpms.sh and build-medium.sh the real ones (real) or stand-ins
     */
    QString guestDir(const QString &name, bool real = false)
    {
        const QString dir = m_tmp.filePath(name);
        const QByteArray rpms = QByteArray(kRpmsStandIn).replace("@HASH@", kInputsHash);
        const bool ok =
            write(dir + "/tools/vitrine-guest-tools.spec", "Name: vitrine-guest-tools\nVersion: 0.1.0\n") &&
            write(dir + "/tools/install", "#!/bin/bash\n", true) &&
            write(dir + "/mesa/mesa.spec", "Name: mesa\nVersion:        26.2.3\nRelease: 1\n") &&
            write(dir + "/mesa/mesa-fix.patch", "--- a\n+++ b\n") &&
            write(dir + "/kwin/kwin.spec", "Name: kwin\nVersion: %{kf6_version}\n") &&
            write(dir + "/rpm-build-inside.sh", "#!/bin/bash\n", true) &&
            (real ? QFile::copy(GUEST_SOURCE_DIR "/build-rpms.sh", dir + "/build-rpms.sh")
                  : write(dir + "/build-rpms.sh", rpms, true)) &&
            write(dir + "/build-medium.sh", kMediumStandIn, true);
        return ok ? dir : QString();
    }

    /* A builder of @guest into folders of @name */
    void setUp(GuestToolsBuilder &b, const QString &guest, const QString &name)
    {
        b.setGuestDir(guest);
        b.setRpmsDir(m_tmp.filePath(name + "/rpms"));
        b.setCacheDir(m_tmp.filePath(name + "/cache"));
        b.setMediumImage(m_tmp.filePath(name + "/media/vitrine-guest-tools-fc44.img"));
        b.setMeminfo(m_tmp.filePath("meminfo-plenty"));
        b.setMemoryPoll(50);
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
    static QString build(GuestToolsBuilder &builder, int timeout = 60000)
    {
        QSignalSpy finished(&builder, &GuestToolsBuilder::finished);
        builder.start();
        if (finished.isEmpty() && !finished.wait(timeout)) {
            return "timeout";
        }
        return finished[0][0].toString();
    }

    static QByteArray meminfo(qint64 totalMiB, qint64 availableMiB)
    {
        return QString("MemTotal:       %1 kB\nMemFree:          100000 kB\n"
                       "MemAvailable:   %2 kB\nSwapFree:       1048576 kB\n")
            .arg(totalMiB * 1024)
            .arg(availableMiB * 1024)
            .toUtf8();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        /* the app's data, cache and settings in here */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        qunsetenv("MEMORY");
        qunsetenv("VITRINE_GUEST_DIR");
        /* podman and the medium's tools found, whatever this host has; the
           real build-rpms.sh's podman is the stand-in */
        m_bin = m_tmp.filePath("bin");
        m_podman = m_tmp.filePath("podman");
        QVERIFY(write(m_bin + "/podman", kPodman, true));
        for (const char *tool : {"mkfs.fat", "mcopy", "rpm"}) {
            QVERIFY(write(m_bin + '/' + tool, "#!/bin/sh\nexit 0\n", true));
        }
        QVERIFY(write(m_podman + "/container", kContainer, true));
        qputenv("FAKE_PODMAN", m_podman.toUtf8());
        m_path = qEnvironmentVariable("PATH");
        qputenv("PATH", (m_bin + ':' + m_path).toUtf8());
        QVERIFY(write(m_tmp.filePath("meminfo-plenty"), meminfo(65536, 60000)));
    }

    void cleanup()
    {
        qputenv("PATH", (m_bin + ':' + m_path).toUtf8());
        qunsetenv("GATE");
        qunsetenv("FAKE_CONTAINER");
        qunsetenv("FAKE_MEDIUM_FAIL");
        qunsetenv("MEMORY");
    }

    void parsesLines()
    {
        using Line = GuestToolsBuilder::Line;
        Line l = Line::parse("vitrine-build: phase: installing the build dependencies\n");
        QCOMPARE(l.kind, Line::Phase);
        QCOMPARE(l.text, "installing the build dependencies");
        QCOMPARE(Line::parse("vitrine-build: building: mesa (log: /x/mesa.log)").kind,
                 Line::Building);
        QCOMPARE(Line::parse("vitrine-build: up to date: /x/tools").text, "/x/tools");
        QCOMPARE(Line::parse("vitrine-build: built: /x/tools").kind, Line::Built);
        QCOMPARE(Line::parse("vitrine-build: missing: podman").text, "podman");
        QCOMPARE(Line::parse("vitrine-build: install: sudo dnf install podman").kind,
                 Line::Install);
        QCOMPARE(Line::parse("vitrine-build: warning: no kwin packages").kind, Line::Warning);
        l = Line::parse("vitrine-build: error: building mesa failed (status 1): see /x");
        QCOMPARE(l.kind, Line::Error);
        QCOMPARE(l.text, "building mesa failed (status 1): see /x");
        QCOMPARE(Line::parse("vitrine-build: something new").kind, Line::Other);

        l = Line::parse("[1234/5808] g++ -o a.o");
        QCOMPARE(l.kind, Line::Counted);
        QCOMPARE(l.done, 1234);
        QCOMPARE(l.total, 5808);
        l = Line::parse("[ 7/82] cpio-0:2.15-9.fc44.x86_64       100% |   6.8 MiB/s");
        QCOMPARE(l.kind, Line::Counted);
        QCOMPARE(l.done, 7);
        l = Line::parse("[ 45%] Building CXX object src/x.cpp.o");
        QCOMPARE(l.kind, Line::Percent);
        QCOMPARE(l.done, 45);
        QCOMPARE(l.total, 100);
        l = Line::parse("Executing(%build): /bin/sh -e /var/tmp/rpm-tmp.Nr86Pm");
        QCOMPARE(l.kind, Line::Section);
        QCOMPARE(l.text, "build");
        QCOMPARE(Line::parse("Processing files: mesa-libGL-26.2.3-1.xe.fc44.x86_64").text, "files");
        QCOMPARE(Line::parse("Executing(rmbuild): /bin/sh -e x").kind, Line::Other);
        QCOMPARE(Line::parse("+ /usr/bin/meson compile -C redhat-linux-build").kind, Line::Other);
    }

    void parsesMadeAndMemory()
    {
        const GuestToolsBuilder::Made made = GuestToolsBuilder::Made::parse(
            R"({"mediumId": "abc", "tools": "0.1.0-14.fc44", "built": "2026-10-05T16:30:00Z",
                "inputs": {"tools": "t1", "mesa": "m1", "medium": "x1"}})");
        QCOMPARE(made.built, QDateTime(QDate(2026, 10, 5), QTime(16, 30), QTimeZone::utc()));
        QCOMPARE(made.inputs.size(), 3);
        QCOMPARE(made.inputs.value("mesa"), "m1");
        /* an older medium's: none */
        QVERIFY(GuestToolsBuilder::Made::parse(R"({"mediumId": "abc"})").inputs.isEmpty());

        QCOMPARE(GuestToolsBuilder::memoryCapMiB(), 10240);
        const QList<std::pair<const char *, qint64>> caps = {
            {"4g", 4096}, {"6G", 6144}, {"3000m", 3000}, {"2097152k", 2048},
            {"1073741824", 1024}, {"512M", 512}, {"lots", 10240}, {"0g", 10240},
        };
        for (const auto &[value, mib] : caps) {
            qputenv("MEMORY", value);
            QCOMPARE(GuestToolsBuilder::memoryCapMiB(), mib);
        }
    }

    /* The hash is build-rpms.sh's own, in any locale: the scripts and the app agree */
    void inputsHashIsTheScripts()
    {
        const QString odd = m_tmp.filePath("odd");
        QVERIFY(QDir().mkpath(odd));
        QVERIFY(QFile::copy(GUEST_SOURCE_DIR "/build-rpms.sh", odd + "/build-rpms.sh"));
        /* names whose order differs between the C locale and en_US's */
        QVERIFY(write(odd + "/rpm-build-inside.sh", "inside\n"));
        QVERIFY(write(odd + "/kwin/kwin.spec", "spec\n"));
        QVERIFY(write(odd + "/kwin/kwin-watch-gpu.patch", "a\n"));
        QVERIFY(write(odd + "/kwin/Z-upper.patch", "b\n"));
        QVERIFY(write(odd + "/kwin/.hidden", "c\n"));
        QVERIFY(write(odd + "/kwin/sub/dir/deep", "d\n"));
        QVERIFY(write(odd + "/kwin/.dot/inner", "e\n"));
        QVERIFY(QFile::link("kwin.spec", odd + "/kwin/link-to-spec"));
        QVERIFY(QDir().mkpath(odd + "/kwin/empty"));
        QVERIFY(write(odd + "/tools/vitrine-guest-tools.spec", "t\n"));

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("LC_ALL");
        env.insert("LANG", "en_US.UTF-8");
        for (const QString &guest : {QString(GUEST_SOURCE_DIR), odd}) {
            int status = -1;
            const QString printed = run("bash", {guest + "/build-rpms.sh", "--print-inputs",
                                                 "tools", "kwin"},
                                        &status, env);
            QCOMPARE(status, 0);
            for (const QString &line : printed.split('\n', Qt::SkipEmptyParts)) {
                const QString package = line.section(' ', 0, 0);
                QVERIFY2(!GuestToolsBuilder::inputsHash(guest, package).isEmpty(),
                         qPrintable(package));
                QCOMPARE(GuestToolsBuilder::inputsHash(guest, package), line.section(' ', 1));
            }
        }
        /* each file counts, links do not */
        const QString before = GuestToolsBuilder::inputsHash(odd, "kwin");
        QVERIFY(QFile::remove(odd + "/kwin/link-to-spec"));
        QCOMPARE(GuestToolsBuilder::inputsHash(odd, "kwin"), before);
        QVERIFY(write(odd + "/kwin/.dot/inner", "changed\n"));
        QVERIFY(GuestToolsBuilder::inputsHash(odd, "kwin") != before);
        QVERIFY(GuestToolsBuilder::inputsHash(odd, "mesa").isEmpty());

        QCOMPARE(GuestToolsBuilder::mediumHash(GUEST_SOURCE_DIR),
                 QString(QCryptographicHash::hash(read(GUEST_SOURCE_DIR "/build-medium.sh"),
                                                  QCryptographicHash::Sha256)
                             .toHex()));
        QCOMPARE(GuestToolsBuilder::version(GUEST_SOURCE_DIR, "kwin"), "6.7.5");
        QCOMPARE(GuestToolsBuilder::packages(GUEST_SOURCE_DIR),
                 QStringList({"tools", "mesa", "kwin"}));
    }

    /* Out of date by the inputs the medium records, as the scripts tell them */
    void states()
    {
        const QString guest = guestDir("state-guest");
        const QString image = m_tmp.filePath("state/vitrine-guest-tools-fc44.img");
        const QString manifest = m_tmp.filePath("state/vitrine-guest-tools-fc44.json");
        const auto inputs = [&guest](const QString &medium) {
            return QString(R"("inputs": {"tools": "%1", "mesa": "%2", "kwin": "%3", "medium": "%4"})")
                .arg(GuestToolsBuilder::inputsHash(guest, "tools"),
                     GuestToolsBuilder::inputsHash(guest, "mesa"),
                     GuestToolsBuilder::inputsHash(guest, "kwin"), medium);
        };
        const QString medium = GuestToolsBuilder::mediumHash(guest);

        QCOMPARE(GuestToolsBuilder::state(m_tmp.filePath("nowhere"), image), State::NoSources);
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::NotBuilt);
        QVERIFY(write(image, "image\n"));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::NotBuilt);
        /* an older Vitrine's: no inputs */
        QVERIFY(write(manifest, R"({"mediumId": "a", "tools": "0.1.0-14.fc44"})"));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::Outdated);
        QVERIFY(write(manifest, QString(R"({"mediumId": "a", "tools": "0.1.0-14.fc44", %1})")
                                    .arg(inputs(medium)).toUtf8()));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::UpToDate);
        /* a patch, or the script that makes the medium */
        QVERIFY(write(guest + "/mesa/mesa-fix.patch", "--- a\n+++ c\n"));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::Outdated);
        QVERIFY(write(manifest, QString(R"({"mediumId": "a", "tools": "0.1.0-14.fc44", %1})")
                                    .arg(inputs("other")).toUtf8()));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::Outdated);
        QVERIFY(write(manifest, QString(R"({"mediumId": "a", "tools": "0.1.0-14.fc44", %1})")
                                    .arg(inputs(medium)).toUtf8()));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::UpToDate);
        /* a package the medium lacks */
        QVERIFY(write(manifest, QString(R"({"mediumId": "a", "tools": "0.1.0-14.fc44",
                                           "inputs": {"tools": "%1", "medium": "%2"}})")
                                    .arg(GuestToolsBuilder::inputsHash(guest, "tools"), medium)
                                    .toUtf8()));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::Outdated);
        /* the app's own: in its data folder */
        QCOMPARE(GuestToolsBuilder::mediumImage(), GuestTools::dataDir() +
                                                       "/vitrine-guest-tools-fc44.img");
        QVERIFY(GuestToolsBuilder::rpmsDir().startsWith(m_tmp.filePath("data/vitrine/")));
        QVERIFY(GuestToolsBuilder::cacheDir().startsWith(m_tmp.filePath("cache/vitrine/")));
    }

    /* The packages not built from these sources, then the medium */
    void plans()
    {
        const QString guest = guestDir("plan-guest");
        const QString rpms = m_tmp.filePath("plan-rpms");
        const auto names = [&]() {
            QStringList list;
            for (const GuestToolsBuilder::Step &s : GuestToolsBuilder::plan(guest, rpms)) {
                list << s.name + (s.heavy ? "*" : "");
            }
            return list;
        };

        QCOMPARE(names(), QStringList({"tools", "mesa*", "kwin*", "medium"}));
        const QList<GuestToolsBuilder::Step> steps = GuestToolsBuilder::plan(guest, rpms);
        QCOMPARE(steps[0].text, "building vitrine-guest-tools 0.1.0");
        QCOMPARE(steps[1].text, "building Mesa 26.2.3");
        /* a macro: no version */
        QCOMPARE(steps[2].text, "building KWin");
        QCOMPARE(steps[3].text, "making the medium");

        QVERIFY(write(rpms + "/tools/.inputs",
                      GuestToolsBuilder::inputsHash(guest, "tools").toUtf8() + '\n'));
        QVERIFY(write(rpms + "/kwin/.inputs", "an older one\n"));
        QCOMPARE(names(), QStringList({"mesa*", "kwin*", "medium"}));
        QVERIFY(write(rpms + "/mesa/.inputs", GuestToolsBuilder::inputsHash(guest, "mesa").toUtf8()));
        QVERIFY(write(rpms + "/kwin/.inputs", GuestToolsBuilder::inputsHash(guest, "kwin").toUtf8()));
        QCOMPARE(names(), QStringList({"medium"}));
    }

    /* The steps, their phases and progress from the scripts' lines, and the medium */
    void runsTheScripts()
    {
        const QString guest = guestDir("run-guest");
        GuestToolsBuilder b;
        setUp(b, guest, "run");
        QSignalSpy started(&b, &GuestToolsBuilder::started);
        QSignalSpy steps(&b, &GuestToolsBuilder::stepStarted);
        QSignalSpy phases(&b, &GuestToolsBuilder::phaseChanged);
        QSignalSpy progress(&b, &GuestToolsBuilder::progress);
        QSignalSpy output(&b, &GuestToolsBuilder::output);
        QSignalSpy built(&b, &GuestToolsBuilder::built);

        QCOMPARE(build(b), "");
        QCOMPARE(started.size(), 1);
        QCOMPARE(built.size(), 1);
        QVERIFY(!b.isRunning());
        QVERIFY(!b.wasStopped());
        QVERIFY(b.error().isEmpty());

        QCOMPARE(steps.size(), 4);
        for (int i = 0; i < 4; i++) {
            QCOMPARE(steps[i][0].toInt(), i + 1);
            QCOMPARE(steps[i][1].toInt(), 4);
        }
        QCOMPARE(steps[1][2].toString(), "building Mesa 26.2.3");
        QCOMPARE(steps[3][2].toString(), "making the medium");
        /* each package by itself, the container's output passed on */
        QCOMPARE(read(guest + "/rpms-args"), "--verbose tools\n--verbose mesa\n--verbose kwin\n");
        QCOMPARE(read(guest + "/medium-args"), "\n");
        const QByteArray env = QString("CACHE_DIR=%1\nFEDORA_RELEASE=44\nOUT=%2\nRPMS_DIR=%3\n")
                                   .arg(m_tmp.filePath("run/cache"),
                                        m_tmp.filePath("run/media/vitrine-guest-tools-fc44.img"),
                                        m_tmp.filePath("run/rpms"))
                                   .toUtf8();
        QCOMPARE(read(guest + "/rpms-env"), env);
        QCOMPARE(read(guest + "/medium-env"), env);

        /* the phases of a package's build: the container's, then rpmbuild's */
        QStringList named;
        for (const QList<QVariant> &p : phases) {
            if (!p[0].toString().isEmpty() && !named.contains(p[0].toString())) {
                named << p[0].toString();
            }
        }
        QCOMPARE(named, QStringList({"installing rpmbuild", "downloading the sources",
                                     "building the packages", "unpacking and patching the sources",
                                     "compiling", "installing", "making the packages",
                                     "making the repository"}));
        /* counts while dnf installs and while it compiles, none else */
        QList<std::pair<int, int>> counts;
        for (const QList<QVariant> &p : progress) {
            if (p[1].toInt() > 0 && !counts.contains({p[0].toInt(), p[1].toInt()})) {
                counts.append({p[0].toInt(), p[1].toInt()});
            }
        }
        QCOMPARE(counts, (QList<std::pair<int, int>>{{1, 82}, {82, 82}, {1, 5808}, {5808, 5808},
                                                     {13, 100}}));
        QCOMPARE(b.warnings(), QStringList({"a warning of tools", "a warning of mesa",
                                            "a warning of kwin"}));
        QVERIFY(log(output).contains("[5808/5808] g++"));
        QVERIFY(b.log().contains("bash " + guest + "/build-medium.sh"));

        /* a medium of these sources: up to date, nothing left to build but the medium */
        const QString image = m_tmp.filePath("run/media/vitrine-guest-tools-fc44.img");
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::UpToDate);
        QCOMPARE(GuestToolsBuilder::plan(guest, m_tmp.filePath("run/rpms")).size(), 1);
        QCOMPARE(GuestToolsBuilder::made(image).built.date(), QDate(2026, 10, 5));

        /* again: the medium alone */
        steps.clear();
        QCOMPARE(build(b), "");
        QCOMPARE(steps.size(), 1);
        QCOMPARE(read(guest + "/rpms-args").count('\n'), 3);
    }

    /* A package that fails: its error, the last good packages and medium kept */
    void failureKeepsTheLastGoodMedium()
    {
        const QString guest = guestDir("fail-guest", true);
        GuestToolsBuilder b;
        setUp(b, guest, "fail");
        const QString rpms = m_tmp.filePath("fail/rpms");
        const QString image = m_tmp.filePath("fail/media/vitrine-guest-tools-fc44.img");
        QSignalSpy built(&b, &GuestToolsBuilder::built);

        /* a medium and a Mesa of older sources */
        QVERIFY(write(image, "the last good medium\n"));
        QVERIFY(write(GuestToolsBuilder::manifestOf(image), R"({"mediumId": "good", "tools": "1"})"));
        QVERIFY(write(rpms + "/mesa/mesa-1.rpm", "good\n"));
        QVERIFY(write(rpms + "/mesa/.inputs", "older\n"));

        qputenv("FAKE_CONTAINER", "fail-mesa");
        const QString error = build(b);
        QCOMPARE(error, "Building mesa failed (status 1)");
        QCOMPARE(b.error(), error);
        QVERIFY(!b.wasStopped());
        QVERIFY(built.isEmpty());
        /* tools built and kept; Mesa as it was, nothing of the failed one */
        QVERIFY(QFileInfo::exists(rpms + "/tools/tools-1.0-1.fc44.noarch.rpm"));
        QCOMPARE(read(rpms + "/tools/.inputs").trimmed(),
                 GuestToolsBuilder::inputsHash(guest, "tools").toUtf8());
        QCOMPARE(read(rpms + "/mesa/mesa-1.rpm"), "good\n");
        QCOMPARE(read(rpms + "/mesa/.inputs"), "older\n");
        QVERIFY(!QFileInfo::exists(rpms + "/.mesa.new"));
        QVERIFY(read(rpms + "/mesa.log").contains("something broke in mesa"));
        /* the medium not made again */
        QCOMPARE(read(image), "the last good medium\n");
        QVERIFY(!QFileInfo::exists(guest + "/medium-args"));
        QCOMPARE(GuestToolsBuilder::state(guest, image), State::Outdated);
        /* the container's memory as the scripts' default */
        QCOMPARE(read(m_podman + "/memory").trimmed(), "10g");

        /* the medium's failure: the last good one kept too */
        qunsetenv("FAKE_CONTAINER");
        qputenv("FAKE_MEDIUM_FAIL", "1");
        QCOMPARE(build(b), "No room left");
        QCOMPARE(read(image), "the last good medium\n");
        QVERIFY(built.isEmpty());
    }

    /* Stop: the script's container goes, and its partial output; the last good build stays */
    void stopRemovesTheContainer()
    {
        const QString guest = guestDir("stop-guest", true);
        const QString rpms = m_tmp.filePath("stop/rpms");
        const QString hang = m_podman + "/hang.pid";
        GuestToolsBuilder b;
        setUp(b, guest, "stop");
        QSignalSpy finished(&b, &GuestToolsBuilder::finished);

        QVERIFY(write(rpms + "/tools/tools-0.rpm", "good\n"));
        QFile::remove(hang);
        QFile::remove(m_podman + "/calls");
        qputenv("FAKE_CONTAINER", "hang");
        b.start();
        QVERIFY(b.isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(hang).size() > 0, 20000);
        const pid_t container = pid_t(read(hang).trimmed().toLongLong());
        QVERIFY(container > 0);
        QCOMPARE(::kill(container, 0), 0);
        QVERIFY(QFileInfo::exists(rpms + "/.tools.new"));

        b.cancel();
        QVERIFY(finished.wait(15000));
        QCOMPARE(finished[0][0].toString(), "Stopped");
        QVERIFY(b.wasStopped());
        QVERIFY(b.error().isEmpty());
        QVERIFY(!b.isRunning());
        /* the container, which ignored the SIGTERM, removed by its name */
        QTRY_VERIFY_WITH_TIMEOUT(::kill(container, 0) != 0 && errno == ESRCH, 3000);
        const QString tag = QString::fromLatin1(
            QCryptographicHash::hash(rpms.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
        QVERIFY2(read(m_podman + "/calls").contains("rm -f -t 0 vitrine-rpm-tools-fc44-" +
                                                     tag.toUtf8()),
                 read(m_podman + "/calls").constData());
        QVERIFY(!QFileInfo::exists(rpms + "/.tools.new"));
        QCOMPARE(read(rpms + "/tools/tools-0.rpm"), "good\n");
        QVERIFY(!QFileInfo::exists(guest + "/medium-args"));
    }

    /* vitrine killed while it builds: the script stops, and removes its container */
    void stopsWhenVitrineDies()
    {
        const QString guest = guestDir("dies-guest", true);
        const QString rpms = m_tmp.filePath("dies/rpms");
        const QString hang = m_podman + "/hang.pid";
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QProcess vitrine;

        QFile::remove(hang);
        QFile::remove(m_podman + "/calls");
        env.insert("BUILDER_CHILD", guest);
        env.insert("BUILDER_RPMS", rpms);
        env.insert("FAKE_CONTAINER", "hang");
        vitrine.setProcessEnvironment(env);
        vitrine.start(QCoreApplication::applicationFilePath(), {});
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(hang).size() > 0, 20000);
        const pid_t container = pid_t(read(hang).trimmed().toLongLong());
        QCOMPARE(::kill(container, 0), 0);

        vitrine.kill();
        QVERIFY(vitrine.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(::kill(container, 0) != 0 && errno == ESRCH, 10000);
        QVERIFY(read(m_podman + "/calls").contains("rm -f -t 0 vitrine-rpm-tools-fc44-"));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(rpms + "/.tools.new"), 5000);
    }

    /* Stop ends what the script runs too, not the script alone */
    void stopsTheProcessGroup()
    {
        const QString guest = guestDir("group-guest");
        const QString pidFile = m_tmp.filePath("group-pid");
        QVERIFY(write(guest + "/build-rpms.sh",
                      QString("#!/bin/bash\nsh -c 'echo $$ > %1; exec sleep 300'\n")
                          .arg(pidFile).toUtf8(),
                      true));
        GuestToolsBuilder b;
        setUp(b, guest, "group");
        QSignalSpy finished(&b, &GuestToolsBuilder::finished);

        b.start();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 10000);
        const pid_t sleeper = pid_t(read(pidFile).trimmed().toLongLong());
        QCOMPARE(::kill(sleeper, 0), 0);
        b.cancel();
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][0].toString(), "Stopped");
        QTRY_VERIFY_WITH_TIMEOUT(::kill(sleeper, 0) != 0 && errno == ESRCH, 10000);
    }

    /* One build at a time: in the app, by the scripts' lock, and not with QEMU's */
    void oneAtATime()
    {
        const QString guest = guestDir("one-guest");
        const QString pidFile = m_tmp.filePath("one-pid");
        QVERIFY(write(guest + "/build-rpms.sh",
                      QString("#!/bin/bash\necho \"$*\" >> \"$(dirname \"$0\")/rpms-args\"\n"
                              "sh -c 'echo $$ > %1; exec sleep 300'\n")
                          .arg(pidFile).toUtf8(),
                      true));
        GuestToolsBuilder *b = GuestToolsBuilder::instance();
        setUp(*b, guest, "one");
        QSignalSpy started(b, &GuestToolsBuilder::started);
        QSignalSpy finished(b, &GuestToolsBuilder::finished);

        b->start();
        b->start();
        QCOMPARE(started.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 10000);
        QCOMPARE(read(guest + "/rpms-args"), "--verbose tools\n");

        /* QEMU's waits: both take many GiB */
        StackBuilder stack;
        QSignalSpy stackFinished(&stack, &StackBuilder::finished);
        stack.setHostDir(m_tmp.filePath("one-host"));
        QVERIFY(write(m_tmp.filePath("one-host/build.sh"), "#!/bin/bash\nexit 0\n", true));
        QVERIFY(write(m_tmp.filePath("one-host/versions.conf"), "QEMU_URL=a\n"));
        stack.start();
        QCOMPARE(stackFinished.size(), 1);
        QVERIFY(stackFinished[0][0].toString().contains("guest tools are being built"));
        QVERIFY(!stack.isRunning());

        b->cancel();
        QVERIFY(finished.wait(10000));
        QVERIFY(b->wasStopped());

        /* and the other way around */
        QVERIFY(write(m_tmp.filePath("one-host/build.sh"), "#!/bin/bash\nsleep 300\n", true));
        StackBuilder *qemu = StackBuilder::instance();
        qemu->setHostDir(m_tmp.filePath("one-host"));
        qemu->setStackDir(m_tmp.filePath("one-stack"));
        qemu->setWorkDir(m_tmp.filePath("one-work"));
        QSignalSpy qemuFinished(qemu, &StackBuilder::finished);
        qemu->start();
        QVERIFY(qemu->isRunning());
        finished.clear();
        b->start();
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished[0][0].toString().contains("QEMU is being built"));
        QVERIFY(!b->isRunning());
        qemu->cancel();
        QVERIFY(qemuFinished.wait(15000));

        /* two scripts into the same folder: the second one refuses */
        const QString real = guestDir("lock-guest", true);
        const QString rpms = m_tmp.filePath("lock/rpms");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("RPMS_DIR", rpms);
        env.insert("CACHE_DIR", m_tmp.filePath("lock/cache"));
        env.insert("FAKE_CONTAINER", "hang");
        QFile::remove(m_podman + "/hang.pid");
        QProcess first;
        first.setProcessEnvironment(env);
        first.setProcessChannelMode(QProcess::MergedChannels);
        first.setChildProcessModifier([]() { ::setpgid(0, 0); });
        first.start("bash", {real + "/build-rpms.sh", "tools"});
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(m_podman + "/hang.pid").size() > 0, 20000);
        int status = -1;
        const QString second = run("bash", {real + "/build-rpms.sh", "tools"}, &status, env);
        QCOMPARE(status, 1);
        QVERIFY2(second.contains("vitrine-build: error: another build of the guest tools is "
                                 "running into " + rpms),
                 qPrintable(second));
        ::kill(-pid_t(first.processId()), SIGTERM);
        QVERIFY(first.waitForFinished(15000));
        QCOMPARE(first.exitCode(), 143);
        QVERIFY(QString::fromUtf8(first.readAll()).contains("vitrine-build: error: stopped"));
    }

    /* Mesa and KWin wait for their memory, or are told to go on */
    void waitsForMemory()
    {
        const QString guest = guestDir("memory-guest");
        const QString meminfo = m_tmp.filePath("meminfo-short");
        GuestToolsBuilder b;
        setUp(b, guest, "memory");
        b.setMeminfo(meminfo);
        QSignalSpy waiting(&b, &GuestToolsBuilder::waitingChanged);
        QSignalSpy finished(&b, &GuestToolsBuilder::finished);
        QSignalSpy steps(&b, &GuestToolsBuilder::stepStarted);

        /* 4 GiB for each, and the host's 5% of its 20 GiB: 5 GiB free needed */
        qputenv("MEMORY", "4g");
        qputenv("GATE", "mesa:" + m_tmp.filePath("memory-gate").toUtf8());
        QVERIFY(write(meminfo, this->meminfo(20480, 3000)));
        b.start();
        /* tools first, which takes little */
        QTRY_VERIFY_WITH_TIMEOUT(b.isWaiting(), 10000);
        QCOMPARE(b.step(), 2);
        QCOMPARE(read(guest + "/rpms-args"), "--verbose tools\n");
        QCOMPARE(b.availableMiB(), 3000);
        QCOMPARE(b.neededMiB(), 4096 + 1024);
        QVERIFY(!waiting.isEmpty());
        QVERIFY(b.isRunning());
        QVERIFY(b.log().contains("Waiting for memory"));
        /* less and less free: told */
        waiting.clear();
        QVERIFY(write(meminfo, this->meminfo(20480, 2000)));
        QTRY_VERIFY_WITH_TIMEOUT(b.availableMiB() == 2000, 5000);
        QVERIFY(!waiting.isEmpty());
        /* free: on by itself; KWin waits again, and goes on when told */
        QVERIFY(write(meminfo, this->meminfo(20480, 6000)));
        QTRY_VERIFY_WITH_TIMEOUT(read(guest + "/rpms-args").contains("mesa"), 5000);
        QVERIFY(!b.isWaiting());
        QVERIFY(write(meminfo, this->meminfo(20480, 1000)));
        QVERIFY(write(m_tmp.filePath("memory-gate"), "go\n"));
        QTRY_VERIFY_WITH_TIMEOUT(b.isWaiting() && b.step() == 3, 10000);
        b.proceed();
        QVERIFY(!b.isWaiting());
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
        QCOMPARE(finished[0][0].toString(), "");
        QCOMPARE(read(guest + "/rpms-args"), "--verbose tools\n--verbose mesa\n--verbose kwin\n");

        /* stopped while it waits */
        QFile::remove(guest + "/rpms-args");
        QVERIFY(write(m_tmp.filePath("memory/rpms/mesa/.inputs"), "older\n"));
        finished.clear();
        b.start();
        QTRY_VERIFY_WITH_TIMEOUT(b.isWaiting(), 10000);
        b.cancel();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished[0][0].toString(), "Stopped");
        QVERIFY(b.wasStopped());
        QVERIFY(!b.isWaiting());
        QVERIFY(!b.isRunning());
        QVERIFY(!QFileInfo::exists(guest + "/rpms-args"));
    }

    /* What is missing, said with Fedora's packages, before anything runs */
    void tellsWhatIsMissing()
    {
        const QString guest = guestDir("missing-guest");
        const QString bin = m_tmp.filePath("bash-only");
        const QString script = m_tmp.filePath("no-podman");
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(QDir().mkpath(script));
        QVERIFY(QFile::link(QStandardPaths::findExecutable("bash"), bin + "/bash"));
        /* what build-rpms.sh needs to tell it, podman aside */
        for (const char *tool : {"bash", "dirname", "readlink", "flock"}) {
            const QString found =
                QStandardPaths::findExecutable(tool, m_path.split(':', Qt::SkipEmptyParts));
            QVERIFY2(!found.isEmpty(), tool);
            QVERIFY(QFile::link(found, script + '/' + tool));
        }
        GuestToolsBuilder b;
        setUp(b, guest, "missing");
        QSignalSpy started(&b, &GuestToolsBuilder::started);

        qputenv("PATH", bin.toUtf8());
        QCOMPARE(build(b), "Build dependencies are missing");
        QCOMPARE(b.missingCommands(), QStringList({"podman", "mkfs.fat", "mcopy", "rpm", "flock"}));
        QCOMPARE(b.missingInstall(), "sudo dnf install podman dosfstools mtools rpm util-linux-core");
        QCOMPARE(b.error(), "Build dependencies are missing");
        QVERIFY(started.isEmpty());
        QVERIFY(!QFileInfo::exists(guest + "/rpms-args"));

        /* the scripts say it too */
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PATH", script);
        int status = -1;
        const QString out = run(script + "/bash",
                                {GUEST_SOURCE_DIR "/build-rpms.sh", "--print-deps"}, &status, env);
        QCOMPARE(status, 1);
        QVERIFY2(out.contains("vitrine-build: missing: podman\n"), qPrintable(out));
        QVERIFY(out.contains("vitrine-build: install: sudo dnf install podman\n"));

        /* no guest/ */
        qputenv("PATH", (m_bin + ':' + m_path).toUtf8());
        b.setGuestDir(m_tmp.filePath("nowhere"));
        QVERIFY(build(b).contains("no guest/ folder"));
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    /* stopsWhenVitrineDies()'s vitrine: it builds until killed */
    if (const QString guest = qEnvironmentVariable("BUILDER_CHILD"); !guest.isEmpty()) {
        GuestToolsBuilder builder;
        builder.setGuestDir(guest);
        builder.setRpmsDir(qEnvironmentVariable("BUILDER_RPMS"));
        builder.setCacheDir(qEnvironmentVariable("BUILDER_RPMS") + "/../cache");
        builder.setMediumImage(qEnvironmentVariable("BUILDER_RPMS") + "/../medium.img");
        builder.start();
        return app.exec();
    }
    TestGuestToolsBuilder test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_guesttoolsbuilder.moc"
