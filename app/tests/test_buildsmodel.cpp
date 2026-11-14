// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/buildcomponents.h"
#include "core/buildmanager.h"
#include "core/guesttoolsbuilder.h"
#include "core/stackbuilder.h"
#include "core/tasks.h"
#include "gtk/buildsmodel.h"
#include "gtk/markup.h"
#include "ui/buildtexts.h"

#include "buildstandins.h"

using BuildsModel::Group;
using BuildsModel::Look;

/*
 * What the GTK app's Builds window says (BuildsModel), in the Qt window's
 * words, from a build manager on stand-ins for host/build.sh and the guest
 * tools' scripts: each component's state before, during and after a run,
 * the groups', the buttons', the run's, the details of a component and its
 * patches.  No GTK: the words alone.  And the rich text of the texts both
 * apps share, as Pango markup.
 */
class TestBuildsModel : public QObject
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

    /* @m on a host/ and a guest/ of stand-ins in folder @name, the real components.conf */
    Builds::Places setUp(BuildManager &m, const QString &name)
    {
        const QString dir = m_tmp.filePath(name);
        Builds::Places places;
        places.hostDir = dir + "/host";
        places.guestDir = dir + "/guest";
        places.stackDir = dir + "/stack";
        places.rpmsDir = dir + "/rpms";
        places.guestRelease = "45";
        places.mediumImage = dir + "/media/vitrine-guest-tools-fc45.img";
        const bool ok =
            write(places.hostDir + "/build.sh", kBuildSh, true) &&
            write(places.hostDir + "/versions.conf",
                  "QEMU_URL=https://q/qemu.git\nQEMU_COMMIT=1\nVIRGL_URL=https://v/v.git\n"
                  "VIRGL_COMMIT=2\nRUTABAGA_URL=https://r/r.git\nRUTABAGA_COMMIT=3\n"
                  "PASST_URL=https://p/passt\nPASST_COMMIT=4\n") &&
            write(places.hostDir + "/patches/qemu/0001-a.patch", "Subject: [PATCH] Make a\n") &&
            QFile::copy(StackBuilder::hostDir() + "/components.conf",
                        places.hostDir + "/components.conf") &&
            write(places.guestDir + "/build-rpms.sh", kRpmsSh, true) &&
            write(places.guestDir + "/build-medium.sh", kMediumSh, true) &&
            write(places.guestDir + "/rpm-build-inside.sh", "#!/bin/bash\n", true) &&
            write(places.guestDir + "/tools/vitrine-guest-tools.spec",
                  "Name: vitrine-guest-tools\nVersion: 0.1.0\n") &&
            write(places.guestDir + "/mesa/mesa.spec", "Name: mesa\nVersion: 26.2.3\n") &&
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

    /* No size known yet, as the window has it while it counts */
    static qint64 unknownSize(const QString &)
    {
        return -1;
    }

    static QStringList labels(const BuildsModel::Details &d)
    {
        QStringList list;
        for (const BuildsModel::Property &p : d.properties) {
            list << p.label;
        }
        return list;
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
        for (const char *name : {"QEMU_HOLD", "HOST_FAIL", "GUEST_FAIL"}) {
            qunsetenv(name);
        }
        TaskList::instance()->clearFinished();
    }

    /* Nothing built yet: what each is, what there is to build, and how */
    void beforeABuild()
    {
        BuildManager m;
        QVERIFY(!setUp(m, "before").hostDir.isEmpty());

        /* no host desktop where host/kde offers none */
        QCOMPARE(BuildsModel::groups(m), (QList<Group>{Group::Host, Group::Guest}));
        QCOMPARE(BuildsModel::componentsOf(m, Group::Host),
                 QStringList({"virglrenderer", "rutabaga_gfx", "passt", "qemu"}));
        QCOMPARE(BuildsModel::componentsOf(m, Group::Guest),
                 QStringList({"tools", "mesa", "medium"}));
        QCOMPARE(BuildsModel::groupTitle(Group::Guest, "45"), "Guest Tools for Fedora 45");
        QCOMPARE(BuildsModel::groupTitle(Group::Host, "45"), "Vitrine's QEMU");
        /* the other release's guest tools, by their medium */
        QCOMPARE(m.otherGuestReleases(), QStringList({"44"}));
        QCOMPARE(BuildsModel::releaseId("44"), "release:44");
        QCOMPARE(BuildsModel::releaseOf("release:44"), "44");
        QVERIFY(BuildsModel::releaseOf("medium").isEmpty());
        const BuildsModel::Status other = BuildsModel::releaseStatus(m, "44");
        QCOMPARE(other.text, "Not built");
        QCOMPARE(other.look, Look::ToBuild);
        QVERIFY(BuildsModel::releaseBuilt(m, "44").isEmpty());
        /* what a release's tools bring, by desktop */
        QCOMPARE(BuildTexts::releaseContents({"tools", "mesa", "medium"}),
                 "the virtio-gpu driver and Mesa for every guest");
        QCOMPARE(BuildTexts::releaseContents({"tools", "mesa", "kwayland", "kwin", "mutter"}),
                 "the virtio-gpu driver and Mesa for every guest, Mutter for GNOME guests and "
                 "KWin with its libraries for KDE Plasma guests");
        QCOMPARE(BuildTexts::releaseContents({"tools", "mesa", "kwin"}),
                 "the virtio-gpu driver and Mesa for every guest and KWin with its libraries for "
                 "KDE Plasma guests");
        QVERIFY(BuildTexts::groupAbout("guest", "45", {"tools", "mesa", "mutter"})
                    .contains("and the medium itself: the virtio-gpu driver and Mesa for every "
                              "guest and Mutter for GNOME guests. Each Fedora release"));
        QCOMPARE(BuildsModel::title(m, "mesa"), "Mesa 26.2.3");
        QCOMPARE(BuildsModel::title(m, "medium"), "Guest tools medium");

        const BuildsModel::Status qemu = BuildsModel::status(m, "qemu");
        QCOMPARE(qemu.text, "Not built");
        QCOMPARE(qemu.look, Look::ToBuild);
        QVERIFY(qemu.fraction < 0);
        QVERIFY(BuildsModel::built(m, "qemu").isEmpty());
        QCOMPARE(BuildsModel::groupStatus(m, Group::Host), "Not built");

        /* a component's Build builds what it needs too; a group's, all of it */
        const BuildsModel::Action build = BuildsModel::build(m, "qemu");
        QCOMPARE(build.label, "_Build QEMU");
        QVERIFY(build.enabled);
        QCOMPARE(build.tooltip, "Builds virglrenderer, rutabaga_gfx, passt, QEMU");
        QCOMPARE(BuildsModel::build(m, "host").label, "_Build Vitrine's QEMU");
        QCOMPARE(BuildsModel::build(m, "guest").label, "_Build Guest Tools");
        QCOMPARE(BuildsModel::selection(m, "guest"), QStringList({"tools", "mesa", "medium"}));
        QVERIFY(BuildsModel::selection(m, "").isEmpty());
        QCOMPARE(BuildsModel::build(m, "").label, "_Build");
        QVERIFY(!BuildsModel::build(m, "").enabled);
        const BuildsModel::Action all = BuildsModel::buildAll(m);
        QCOMPARE(all.label, "Build _All");
        QVERIFY(all.enabled);
        QCOMPARE(BuildsModel::runText(m),
                 QString("%1 components to build.").arg(m.outdated().size()));
        QVERIFY(BuildsModel::memoryText(m).isEmpty());

        /* where it comes from, and its patches */
        const BuildsModel::Details d = BuildsModel::details(m, "qemu", unknownSize);
        QVERIFY2(Markup::plain(d.paragraphs.value(0)) == "QEMU — not built yet.",
                 qPrintable(d.paragraphs.value(0)));
        QCOMPARE(labels(d), QStringList({"Source", "Patches", "Built against", "Runs with"}));
        QCOMPARE(d.properties[0].link, "https://q/qemu");
        QCOMPARE(Markup::plain(d.properties[0].value), "q/qemu, commit 1");
        QCOMPARE(Markup::plain(d.properties[2].value), "virglrenderer, rutabaga_gfx");
        QCOMPARE(Markup::plain(d.properties[3].value), "passt");
        const QList<BuildsModel::PatchGroup> patches = BuildsModel::patchGroups(m, "qemu");
        QCOMPARE(patches.size(), 1);
        QCOMPARE(patches[0].title, "1 patch");
        QCOMPARE(patches[0].subjects, QStringList({"1. Make a"}));
        QVERIFY(BuildsModel::patchGroups(m, "virglrenderer").isEmpty());
    }

    /*
     * The Mesa for Flatpak apps (a guest/ with flatpak/): one of the guest
     * tools, before the medium, which a build of it brings; its title alone,
     * its runtimes in its source, and the medium made of it too
     */
    void flatpakMesa()
    {
        BuildManager m;
        const Builds::Places places = setUp(m, "flatpak");
        QVERIFY(!places.guestDir.isEmpty());
        QVERIFY(write(places.guestDir + "/flatpak/branches.conf",
                      "[25.08]\ncommit=279b0ea50eb42917af81a28d4582f11a5d18a39c\nmesa=26.2.7\n"
                      "patches=mesa/fix.patch\n"));
        QVERIFY(write(places.guestDir + "/mesa/fix.patch", "--- a\n+++ b\n"));
        QVERIFY(write(places.guestDir + "/build-flatpak.sh", "#!/bin/bash\n", true));
        m.refresh();

        QCOMPARE(BuildsModel::componentsOf(m, Group::Guest),
                 QStringList({"tools", "mesa", "flatpak", "medium"}));
        QCOMPARE(m.component("flatpak").kind, Builds::Kind::Flatpak);
        QCOMPARE(BuildsModel::title(m, "flatpak"), "Mesa for Flatpak apps");
        QCOMPARE(m.plan({"flatpak"}), QStringList({"flatpak", "medium"}));
        /* built: its Mesa versions, which its title does not show */
        Builds::Places built = places;
        built.flatpakDir = m_tmp.filePath("flatpak/data/flatpak");
        QVERIFY(write(built.flatpakDir + "/.inputs", "other\n"));
        QVERIFY(write(built.flatpakDir + "/branches", "25.08 26.2.7 279b0ea5\n"));
        m.setPlaces(built);
        QCOMPARE(m.status("flatpak").version, "Mesa 26.2.7 for 25.08");
        QCOMPARE(BuildsModel::title(m, "flatpak"), "Mesa for Flatpak apps");
        m.setPlaces(places);
        const BuildsModel::Details d = BuildsModel::details(m, "flatpak", unknownSize);
        QCOMPARE(labels(d), QStringList({"Source", "Patches"}));
        QCOMPARE(d.properties[0].link, "https://gitlab.com/freedesktop-sdk/freedesktop-sdk");
        QCOMPARE(Markup::plain(d.properties[0].value),
                 "gitlab.com/freedesktop-sdk/freedesktop-sdk's recipe, the one of Flathub's: "
                 "Mesa 26.2.7 for the runtimes 25.08 (commit 279b0ea50eb4)");
        const BuildsModel::Details medium = BuildsModel::details(m, "medium", unknownSize);
        QCOMPARE(labels(medium).value(0), "Made of");
        QCOMPARE(Markup::plain(medium.properties[0].value),
                 "the packages vitrine-guest-tools, Mesa; Mesa for Flatpak apps and the installer");
    }

    /* During a run: each component's part, how far the compiler is, the run's words */
    void duringARun()
    {
        BuildManager m;
        const Builds::Places places = setUp(m, "run");
        QVERIFY(!places.hostDir.isEmpty());
        qputenv("QEMU_HOLD", m_tmp.filePath("run/go").toUtf8());
        QSignalSpy finished(&m, &BuildManager::runFinished);

        m.build({"qemu"});
        QTRY_VERIFY_WITH_TIMEOUT(m.progress("qemu").done == 2, 10000);
        const BuildsModel::Status qemu = BuildsModel::status(m, "qemu");
        QCOMPARE(qemu.text, "Compiling QEMU, 50%");
        QCOMPARE(qemu.look, Look::Building);
        QCOMPARE(qemu.fraction, 0.5);
        QCOMPARE(BuildsModel::status(m, "virglrenderer").text, "Built");
        QCOMPARE(BuildsModel::groupStatus(m, Group::Host), "Building");
        QCOMPARE(BuildsModel::runText(m), "Building QEMU (4 of 4).");
        /* nothing else starts meanwhile */
        QVERIFY(!BuildsModel::buildAll(m).enabled);
        QVERIFY(!BuildsModel::build(m, "mesa").enabled);
        QVERIFY(BuildsModel::missingText(m).isEmpty());
        const BuildsModel::Details d = BuildsModel::details(m, "qemu", unknownSize);
        QVERIFY2(d.paragraphs.contains("Building: compiling QEMU."),
                 qPrintable(d.paragraphs.join('|')));

        QVERIFY(write(m_tmp.filePath("run/go"), "\n"));
        QVERIFY(finished.wait(10000));
        QCOMPARE(finished[0][0].toString(), "");
        /* over: where it is, how big (the stand-in's build says no date) */
        QVERIFY(!BuildsModel::status(m, "qemu").text.startsWith("Compiling"));
        QCOMPARE(m.durations("qemu").size(), 1);
        const BuildsModel::Details after =
            BuildsModel::details(m, "qemu", [](const QString &) { return qint64(3 << 20); });
        QVERIFY2(labels(after).contains("Output"), qPrintable(labels(after).join(", ")));
        for (const BuildsModel::Property &p : after.properties) {
            if (p.label == "Output") {
                QCOMPARE(p.path, places.stackDir + "/0123456789abcdef");
                QVERIFY2(Markup::plain(p.value).endsWith("— 3.0 MB"), qPrintable(p.value));
            }
        }
    }

    /* A failure: the component's own, the run's in its header */
    void failure()
    {
        BuildManager m;
        QVERIFY(!setUp(m, "fail").hostDir.isEmpty());
        qputenv("HOST_FAIL", "qemu");
        QSignalSpy finished(&m, &BuildManager::runFinished);

        m.build({"qemu"});
        QVERIFY(finished.wait(10000));
        const BuildsModel::Status qemu = BuildsModel::status(m, "qemu");
        QCOMPARE(qemu.text, "Failed: Compiling QEMU failed (status 2)");
        QCOMPARE(qemu.look, Look::Failed);
        QVERIFY2(BuildsModel::runText(m).startsWith("The last build failed: QEMU: Compiling QEMU "
                                                    "failed (status 2). "),
                 qPrintable(BuildsModel::runText(m)));
        const BuildsModel::Details d = BuildsModel::details(m, "qemu", unknownSize);
        QVERIFY2(d.paragraphs.contains("Its last build failed: Compiling QEMU failed (status 2)"),
                 qPrintable(d.paragraphs.join('|')));
    }

    /* The rich text both apps write, as Pango markup */
    void markup()
    {
        QCOMPARE(Markup::pango("<p><b>A</b> &amp; b</p><p>c <code>d &lt;e&gt;</code></p>"),
                 "<b>A</b> &amp; b\n\nc <tt>d &lt;e&gt;</tt>");
        QCOMPARE(Markup::pango("<p>They install:</p><ul><li>one</li><li>two</li></ul><p>End.</p>"),
                 QString("They install:\n\n• one\n• two\n\nEnd."));
        QCOMPARE(Markup::pango("Commands:<pre>sudo dnf a\nsudo dnf b</pre>"),
                 "Commands:\n\n<tt>sudo dnf a\nsudo dnf b</tt>");
        QCOMPARE(Markup::pango("a<br>b &nbsp;c, R&D"), "a\nb &#160;c, R&amp;D");
        QCOMPARE(Markup::pango("<a href=\"https://x/y\">See</a> the <span>rest</span>"),
                 "<a href=\"https://x/y\">See</a> the rest");
        QCOMPARE(Markup::plain("<p>x &lt;y&gt; &amp; <b>z</b></p>"), "x <y> & z");
    }
};

QTEST_GUILESS_MAIN(TestBuildsModel)
#include "test_buildsmodel.moc"
