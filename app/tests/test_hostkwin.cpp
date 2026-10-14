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
#include "core/hostkwin.h"
#include "core/hostkwinbuilder.h"
#include "core/stackbuilder.h"

using namespace HostKwin;
using Offer = Status::Offer;
using Owner = Status::Owner;

/*
 * Stand-ins for rpm and dnf, on a state folder STATE: installed/NAME and
 * repo/NAME (what distro-sync brings) hold NAME=value lines as the "RPM
 * files" do (NAME, VERSION, RELEASE, ARCH, DIGEST, SOURCERPM, EPOCHNUM),
 * locks/NAME the EVR locked.  Enough of both for install.sh and for
 * System::read(); rpm --eval goes to the real rpm (vercmp).  (No line of
 * them starts with a #, nor has a \": moc would take them for a directive
 * or a string's end.)
 */
static const char kFakeRpm[] = R"sh(#!/bin/bash
state=${STATE:?}
field() { sed -n "s/^$2=//p" "$1" | head -n 1; }
format() {
	local f=$1 fmt=$2 tag
	for tag in NAME VERSION RELEASE ARCH SOURCERPM EPOCHNUM; do
		fmt=${fmt//%\{$tag\}/$(field "$f" "$tag")}
	done
	fmt=${fmt//%\{SHA256HEADER\}/$(field "$f" DIGEST)}
	fmt=${fmt//%\{VENDOR\}/$(field "$f" VENDOR)}
	fmt=${fmt//%\{INSTALLTIME\}/$(field "$f" INSTALLTIME)}
	fmt=${fmt//%\{NEVRA\}/$(field "$f" NAME)-$(field "$f" VERSION)-$(field "$f" RELEASE).$(field "$f" ARCH)}
	printf '%b' "$fmt"
}
echo "rpm $*" >> "$state/calls"
case "$1" in
--eval) exec /usr/bin/rpm "$@" ;;
-qp) shift; [ "$1" = --qf ] && { fmt=$2; shift 2; }; format "$1" "$fmt"; exit 0 ;;
-qa) shift; [ "$1" = --qf ] && fmt=$2; for f in "$state"/installed/*; do [ -e "$f" ] && format "$f" "$fmt"; done; exit 0 ;;
-q)
	shift
	[ "${1:-}" = --qf ] && { fmt=$2; shift 2; }
	rc=0
	[ -n "${fmt:-}" ] || fmt='%{NAME}\n'
	for n; do
		if [ -e "$state/installed/$n" ]; then format "$state/installed/$n" "$fmt"; else echo "package $n is not installed"; rc=1; fi
	done
	exit $rc
	;;
esac
exit 2
)sh";

static const char kFakeDnf[] = R"sh(#!/bin/bash
state=${STATE:?}
echo "dnf $*" >> "$state/calls"
field() { sed -n "s/^$2=//p" "$1" | head -n 1; }
args=()
for a; do case "$a" in -y | -q | -C | --setopt=*) ;; *) args+=("$a") ;; esac; done
set -- "${args[@]}"
locked() { [ -e "$state/locks/$1" ] && [ "$(cat "$state/locks/$1")" != "$2" ]; }
take() {
	local f=$1 n
	n=$(field "$f" NAME)
	if locked "$n" "$(field "$f" VERSION)-$(field "$f" RELEASE)"; then echo "$n is version-locked" >&2; return 1; fi
	cp "$f" "$state/installed/$n"
}
case "$1" in
--version) echo "dnf5 version 5.4.6.0"; echo "libdnf5 version 5.4.6.0" ;;
versionlock)
	case "$2" in
	list) for f in "$state"/locks/*; do [ -e "$f" ] && printf 'Package name: %s\nevr = %s\n\n' "${f##*/}" "$(cat "$f")"; done ;;
	add) shift 2; for n; do echo "$(field "$state/installed/$n" VERSION)-$(field "$state/installed/$n" RELEASE)" > "$state/locks/$n"; done ;;
	delete) shift 2; for n; do rm -f "$state/locks/$n"; done ;;
	esac
	;;
do)
	[ "${2:-}" = --help ] && { [ -e "$state/no-do" ] && exit 2; exit 0; }
	[ -e "$state/fail" ] && { echo "Transaction failed" >&2; exit 1; }
	shift
	for a; do case "$a" in --action=*) echo "${a#--action=}" >> "$state/actions" ;; *) take "$a" || exit 1 ;; esac; done
	;;
upgrade | downgrade | reinstall)
	[ -e "$state/fail" ] && exit 1
	echo "$1" >> "$state/actions"
	shift
	for f; do take "$f" || exit 1; done
	;;
distro-sync)
	[ -e "$state/fail" ] && exit 1
	shift
	for n; do [ -e "$state/repo/$n" ] && { take "$state/repo/$n" || exit 1; }; done
	;;
repoquery)
	shift
	for n; do [ -e "$state/installed/$n" ] && echo "$n $(field "$state/installed/$n" REPO)"; done
	;;
esac
exit 0
)sh";

