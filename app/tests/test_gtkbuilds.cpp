// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "buildstandins.h"
#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/paths.h"
#include "core/stackbuilder.h"
#include "core/tasks.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/appbanners.h"
#include "gtk/buildswindow.h"
#include "gtk/gtkloop.h"
#include "gtk/guesttoolsdialog.h"
#include "gtk/guesttoolsinstall.h"
#include "gtk/guesttoolsstatus.h"
#endif

using GuestTools::Pending;
using Step = GuestToolsInstall::Step;

/* A guest the tools are for, but none in it: QEMU's firmware alone answers */
static const char kVm[] = "-name %1\n"
                          "#guest linux,desktop=kde\n"
                          "-machine q35\n"
                          "-m 128\n"
                          "-nodefaults\n"
                          "-display none\n";

/*
 * The GTK app's builds and guest tools, on a display no one sees: the
 * Builds window through a run of stand-ins for the build scripts; Install
 * Guest Tools for a VM that is off, and the install of one that runs,
 * which waits for its guest to shut down, gives up, is called off, or goes
 * on once the VM is off; the banners of Vitrine's QEMU, out of sight while
 * a screen shows; the guest tools' state of a VM and its button.
 */
class TestGtkBuilds : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;
    QTemporaryDir m_tmp;
    QStringList m_alerts;

    Gtk4::Dialog::Presenter presenter()
    {
        return [this](AdwDialog *dialog) {
            g_object_ref_sink(dialog);
            if (ADW_IS_ALERT_DIALOG(dialog)) {
                m_alerts << QString::fromUtf8(
                    adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)));
            }
            g_object_unref(dialog);
        };
    }

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

    /* @m on a host/ and a guest/ of stand-ins in folder @name */
    bool setUp(BuildManager &m, const QString &name)
    {
        const QString dir = m_tmp.filePath(name);
        Builds::Places places;
        places.hostDir = dir + "/host";
        places.guestDir = dir + "/guest";
        places.stackDir = dir + "/stack";
        places.rpmsDir = dir + "/rpms";
        places.guestRelease = "45";
        places.mediumImage = dir + "/media/vitrine-guest-tools-fc45.img";
        if (!write(places.hostDir + "/build.sh", kBuildSh, true) ||
            !write(places.hostDir + "/versions.conf",
                   "QEMU_URL=https://q/qemu.git\nQEMU_COMMIT=1\nVIRGL_URL=https://v/v.git\n"
                   "VIRGL_COMMIT=2\nRUTABAGA_URL=https://r/r.git\nRUTABAGA_COMMIT=3\n"
                   "PASST_URL=https://p/passt\nPASST_COMMIT=4\n") ||
            !write(places.hostDir + "/patches/qemu/0001-a.patch", "Subject: [PATCH] Make a\n") ||
            !QFile::copy(StackBuilder::hostDir() + "/components.conf",
                         places.hostDir + "/components.conf") ||
            !write(places.guestDir + "/build-rpms.sh", kRpmsSh, true) ||
            !write(places.guestDir + "/build-medium.sh", kMediumSh, true) ||
            !write(places.guestDir + "/rpm-build-inside.sh", "#!/bin/bash\n", true) ||
            !write(places.guestDir + "/tools/vitrine-guest-tools.spec",
                   "Name: vitrine-guest-tools\nVersion: 0.1.0\n") ||
            !write(places.guestDir + "/mesa/mesa.spec", "Name: mesa\nVersion: 26.2.3\n") ||
            !QFile::copy(GuestToolsBuilder::guestDir() + "/components.conf",
                         places.guestDir + "/components.conf")) {
            return false;
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
        m.setPlaces(places);
        m.setBuilders(stack, tools);
        m.setHistoryFile(dir + "/history.json");
        m.setLogDir(dir + "/logs");
        return true;
    }

    /* A medium of the guest tools for Fedora 44, as build-medium.sh leaves it */
    static bool writeMedium()
    {
        const QString dir = GuestTools::dataDir();
        return write(dir + "/vitrine-guest-tools-fc44.img", QByteArray(1 << 16, '\0')) &&
               write(dir + "/vitrine-guest-tools-fc44.json",
                     R"({"label": "VITRINETOOL", "mediumId": "32fe6c83b34e87e8", "fedora": "44",
                         "tools": "0.1.0-1.fc44", "mesa": "26.2.6-1.xe.fc44", "packages": []})");
    }

    /* What a Fedora 44 KDE guest says it runs */
    static GuestTools::System fedora44()
    {
        return {"fedora", "44", "Fedora Linux 44 (KDE Plasma Desktop Edition)"};
    }

    /* QEMU for a VM that runs: $VITRINE_TEST_QEMU, else the system's */
    static QString testQemu()
    {
        const QString env = qEnvironmentVariable("VITRINE_TEST_QEMU");
        return env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64") : env;
    }

    /* The widget named @name under @widget, depth first */
    static GtkWidget *named(GtkWidget *widget, const char *name)
    {
        if (g_strcmp0(gtk_widget_get_name(widget), name) == 0) {
            return widget;
        }
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            if (GtkWidget *found = named(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QString error;

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
        qputenv("PATH", (bin + ':' + qEnvironmentVariable("PATH")).toUtf8());
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        m_loop = new GtkLoop("org.vitrine.Vitrine");
        QCOMPARE(m_loop->start(&error), GtkLoop::Start::Primary);
    }

    void cleanupTestCase()
    {
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    void init()
    {
        m_alerts.clear();
    }

    void cleanup()
    {
        for (const char *name : {"QEMU_HOLD", "VITRINE_HOST_DIR"}) {
            qunsetenv(name);
        }
        GuestToolsInstall::setShutdownWait(5 * 60 * 1000);
        TaskList::instance()->clearFinished();
    }

    /* The window: its components and their state, a component's page, through a run */
    void buildsWindow()
    {
        BuildManager m;
        QVERIFY(setUp(m, "window"));
        GtkWidget *parent = gtk_window_new();
        auto window = std::make_unique<BuildsWindow>(GTK_WINDOW(parent), &m);

        window->present();
        QCOMPARE(window->items(), QStringList({"", "virglrenderer", "rutabaga_gfx", "passt", "qemu",
                                               "tools", "mesa", "medium", "release:44"}));
        QCOMPARE(window->runText(), QString("%1 components to build.").arg(m.outdated().size()));
        QCOMPARE(window->itemSubtitle("qemu"), "Not built");
        QVERIFY(window->memoryText().isEmpty());

        /* a component: its Build, its details, its patches */
        window->select("qemu");
        QCOMPARE(window->selected(), "qemu");
        QCOMPARE(window->buildLabel(), "Build QEMU");
        QVERIFY(window->canBuild());
        QVERIFY2(window->detailsText().contains("QEMU — not built yet."),
                 qPrintable(window->detailsText()));
        QCOMPARE(window->patchRows(), QStringList({"1. Make a"}));
        /* a group: the Overview, which shows them */
        window->select("guest");
        QCOMPARE(window->selected(), "");
        /* another release's guest tools, by their medium: selected, they show instead, from
           their medium; those shown before come after them, as the older ones */
        QCOMPARE(window->itemSubtitle("release:44"), "Not built");
        window->select("release:44");
        QTRY_COMPARE(m.guestRelease(), "44");
        QTRY_COMPARE(window->selected(), "medium");
        QCOMPARE(window->items(), QStringList({"", "virglrenderer", "rutabaga_gfx", "passt", "qemu",
                                               "release:45", "tools", "mesa", "medium"}));
        window->select("release:45");
        QTRY_COMPARE(m.guestRelease(), "45");
        QTRY_COMPARE(window->selected(), "medium");
        window->select("qemu");

        /* a run: its state as it goes, the log as it comes */
        qputenv("QEMU_HOLD", m_tmp.filePath("window/go").toUtf8());
        QSignalSpy finished(&m, &BuildManager::runFinished);
        window->buildSelected();
        QTRY_VERIFY_WITH_TIMEOUT(m.progress("qemu").done == 2, 10000);
        QTRY_COMPARE(window->itemSubtitle("qemu"), "Compiling QEMU, 50%");
        QCOMPARE(window->itemSubtitle("virglrenderer"), "Built");
        QCOMPARE(window->runText(), "Building QEMU (4 of 4).");
        QVERIFY(!window->canBuild());
        /* the release shown stays until the run is over */
        window->select("release:44");
        QTest::qWait(100);
        QCOMPARE(m.guestRelease(), "45");
        QCOMPARE(window->selected(), "qemu");
        QTRY_VERIFY2(window->logText().contains("[2/4] cc r.c"), qPrintable(window->logText()));
        QVERIFY(write(m_tmp.filePath("window/go"), "\n"));
        QVERIFY(finished.wait(10000));
        QTRY_VERIFY(!window->itemSubtitle("qemu").startsWith("Compiling"));
        QVERIFY(window->logText().contains("[4/4] link qemu-system-x86_64"));

        /* the window goes with its parent; then this side does */
        gtk_window_destroy(GTK_WINDOW(parent));
        window.reset();
        QTest::qWait(50);
    }

    /* A VM that is off: the medium to build first, then Install starts it with the tools */
    void installIntoAVmThatIsOff()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("off/vm.args"), QString(kVm).arg("Off").toUtf8()));
        GuestTools::setSystem("off", fedora44());
        QVERIFY(QDir(GuestTools::dataDir()).removeRecursively() ||
                !QFileInfo::exists(GuestTools::dataDir()));
        GuestToolsBuilder::instance()->checkMedium();
        VmStore store(dir.path());
        Vm *vm = store.find("off");
        QVERIFY(vm);
        QList<Vm *> started;
        std::shared_ptr<GuestToolsInstall> install;
        QStringList builds;
        const auto start = [&](Vm *v, bool mediumOnly, bool snapshot) {
            install = std::make_shared<GuestToolsInstall>(v, [&](Vm *s) { started << s; });
            install->start(mediumOnly, snapshot);
            return install;
        };

        /* no medium yet: the banner says so, with Build…, which opens the Builds window */
        auto *dialog = new GuestToolsDialog(
            vm, nullptr, start, [&](const QString &c, const QString &r) { builds << c + ' ' + r; },
            presenter());
        QVERIFY2(dialog->buildBannerText().contains(
                     "The guest tools for Fedora 44 are not built yet."),
                 qPrintable(dialog->buildBannerText()));
        QVERIFY(!dialog->canInstall());
        QVERIFY(!dialog->canAttach());
        dialog->forceClose();
        QTest::qWait(50);

        /* the medium there: Install, which starts the VM with it */
        QVERIFY(writeMedium());
        GuestToolsBuilder::instance()->checkMedium();
        dialog = new GuestToolsDialog(vm, nullptr, start, {}, presenter());
        QCOMPARE(dialog->installLabel(), "Install");
        QVERIFY(dialog->canInstall());
        QVERIFY(dialog->canAttach());
        QVERIFY(dialog->warningText().isEmpty());
        QVERIFY2(dialog->text().contains("They install:") &&
                     dialog->text().contains("• Mesa 26.2.6"),
                 qPrintable(dialog->text()));
        dialog->go(false);
        QVERIFY(install);
        QCOMPARE(started, QList<Vm *>{vm});
        QVERIFY(install->succeeded());
        QCOMPARE(GuestTools::pending("off"), Pending::Bootstrap);
        /* done: it closed */
        QVERIFY(dialog->isClosed());
        QTest::qWait(50);
        GuestTools::setPending("off", Pending::None);
        GuestTools::setSystem("off", {});
    }

    /*
     * A VM that runs: its guest is asked to shut down, and the install
     * waits for it - this one never does, no guest answers the power
     * button: it gives up after a while, the install pending for the next
     * start; called off, nothing is pending; once the VM is off, it starts
     * again with the medium
     */
    void installIntoAVmThatRuns()
    {
        if (!QFileInfo(testQemu()).isExecutable()) {
            QSKIP("no QEMU, set VITRINE_TEST_QEMU");
        }
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("on/vm.args"), QString(kVm).arg("On").toUtf8()));
        QVERIFY(writeMedium());
        GuestToolsBuilder::instance()->checkMedium();
        GuestTools::setSystem("on", fedora44());
        Paths::setQemuBinary(testQemu());
        VmStore store(dir.path());
        Vm *vm = store.find("on");
        QVERIFY(vm);
        QList<Vm *> started;
        const auto make = [&]() {
            return std::make_shared<GuestToolsInstall>(vm, [&](Vm *s) { started << s; });
        };
        vm->runner()->start(vm->args());
        QTRY_COMPARE_WITH_TIMEOUT(vm->runner()->state(), VmRunner::State::Running, 20000);

        /* given up: after a while, the install pending, no restart */
        GuestToolsInstall::setShutdownWait(500);
        std::shared_ptr<GuestToolsInstall> install = make();
        install->start(false, false);
        QCOMPARE(install->step(), Step::ShuttingDown);
        QVERIFY(install->task() && install->task()->isRunning());
        QCOMPARE(GuestTools::pending("on"), Pending::Bootstrap);
        QTRY_COMPARE_WITH_TIMEOUT(install->step(), Step::Done, 5000);
        QVERIFY(!install->succeeded());
        QVERIFY(!install->failed());
        QVERIFY2(install->progress().contains("did not shut down"),
                 qPrintable(install->progress()));
        QCOMPARE(GuestTools::pending("on"), Pending::Bootstrap);
        QVERIFY(started.isEmpty());

        /* called off: nothing pending, its task gone */
        GuestToolsInstall::setShutdownWait(5 * 60 * 1000);
        install = make();
        install->start(false, false);
        QCOMPARE(install->step(), Step::ShuttingDown);
        const QPointer<Task> task(install->task());
        install->cancel();
        QVERIFY(install->cancelled());
        QCOMPARE(GuestTools::pending("on"), Pending::None);
        QTRY_VERIFY(!task);

        /* off at last: the VM starts again, with the medium only */
        install = make();
        install->start(true, false);
        QCOMPARE(install->step(), Step::ShuttingDown);
        QCOMPARE(GuestTools::pending("on"), Pending::Medium);
        vm->runner()->forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 15000);
        QCOMPARE(started.first(), vm);
        QVERIFY(install->succeeded());
        QVERIFY(install->task() && install->task()->state() == Task::State::Done);

        GuestTools::setPending("on", Pending::None);
        GuestTools::setSystem("on", {});
        Paths::setQemuBinary({});
    }

    /* Vitrine's QEMU not built: the banner says so, its Build… opens the Builds window on QEMU */
    void stackBanner()
    {
        const QString host = m_tmp.filePath("banner/host");
        QVERIFY(write(host + "/build.sh", kBuildSh, true));
        QVERIFY(write(host + "/versions.conf",
                      "QEMU_URL=https://q/qemu.git\nQEMU_COMMIT=1\nVIRGL_URL=https://v/v.git\n"
                      "VIRGL_COMMIT=2\nRUTABAGA_URL=https://r/r.git\nRUTABAGA_COMMIT=3\n"
                      "PASST_URL=https://p/passt\nPASST_COMMIT=4\n"));
        QVERIFY(QFile::copy(StackBuilder::hostDir() + "/components.conf",
                            host + "/components.conf"));
        qputenv("VITRINE_HOST_DIR", QFile::encodeName(host));
        QCOMPARE(StackBuilder::state(), StackBuilder::State::NotBuilt);
        GtkWidget *view = adw_toolbar_view_new();
        g_object_ref_sink(view);
        QString shown;
        {
            AppBanners banners;
            banners.showBuilds = [&](const QString &component) { shown = component; };
            banners.addTo(ADW_TOOLBAR_VIEW(view));
            QVERIFY2(banners.stackText().startsWith("Vitrine's QEMU is not built yet."),
                     qPrintable(banners.stackText()));
            QCOMPARE(banners.stackButton(), "Build…");
            banners.activateStack();
            QCOMPARE(shown, "qemu");
            /* a VM's screen in sight: out of the way at once */
            GtkWidget *banner = named(view, "stackBanner");
            QVERIFY(banner);
            QVERIFY(gtk_widget_get_visible(gtk_widget_get_parent(banner)));
            banners.setScreenShown(ADW_TOOLBAR_VIEW(view), true);
            QVERIFY(!gtk_widget_get_visible(gtk_widget_get_parent(banner)));
            banners.setScreenShown(ADW_TOOLBAR_VIEW(view), false);
            QVERIFY(gtk_widget_get_visible(gtk_widget_get_parent(banner)));
        }
        g_object_unref(view);
    }

    /* A VM whose guest had no tools when it last ran: offered, with Install Guest Tools… */
    void guestToolsStatus()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("st/vm.args"),
                      (QString(kVm).arg("Status") + "-device virtio-vga-gl,blob=on\n").toUtf8()));
        QVERIFY(writeMedium());
        GuestToolsBuilder::instance()->checkMedium();
        QSettings(Paths::settingsPath(), QSettings::IniFormat).setValue("guesttools/last/st",
                                                                         "not-installed");
        GuestTools::setSystem("st", fedora44());
        VmStore store(dir.path());
        Vm *vm = store.find("st");
        QVERIFY(vm);
        GuestToolsStatus status;
        Vm *asked = nullptr;
        status.install = [&](Vm *v) { asked = v; };

        status.setVm(vm);
        QVERIFY2(status.text().startsWith(
                     "The guest tools were not installed when this VM last ran"),
                 qPrintable(status.text()));
        QCOMPARE(status.button(), "Install Guest Tools…");
        QVERIFY(gtk_widget_get_visible(status.homeWidget()));
        status.activate();
        QCOMPARE(asked, vm);
        /* pending: Cancel takes it back */
        GuestToolsMonitor::of(vm)->setPending(Pending::Bootstrap);
        QTRY_COMPARE(status.button(), "Cancel");
        status.activate();
        QCOMPARE(GuestTools::pending("st"), Pending::None);
        /* none shown */
        status.setVm(nullptr);
        QVERIFY(!gtk_widget_get_visible(status.homeWidget()));
        GuestTools::setSystem("st", {});
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkbuilds-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkBuilds test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkbuilds.moc"