class TestHostKwin : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_bin;          // the stand-ins, first in PATH
    QString m_state;
    QByteArray m_path;

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

    /* A package's lines, as installed/NAME or an "RPM file" holds them */
    static QByteArray pkg(const QString &name, const QString &evr, const QString &digest,
                          const QString &repo = "fedora", const QString &arch = "x86_64")
    {
        const QString version = evr.section('-', 0, 0);
        const QString release = evr.section('-', 1);
        return QString("NAME=%1\nVERSION=%2\nRELEASE=%3\nARCH=%4\nDIGEST=%5\nSOURCERPM=%6-%2-%3.src.rpm\n"
                       "EPOCHNUM=0\nREPO=%7\nINSTALLTIME=1760000000\nVENDOR=(none)\n")
            .arg(name, version, release, arch, digest, name.startsWith("kwin") ? "kwin" : name, repo)
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
        p.sourceRpm = "kwin-" + evr + ".src.rpm";
        p.repo = repo;
        p.installed = QDateTime::fromSecsSinceEpoch(1760000000, QTimeZone::UTC);
        return p;
    }

    /* A Fedora 44 host with Plasma @plasma from @from and the distribution's KWin @kwin */
    static System host(const QString &plasma = "6.7.91", const QString &kwin = "6.7.91-1.fc44",
                       const QString &from = "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta")
    {
        System s;
        s.rpm = true;
        s.versionlock = true;
        s.pkexec = true;
        s.fedora = "44";
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            s.kwin << package(name, kwin, QString("distro-%1").arg(name), from);
        }
        Package p = package("plasma-workspace", plasma + "-1.fc44", "pw", from);
        p.sourceRpm = "plasma-workspace-" + plasma + "-1.fc44.src.rpm";
        s.others << p;
        Package qt = package("qt6-qtbase", "6.11.2-3.fc44", "qt", "updates");
        qt.sourceRpm = "qt6-qtbase-6.11.2-3.fc44.src.rpm";
        s.others << qt;
        return s;
    }

    static Recipe recipe(const QString &series = "hl39", const QString &tag = "hl39")
    {
        Recipe r;
        r.fedora = "44";
        r.copr = "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta";
        r.series = series;
        r.tag = tag;
        r.key = "key1";
        return r;
    }

    /* A build of @evr from the beta COPR against Qt 6.11.2, its digests "<evr>-<name>" */
    static Build build(const QString &evr = "6.7.91-1.hl39.fc44", const QString &qt = "6.11.2-3.fc44",
                       const QString &key = "key1", const QString &plasma = "beta")
    {
        Build b;
        b.dir = "/out/" + evr;
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

    /* @system with Vitrine's @b installed (locked if @lock) */
    static System withVitrines(System system, const Build &b, bool lock = true)
    {
        for (Package &p : system.kwin) {
            p.version = b.version;
            p.release = b.evr.section('-', 1);
            p.digest = b.rpm(p.name).digest;
            p.repo = "@commandline";
            if (lock) {
                system.locks.insert(p.name, b.evr);
            }
        }
        return system;
    }

    QString hostKwinDir() const { return QStringLiteral(HOST_KWIN_SOURCE_DIR); }

    /* install.sh with the stand-ins, as "root"; its output */
    QString installScript(const QStringList &args, int *status, bool root = true)
    {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PATH", m_bin + ":/usr/bin:/bin");
        env.insert("STATE", m_state);
        env.insert("TMPDIR", m_tmp.filePath("tmp"));
        env.insert("FAKE_UID", root ? "0" : "1000");
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        QDir().mkpath(m_tmp.filePath("tmp"));
        p.start("/bin/bash", QStringList{hostKwinDir() + "/install.sh"} + args);
        p.waitForFinished(30000);
        *status = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        return QString::fromUtf8(p.readAll());
    }

    void resetState()
    {
        QDir(m_state).removeRecursively();
        for (const char *dir : {"installed", "repo", "locks", "files"}) {
            QDir().mkpath(m_state + '/' + dir);
        }
    }

    /* An "RPM file" of the set, and its argument FILE=SHA256 */
    QString rpmFile(const QString &name, const QString &evr, const QString &digest)
    {
        const QString path = m_state + "/files/" + name + '-' + evr + ".x86_64.rpm";
        write(path, pkg(name, evr, digest, "@commandline"));
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

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_bin = m_tmp.filePath("bin");
        m_state = m_tmp.filePath("state");
        QVERIFY(write(m_bin + "/rpm", kFakeRpm));
        QVERIFY(write(m_bin + "/dnf", kFakeDnf));
        QVERIFY(write(m_bin + "/id", "#!/bin/bash\n[ \"$1\" = -u ] && { echo \"${FAKE_UID:-0}\"; exit 0; }\nexec /usr/bin/id \"$@\"\n"));
        for (const char *name : {"rpm", "dnf", "id"}) {
            QFile::setPermissions(m_bin + '/' + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        m_path = qgetenv("PATH");
        resetState();
    }

    void cleanup()
    {
        qputenv("PATH", m_path);
    }

    /* What versions.sh maps to sets, as the window lists them */
    void setsOfTheRecipe()
    {
        const QList<Set> sets = HostKwin::sets(hostKwinDir());
        QCOMPARE(sets.size(), 3);
        QCOMPARE(sets[0].series, "hl39");
        QCOMPARE(sets[0].tag, "hl39");
        QCOMPARE(sets[0].versions, QStringList{"6.7.91"});
        QCOMPARE(sets[1].series, "hl39-68");
        QCOMPARE(sets[1].tag, "h18");
        QCOMPARE(sets[1].versions, QStringList{"6.8.0"});
        QCOMPARE(sets[2].series, "hl39-681");
        QCOMPARE(sets[2].versions, QStringList{"6.8.*"});
        /* their series files are in the copy */
        for (const Set &set : sets) {
            QVERIFY2(QFileInfo::exists(hostKwinDir() + "/research/patches/kwin/sets/" + set.series +
                                       ".series"),
                     qPrintable(set.series));
        }
    }

    /* build.sh --print-recipe: the research's kde_set_for, run by bash */
    void recipeFromTheScript()
    {
        const Recipe r = Recipe::read(hostKwinDir(), "6.7.91", "44");
        QVERIFY(r.isValid());
        QCOMPARE(r.fedora, "44");
        QCOMPARE(r.copr, "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta");
        QCOMPARE(r.series, "hl39");
        QCOMPARE(r.tag, "hl39");
        QCOMPARE(Recipe::read(hostKwinDir(), "6.8.0", "44").series, "hl39-68");
        QCOMPARE(Recipe::read(hostKwinDir(), "6.8.0", "44").tag, "h18");
        QCOMPARE(Recipe::read(hostKwinDir(), "6.8.3", "44").series, "hl39-681");
        /* Fedora 44's own Plasma, and one after the sets */
        QVERIFY(Recipe::read(hostKwinDir(), "6.7.5", "44").series.isEmpty());
        QVERIFY(Recipe::read(hostKwinDir(), "6.9.0", "44").series.isEmpty());
        QVERIFY(Recipe::read(hostKwinDir(), "6.7.5", "44").isValid());
        /* VGD_FEDORA: the recipe builds for the host's Fedora */
        QCOMPARE(Recipe::read(hostKwinDir(), "6.7.91", "45").fedora, "45");
        QVERIFY(!Recipe::read(QString(), "6.7.91", "44").isValid());

        /* its key is the Builds component's, which a build records */
        Builds::Places places;
        places.kwinDir = hostKwinDir();
        const QList<Builds::Component> all = Builds::load(places);
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.first().id, "host-kwin");
        QCOMPARE(all.first().kind, Builds::Kind::System);
        QVERIFY(all.first().heavy);
        QCOMPARE(Builds::key(all.first(), all), r.key);
        QProcess p;
        p.start("/bin/bash", {hostKwinDir() + "/build.sh", "--print-inputs"});
        QVERIFY(p.waitForFinished());
        QCOMPARE(p.readAllStandardOutput(), Builds::inputLines(all.first(), all));
    }

    void patchesBySet()
    {
        Builds::Places places;
        places.kwinDir = hostKwinDir();
        const Builds::Component c = Builds::load(places).value(0);
        const QList<Builds::Patch> patches = Builds::patches(c);
        QCOMPARE(patches.size(), 14 * 3);
        QCOMPARE(patches.first().origin, "hl39, for KWin 6.7.91");
        QCOMPARE(patches.last().origin, "hl39-681, for KWin 6.8.x");
        QCOMPARE(patches.last().name(), "0039-EXP-input-pointer-constraints-on-subsurfaces.68.patch");
        QCOMPARE(Builds::subject(c, patches.first()), "Patch100 kwin-vm-frame-timing (ported to 6.8)");
        QVERIFY(Builds::text(c, patches.last()).contains("pointer"));
    }

    void parsing()
    {
        const QByteArray rpm =
            "kwin\t6.7.91\t1.hm4.fc44\tx86_64\t8fe2\t(none)\tkwin-6.7.91-1.hm4.fc44.src.rpm\t1760375000\n"
            "package kwin-devel is not installed\n"
            "plasma-workspace\t6.7.91\t1.fc44\tx86_64\t85f5\tFedora Copr - group @kdesig\t"
            "plasma-workspace-6.7.91-1.fc44.src.rpm\t1760300000\n";
        const QByteArray repos = "kwin @commandline\nplasma-workspace copr:copr.fedorainfracloud.org:group_kdesig:kde-beta\n";
        const QList<Package> list = System::parsePackages(rpm, repos);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].nevra(), "kwin-6.7.91-1.hm4.fc44.x86_64");
        QCOMPARE(list[0].digest, "8fe2");
        QVERIFY(list[0].vendor.isEmpty());
        QVERIFY(list[0].isLocal());
        QCOMPARE(list[0].installed.toSecsSinceEpoch(), 1760375000);
        QCOMPARE(list[1].vendor, "Fedora Copr - group @kdesig");
        QVERIFY(!list[1].isLocal());

        /* dnf5's versionlock list, of kwin's packages only */
        const QByteArray locks =
            "# Added by 'versionlock add' command on 2026-10-05 17:26:04\n"
            "Package name: virglrenderer\nevr = 1.3.0^20260928gitaafa9bd2-1.fc44\n\n"
            "# Added by 'versionlock add' command on 2026-10-13 19:21:58\n"
            "Package name: kwin\nevr = 6.7.91-1.hm4.fc44\n\n"
            "Package name: kwin-libs\nevr = 6.7.91-1.hm4.fc44\n";
        const QHash<QString, QString> l = System::parseLocks(locks);
        QCOMPARE(l.size(), 2);
        QCOMPARE(l.value("kwin"), "6.7.91-1.hm4.fc44");
        QVERIFY(!l.contains("virglrenderer"));

        const Recipe r = Recipe::parse("fedora=44\ncopr=c\nset=hl39-68 h18\nkey=abc\n");
        QCOMPARE(r.series, "hl39-68");
        QCOMPARE(r.tag, "h18");
        QVERIFY(Recipe::parse("fedora=44\ncopr=c\nset=\nkey=abc\n").series.isEmpty());

        QCOMPARE(plasmaRelease("6.8.1.1"), "6.8.1");
        QCOMPARE(plasmaRelease("6.7.91"), "6.7.91");
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
                      "evr=6.8.0-1.h18.fc44\nversion=6.8.0\nbuilt=2026-10-15T08:00:00Z\n"
                      "rpm=kwin x86_64 d3 kwin/kwin-6.8.0-1.h18.fc44.x86_64.rpm\n"));
        /* a build going on, and one without RPMs */
        QVERIFY(write(out + "/.new/build.conf", "evr=x\nrpm=kwin x86_64 d kwin/x.rpm\n"));
        QVERIFY(write(out + "/broken/build.conf", "evr=6.7.91-2\n"));
        const QList<Build> list = builds(out);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].evr, "6.8.0-1.h18.fc44");
        QCOMPARE(list[1].series, "hl39");
        QCOMPARE(list[1].research, "3653233");
        QCOMPARE(list[1].rpm("kwin-libs").digest, "d2");
        QCOMPARE(list[1].buildroot.value("qt6-qtbase"), "6.11.2-3.fc44");
        QCOMPARE(list[1].built, QDateTime(QDate(2026, 10, 14), QTime(20, 0), QTimeZone::UTC));

        const Record r = Record::of(list[1], QDateTime(QDate(2026, 10, 14), QTime(21, 0), QTimeZone::UTC));
        QCOMPARE(r.digests.value("kwin"), "d1");
        QCOMPARE(r.buildroot.value("qt6-qtbase"), "6.11.2");
        QCOMPARE(r.buildroot.value("kdecoration"), "6.7.91");
        QVERIFY(r.write(Record::path(out)));
        const Record back = Record::read(Record::path(out));
        QCOMPARE(back.evr, r.evr);
        QCOMPARE(back.when, r.when);
        QCOMPARE(back.digests, r.digests);
        QCOMPARE(back.buildroot, r.buildroot);
        QVERIFY(!Record::read(m_tmp.filePath("nothing")).isValid());
    }

    /* Not a Fedora Plasma host: not shown at all */
    void hidden()
    {
        System s = host();
        QCOMPARE(status(s, recipe(), {}, {}).offer, Offer::Offered);
        QVERIFY(!status(s, recipe(), {}, {}).summary().isEmpty());
        s.rpm = false;
        QCOMPARE(status(s, recipe(), {}, {}).offer, Offer::Hidden);
        s = host();
        s.fedora.clear();
        QCOMPARE(status(s, recipe(), {}, {}).offer, Offer::Hidden);
        s = host();
        s.kwin.clear();
        QCOMPARE(status(s, recipe(), {}, {}).offer, Offer::Hidden);
        QVERIFY(status(s, recipe(), {}, {}).summary().isEmpty());
    }

    /* Fedora 44's own Plasma 6.7.5: no set; another Fedora or Plasma: not offered */
    void notOffered()
    {
        const System stock = host("6.7.5", "6.7.5-1.fc44", "6ecc2dfaa0dc41e5ad51e007707a786b");
        Status s = status(stock, recipe(QString(), QString()), {}, {});
        QCOMPARE(s.offer, Offer::NoSet);
        QCOMPARE(s.plasmaFrom, "fedora");
        QCOMPARE(s.summary(), "No host set for KWin 6.7.5");
        QString why;
        QVERIFY(!s.canBuild(&why));
        QVERIFY(why.contains("6.7.5"));
        QVERIFY(!s.canInstall());
        QCOMPARE(s.owner, Owner::Distribution);

        Recipe other = recipe();
        other.fedora = "45";
        s = status(host(), other, {}, {});
        QCOMPARE(s.offer, Offer::Unsupported);
        QVERIFY(s.why.contains("Fedora 45"));
        QVERIFY(!s.canBuild());

        s = status(host("6.7.91", "6.7.91-1.fc44", "copr:copr.fedorainfracloud.org:someone:plasma"),
                   recipe(), {}, {});
        QCOMPARE(s.offer, Offer::Unsupported);
        QVERIFY(s.why.contains("someone"));
        s = status(host("6.7.91", "6.7.91-1.fc44", "updates-testing"), recipe(), {}, {});
        QCOMPARE(s.offer, Offer::Offered);
        QCOMPARE(s.plasmaFrom, "testing");
        QCOMPARE(status(host(), Recipe(), {}, {}).offer, Offer::Unsupported);
    }

    void notBuiltThenBuilt()
    {
        const System system = host();
        Status s = status(system, recipe(), {}, {});
        QCOMPARE(s.offer, Offer::Offered);
        QCOMPARE(s.plasmaFrom, "beta");
        QCOMPARE(s.summary(), "Not built");
        QVERIFY(s.canBuild());
        QVERIFY(!s.canInstall());
        QVERIFY(!s.canRevert());
        QVERIFY(!s.wantsUpdates());

        s = status(system, recipe(), {build()}, {});
        QVERIFY(s.build.isValid());
        QVERIFY(s.differs.isEmpty());
        QVERIFY(!s.recipeChanged);
        QCOMPARE(s.summary(), "Built, not installed");
        QVERIFY(s.canInstall());

        /* a newer recipe: still installable, out of date as a build */
        s = status(system, recipe(), {build("6.7.91-1.hl39.fc44", "6.11.2-3.fc44", "old")}, {});
        QVERIFY(s.recipeChanged);
        QVERIFY(s.canInstall());

        /* against another Qt than the system's: not before they match */
        s = status(system, recipe(), {build("6.7.91-1.hl39.fc44", "6.11.3-1.fc44")}, {});
        QCOMPARE(s.differs, QStringList{"qt6-qtbase 6.11.3 6.11.2"});
        QString why;
        QVERIFY(!s.canInstall(&why));
        QVERIFY(why.contains("6.11.3"));
        QCOMPARE(s.summary(), "Built against Qt 6.11.3 (this computer has 6.11.2)");

        /* without versionlock or pkexec: a plain word, before any password */
        System bare = system;
        bare.versionlock = false;
        QVERIFY(!status(bare, recipe(), {build()}, {}).canInstall(&why));
        QVERIFY(why.contains("versionlock"));
        bare = system;
        bare.pkexec = false;
        QVERIFY(!status(bare, recipe(), {build()}, {}).canInstall(&why));
        QVERIFY(why.contains("pkexec"));

        /* for another Plasma, another origin, another Fedora: none of them is for this one */
        s = status(host("6.8.0", "6.8.0-1.fc44"), recipe("hl39-68", "h18"), {build()}, {});
        QVERIFY(!s.build.isValid());
        QCOMPARE(s.other.evr, "6.7.91-1.hl39.fc44");
        QCOMPARE(s.summary(), "Built for KWin 6.7.91; this computer has Plasma 6.8.0");
        QVERIFY(!s.canInstall(&why));
        QVERIFY(why.contains("6.8.0"));
        s = status(system, recipe(), {build("6.7.91-1.hl39.fc44", "6.11.2-3.fc44", "key1", "fedora")}, {});
        QVERIFY(!s.build.isValid());
        Build b = build();
        b.fedora = "43";
        QVERIFY(!status(system, recipe(), {b}, {}).build.isValid());
    }

    /* The research's own set on this machine: not Vitrine's, whatever its version */
    void foreign()
    {
        System system = host();
        for (Package &p : system.kwin) {
            p.release = "1.hm4.fc44";
            p.digest = "hm4-" + p.name;
            p.repo = "@commandline";
            system.locks.insert(p.name, "6.7.91-1.hm4.fc44");
        }
        Status s = status(system, recipe(), {build()}, {});
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

        /* the repositories' KWin, locked by someone else */
        system = host();
        system.locks.insert("kwin", "6.7.91-1.fc44");
        s = status(system, recipe(), {}, {});
        QCOMPARE(s.owner, Owner::Foreign);
        QCOMPARE(s.summary(), "KWin is version-locked, not by Vitrine");
    }

    void installedByVitrine()
    {
        const Build b = build();
        const Record record = Record::of(b, QDateTime::currentDateTimeUtc());
        System system = withVitrines(host(), b);
        Status s = status(system, recipe(), {b}, record);
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
        QCOMPARE(status(system, recipe(), {}, record).owner, Owner::Vitrine);
        QCOMPARE(status(system, recipe(), {b}, {}).owner, Owner::Vitrine);

        /* a rebuild of it (a newer recipe): ready to replace it */
        Build rebuilt = build("6.7.91-1.hl39.fc44", "6.11.2-3.fc44", "key2");
        for (Build::Rpm &rpm : rebuilt.rpms) {
            rpm.digest += "-2";
        }
        rebuilt.built = b.built.addSecs(3600);
        s = status(system, recipe(), {rebuilt, b}, record);
        QCOMPARE(s.owner, Owner::Vitrine);
        QCOMPARE(s.summary(), "Installed and locked; another build is ready");
        QVERIFY(s.canInstall());

        /* the lock gone: installing again locks it */
        system = withVitrines(host(), b, false);
        s = status(system, recipe(), {b}, record);
        QCOMPARE(s.summary(), "Installed, not version-locked");
        QVERIFY(s.canInstall());

        /* installed after the session's KWin started: from the next login */
        system = withVitrines(host(), b);
        system.session = QDateTime::fromSecsSinceEpoch(1759990000, QTimeZone::UTC);
        s = status(system, recipe(), {b}, record);
        QVERIFY(s.nextLogin);
        QCOMPARE(s.summary(), "Installed and locked, from the next login");
        system.session = QDateTime::fromSecsSinceEpoch(1760010000, QTimeZone::UTC);
        QVERIFY(!status(system, recipe(), {b}, record).nextLogin);
    }

    /* Plasma or Qt updated past Vitrine's KWin: out of date, said plainly */
    void outdatedByUpdates()
    {
        const Build b = build();
        const Record record = Record::of(b, QDateTime::currentDateTimeUtc());
        System system = withVitrines(host("6.8.0", "6.8.0-1.fc44"), b);
        Status s = status(system, recipe("hl39-68", "h18"), {b}, record);
        QCOMPARE(s.owner, Owner::Vitrine);
        QCOMPARE(s.outdated, QStringList{"plasma-workspace 6.7.91 6.8.0"});
        QVERIFY(s.isOutdated());
        QCOMPARE(s.summary(), "Out of date: Plasma 6.8.0 is installed");
        QVERIFY(s.canRevert());
        QVERIFY(s.canBuild());
        QVERIFY(!s.canInstall());
        /* once built for it: installable */
        s = status(system, recipe("hl39-68", "h18"),
                   {build("6.8.0-1.h18.fc44", "6.11.2-3.fc44"), b}, record);
        QVERIFY(s.canInstall());
        QVERIFY(s.isOutdated());

        /* no set for the new Plasma: reverting is what is left */
        s = status(withVitrines(host("6.9.0", "6.9.0-1.fc44"), b), recipe(QString(), QString()), {b},
                   record);
        QCOMPARE(s.offer, Offer::NoSet);
        QVERIFY(s.isOutdated());
        QCOMPARE(s.summary(), "Out of date: Plasma 6.9.0 is installed");
        QVERIFY(s.canRevert());
        QVERIFY(!s.canBuild());
        QVERIFY(!s.wantsUpdates());

        /* Qt: the version a lock lets through, which KWin's private API does not */
        system = withVitrines(host(), b);
        for (Package &p : system.others) {
            if (p.name == "qt6-qtbase") {
                p.version = "6.11.3";
                p.release = "1.fc44";
            }
        }
        s = status(system, recipe(), {b}, record);
        QCOMPARE(s.outdated, QStringList{"qt6-qtbase 6.11.2 6.11.3"});
        QCOMPARE(s.summary(), "Out of date: Qt 6.11.3 is installed");
        /* a rebuild of the same Qt (another release) is not an update of it */
        for (Package &p : system.others) {
            if (p.name == "qt6-qtbase") {
                p.version = "6.11.2";
                p.release = "4.fc44";
            }
        }
        QVERIFY(status(system, recipe(), {b}, record).outdated.isEmpty());
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
        const QStringList install = installCommand("/src/host/kwin", b);
        QCOMPARE(install.mid(0, 4), QStringList({m_bin + "/pkexec", "/usr/bin/bash",
                                                 "/src/host/kwin/install.sh", "install"}));
        QCOMPARE(install.size(), 4 + b.rpms.size());
        /* sha256sum's hash of the files */
        QCOMPARE(install[4], out + "/kwin/kwin-6.7.91-1.hl39.fc44.x86_64.rpm=" +
                                 QCryptographicHash::hash("kwin", QCryptographicHash::Sha256).toHex());

        Record record;
        record.digests.insert("kwin", "recorded");
        record.digests.insert("kwin-libs", "not-kwin");
        const QStringList revert = revertCommand("/src/host/kwin", {b, build()}, record);
        QCOMPARE(revert.mid(2), QStringList({"/src/host/kwin/install.sh", "revert", "recorded",
                                             "6.7.91-1.hl39.fc44-kwin"}));
        QFile::remove(m_bin + "/pkexec");
    }

    /* The system as rpm and dnf say it is, and the session's KWin from /proc */
    void readsTheSystem()
    {
        resetState();
        QVERIFY(write(m_state + "/installed/kwin", pkg("kwin", "6.7.91-1.hm4.fc44", "hm4", "@commandline")));
        QVERIFY(write(m_state + "/installed/kwin-common", pkg("kwin-common", "6.7.91-1.hm4.fc44", "hm4c", "@commandline")));
        QVERIFY(write(m_state + "/installed/plasma-workspace",
                      pkg("plasma-workspace", "6.7.91-1.fc44", "pw",
                          "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta")));
        QVERIFY(write(m_state + "/installed/qt6-qtbase", pkg("qt6-qtbase", "6.11.2-3.fc44", "qt")));
        QVERIFY(write(m_state + "/locks/kwin", "6.7.91-1.hm4.fc44"));
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
        const System s = System::read(m_tmp.filePath("os-release"), proc);
        QVERIFY(s.rpm);
        QVERIFY(s.versionlock);
        QCOMPARE(s.fedora, "44");
        QCOMPARE(s.kwin.size(), 2);
        QCOMPARE(s.package("kwin").release, "1.hm4.fc44");
        QCOMPARE(s.package("kwin").repo, "@commandline");
        QCOMPARE(s.package("plasma-workspace").repo,
                 "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta");
        QCOMPARE(s.plasma(), "6.7.91");
        QVERIFY(s.isPlasmaHost());
        QCOMPARE(s.locks.value("kwin"), "6.7.91-1.hm4.fc44");
        QCOMPARE(s.session.toSecsSinceEpoch(), 1760000100);
        /* what it ran: nothing that writes */
        const QByteArray calls = read(m_state + "/calls");
        QVERIFY2(calls.contains("dnf -q -C repoquery --installed"), calls.constData());
        QVERIFY(!calls.contains("versionlock add"));

        /* not Fedora; no dnf5 */
        QVERIFY(write(m_tmp.filePath("os-release"), "ID=debian\nVERSION_ID=13\n"));
        QVERIFY(System::read(m_tmp.filePath("os-release"), proc).fedora.isEmpty());
        qputenv("PATH", "/nonexistent");
        QVERIFY(!System::read(m_tmp.filePath("os-release"), proc).rpm);
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
        QVERIFY(out.contains("vitrine-kwin: left out: kwin-devel, not installed"));
        QVERIFY(out.contains("vitrine-kwin: installed: kwin-6.7.91-1.hl39.fc44.x86_64"));
        QVERIFY(out.contains("vitrine-kwin: locked: kwin kwin-common kwin-libs"));
        QVERIFY(out.trimmed().endsWith("vitrine-kwin: done"));
        QCOMPARE(installed("kwin"), "v-kwin");
        QCOMPARE(installed("kwin-libs"), "v-libs");
        QVERIFY(!QFileInfo::exists(m_state + "/installed/kwin-devel"));
        QCOMPARE(read(m_state + "/actions"), "upgrade\n");
        QCOMPARE(read(m_state + "/locks/kwin").trimmed(), "6.7.91-1.hl39.fc44");
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
        QVERIFY(out.contains("vitrine-kwin: error: dnf could not install them"));
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
        /* no KWin installed: nothing to replace */
        resetState();
        out = installScript({"install", rpmFile("kwin", "6.7.91-1.hl39.fc44", "v-kwin")}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("nothing to replace"));
        QVERIFY(!QFileInfo::exists(m_state + "/installed/kwin"));
    }

    /* install.sh's revert: Vitrine's KWin only, the repositories' back, the lock gone */
    void revertScript()
    {
        int status = -1;
        resetState();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            QVERIFY(write(m_state + "/installed/" + name,
                          pkg(name, "6.7.91-1.hl39.fc44", QString("v-%1").arg(name), "@commandline")));
            QVERIFY(write(m_state + "/locks/" + name, "6.7.91-1.hl39.fc44"));
            QVERIFY(write(m_state + "/repo/" + name, pkg(name, "6.7.91-1.fc44", "distro")));
        }
        /* another's: left alone */
        QString out = installScript({"revert", "hm4"}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("leaves it alone"));
        QCOMPARE(installed("kwin"), "v-kwin");
        QVERIFY(QFileInfo::exists(m_state + "/locks/kwin"));

        /* the repositories out of reach: locked again */
        QVERIFY(write(m_state + "/fail", ""));
        out = installScript({"revert", "other", "v-kwin"}, &status);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("could not install the distribution's KWin"));
        QCOMPARE(read(m_state + "/locks/kwin").trimmed(), "6.7.91-1.hl39.fc44");

        QFile::remove(m_state + "/fail");
        /* debug information an install left alone: not reverted either */
        QVERIFY(write(m_state + "/installed/kwin-debuginfo", pkg("kwin-debuginfo", "6.7.91-1.fc44", "dbg")));
        QFile::remove(m_state + "/calls");
        out = installScript({"revert", "v-kwin"}, &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY2(read(m_state + "/calls").contains("distro-sync kwin kwin-common kwin-libs\n"),
                 read(m_state + "/calls").constData());
        QCOMPARE(installed("kwin"), "distro");
        QCOMPARE(installed("kwin-libs"), "distro");
        QVERIFY(!QFileInfo::exists(m_state + "/locks/kwin"));
        QVERIFY(out.contains("vitrine-kwin: installed: kwin-6.7.91-1.fc44.x86_64"));
    }

    /* The monitor: read with the stand-ins; pkexec's answers */
    void monitorInstalls()
    {
        resetState();
        for (const char *name : {"kwin", "kwin-common", "kwin-libs"}) {
            QVERIFY(write(m_state + "/installed/" + name, pkg(name, "6.7.91-1.fc44", "distro",
                                                              "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta")));
        }
        QVERIFY(write(m_state + "/installed/plasma-workspace",
                      pkg("plasma-workspace", "6.7.91-1.fc44", "pw",
                          "copr:copr.fedorainfracloud.org:group_kdesig:kde-beta")));
        QVERIFY(write(m_tmp.filePath("os-release"), "ID=fedora\nVERSION_ID=44\n"));
        const QString out = m_tmp.filePath("monitor-out");
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
                      "case ${PKEXEC_EXIT:-0} in 0) echo 'vitrine-kwin: done' ;; 1) echo 'vitrine-kwin: error: no room left' ;; esac\n"
                      "exit ${PKEXEC_EXIT:-0}\n"));
        QFile::setPermissions(m_bin + "/pkexec", QFile::ReadOwner | QFile::ExeOwner);
        qputenv("PATH", m_bin.toUtf8() + ":/usr/bin:/bin");
        qputenv("STATE", m_state.toUtf8());

        HostKwinMonitor monitor;
        monitor.setSourceDir(hostKwinDir());
        monitor.setOutDir(out);
        monitor.setOsRelease(m_tmp.filePath("os-release"));
        monitor.setProc(m_tmp.filePath("noproc"));
        QSignalSpy changed(&monitor, &HostKwinMonitor::changed);
        monitor.refresh();
        QVERIFY(changed.wait(10000));
        QVERIFY(monitor.isRead());
        Status s = monitor.status();
        QCOMPARE(s.offer, Offer::Offered);
        QCOMPARE(s.series, "hl39");
        QVERIFY(s.build.isValid());
        /* the recipe's key differs from "k": still installable */
        QVERIFY(s.recipeChanged);
        QVERIFY(s.canInstall());

        QSignalSpy finished(&monitor, &HostKwinMonitor::finished);
        qputenv("PKEXEC_EXIT", "126");
        monitor.install(s.build);
        QVERIFY(monitor.isBusy());
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(1).toBool(), true);
        QVERIFY(!QFileInfo::exists(Record::path(out)));
        QVERIFY(read(m_state + "/pkexec").startsWith("/usr/bin/bash " + hostKwinDir().toUtf8() +
                                                     "/install.sh install " + b.dir.toUtf8()));

        qputenv("PKEXEC_EXIT", "1");
        monitor.install(s.build);
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), "no room left");
        QCOMPARE(finished.last().value(1).toBool(), false);

        qputenv("PKEXEC_EXIT", "0");
        monitor.install(s.build);
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), QString());
        QVERIFY(monitor.log().contains("vitrine-kwin: done"));
        /* the record: Vitrine's now, by its digests */
        QCOMPARE(Record::read(Record::path(out)).digests.value("kwin"), "6.7.91-1.hl39.fc44-kwin");
        QVERIFY(changed.wait(10000));

        monitor.revert();
        QVERIFY(finished.wait(10000));
        QVERIFY(read(m_state + "/pkexec").contains("install.sh revert 6.7.91-1.hl39.fc44-kwin"));
        QVERIFY(!QFileInfo::exists(Record::path(out)));
        qunsetenv("PKEXEC_EXIT");
        QFile::remove(m_bin + "/pkexec");
    }

    /* The builder: build.sh's stand-in, a wait for memory, a stop */
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
        HostKwinBuilder builder;
        builder.setSourceDir(src);
        builder.setOutDir(m_tmp.filePath("bout"));
        builder.setWorkDir(m_tmp.filePath("bwork"));
        builder.setJobs(3);
        QVERIFY(write(m_tmp.filePath("meminfo"), "MemTotal: 33554432 kB\nMemAvailable: 33554432 kB\n"));
        builder.setMeminfo(m_tmp.filePath("meminfo"));
        QSignalSpy finished(&builder, &HostKwinBuilder::finished);
        QSignalSpy progress(&builder, &HostKwinBuilder::progress);
        builder.start("beta");
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), QString());
        QCOMPARE(builder.built(), "/out/6.7.91-1.hl39.fc44");
        QCOMPARE(read(src + "/args").trimmed(),
                 QString("--out %1 --work %2 -j 3 --plasma beta")
                     .arg(m_tmp.filePath("bout"), m_tmp.filePath("bwork")).toUtf8());
        QVERIFY(std::any_of(progress.begin(), progress.end(), [](const QList<QVariant> &a) {
            return a.value(0).toInt() == 10 && a.value(1).toInt() == 40;
        }));

        qputenv("FAIL", "1");
        builder.start("fedora");
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), "No host set for kwin 6.7.5");
        qunsetenv("FAIL");

        /* short of memory: it waits, then a stop */
        QVERIFY(write(m_tmp.filePath("meminfo"), "MemTotal: 33554432 kB\nMemAvailable: 4194304 kB\n"));
        builder.setMemoryPoll(50);
        builder.start("beta");
        QVERIFY(builder.isWaiting());
        QCOMPARE(builder.availableMiB(), 4096);
        QVERIFY(builder.neededMiB() >= GuestToolsBuilder::memoryCapMiB());
        builder.cancel();
        QVERIFY(builder.wasStopped());
        QVERIFY(!builder.isRunning());
        /* memory comes back: it starts by itself */
        builder.start("beta");
        QVERIFY(builder.isWaiting());
        QVERIFY(write(m_tmp.filePath("meminfo"), "MemTotal: 33554432 kB\nMemAvailable: 33554432 kB\n"));
        QVERIFY(finished.wait(10000));
        QVERIFY(!builder.wasStopped());

        /* a stop while it builds */
        qputenv("GATE", m_tmp.filePath("gate").toUtf8());
        builder.start("beta");
        QTRY_VERIFY(builder.phase() == "compiling");
        builder.cancel();
        QVERIFY(finished.wait(10000));
        QVERIFY(builder.wasStopped());
        QCOMPARE(builder.error(), QString());
        qunsetenv("GATE");
    }

    /*
     * A real system, when HOSTKWIN_SYSTEM names a folder of rpm and dnf
     * that run on it (a test VM's, through ssh) and its os-release: what
     * HostKwin makes of it, against the builds of HOSTKWIN_OUT.  Skipped
     * otherwise.
     */
    void aRealSystem()
    {
        const QString dir = qEnvironmentVariable("HOSTKWIN_SYSTEM");
        if (dir.isEmpty()) {
            QSKIP("HOSTKWIN_SYSTEM not set: no real system to read");
        }
        qputenv("PATH", dir.toUtf8() + ":/usr/bin:/bin");
        const System system = System::read(dir + "/os-release", m_tmp.filePath("noproc"));
        const Recipe r = Recipe::read(hostKwinDir(), system.plasma(), system.fedora);
        const QList<Build> list = builds(qEnvironmentVariable("HOSTKWIN_OUT"));
        const Status s = status(system, r, list, Record::read(Record::path(qEnvironmentVariable("HOSTKWIN_OUT"))));
        QStringList kwin;
        for (const Package &p : system.kwin) {
            kwin << p.nevra() + " from " + p.repo;
        }
        qInfo().noquote() << "fedora" << system.fedora << "plasma" << system.plasma() << "from"
                          << system.package("plasma-workspace").repo;
        qInfo().noquote() << "kwin:" << kwin.join(", ");
        qInfo().noquote() << "locks:" << system.locks;
        qInfo().noquote() << "recipe: set" << r.series << "for Fedora" << r.fedora;
        qInfo().noquote() << "builds:" << list.size() << "status:" << s.summary()
                          << "owner" << int(s.owner) << "offer" << int(s.offer)
                          << "differs" << s.differs << "outdated" << s.outdated;
        QString why;
        qInfo().noquote() << "canInstall" << s.canInstall(&why) << why;
        qInfo().noquote() << "canRevert" << s.canRevert(&why) << why;
        QVERIFY(system.isPlasmaHost());
        QCOMPARE(system.fedora, "44");
        if (!qEnvironmentVariable("HOSTKWIN_EXPECT").isEmpty()) {
            QCOMPARE(s.summary(), qEnvironmentVariable("HOSTKWIN_EXPECT"));
        }
    }

    /* In BuildManager's runs: its own part, and Update All only while Vitrine's KWin is installed */
    void inTheBuilds()
    {
        const QString src = m_tmp.filePath("kwinsrc");
        QVERIFY(QDir().mkpath(src));
        for (const char *name : {"install.sh", "components.conf"}) {
            QVERIFY(QFile::copy(hostKwinDir() + '/' + name, src + '/' + name));
        }
        QVERIFY(write(src + "/build.sh", "#!/bin/bash\necho 'vitrine-build: phase: building the packages'\n"
                                         "echo 'vitrine-build: built: /out/x'\n"));
        Builds::Places places;
        places.kwinDir = src;
        places.kwin = status(host(), recipe(), {}, {});
        HostKwinBuilder kwin;
        kwin.setSourceDir(src);
        QVERIFY(write(m_tmp.filePath("meminfo2"), "MemTotal: 33554432 kB\nMemAvailable: 33554432 kB\n"));
        kwin.setMeminfo(m_tmp.filePath("meminfo2"));
        BuildManager m;
        m.setPlaces(places);
        m.setBuilders(StackBuilder::instance(), GuestToolsBuilder::instance(), &kwin);
        m.setHistoryFile(m_tmp.filePath("history.json"));
        m.setLogDir(m_tmp.filePath("logs"));
        QCOMPARE(m.components().size(), 1);
        QCOMPARE(m.status("host-kwin").state, Builds::Status::State::NotBuilt);
        /* optional: not in Update All */
        QVERIFY(m.outdated().isEmpty());
        QCOMPARE(m.plan({"host-kwin"}), QStringList{"host-kwin"});

        QSignalSpy finished(&m, &BuildManager::runFinished);
        m.build({"host-kwin"});
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished.last().value(0).toString(), QString());
        QCOMPARE(m.progress("host-kwin").state, BuildManager::State::Built);
        QVERIFY(m.log("host-kwin").contains("building the packages"));

        /* Vitrine's installed: kept up to date with the rest */
        const Build b = build();
        places.kwin = status(withVitrines(host("6.8.0", "6.8.0-1.fc44"), b), recipe("hl39-68", "h18"),
                             {b}, Record::of(b, QDateTime::currentDateTimeUtc()));
        m.setPlaces(places);
        QCOMPARE(m.status("host-kwin").state, Builds::Status::State::Outdated);
        QCOMPARE(m.outdated(), QStringList{"host-kwin"});
        QCOMPARE(Builds::summary(m.status("host-kwin").changes), "built for KWin 6.7.91");
        /* no set for this Plasma: nothing to build */
        places.kwin = status(host("6.7.5", "6.7.5-1.fc44", "fedora"), recipe(QString(), QString()), {}, {});
        m.setPlaces(places);
        QCOMPARE(m.status("host-kwin").state, Builds::Status::State::NoSources);
    }
};

QTEST_GUILESS_MAIN(TestHostKwin)
#include "test_hostkwin.moc"
