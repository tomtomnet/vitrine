// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>

#include "core/guestshutdown.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/guesttoolsbuildbanner.h"
#include "ui/guesttoolsdialog.h"
#include "ui/shutdownnotice.h"

#include "activation.h"
#include "guesttoolsstandins.h"

#include <csignal>

using namespace GuestTools;

/* Runs $VITRINE_TEST_QEMU, else the qemu-system-x86_64 in PATH, headless */
static QString testQemu()
{
    const QString env = qEnvironmentVariable("VITRINE_TEST_QEMU");
    return env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64") : env;
}

/* A guest the tools are for, but none in it: QEMU's firmware alone answers */
static const char kVm[] = "-name Tools Test\n"
                          "#guest linux,desktop=kde\n"
                          "-machine q35\n"
                          "-m 128\n"
                          "-nodefaults\n"
                          "-display none\n";

/*
 * Install Guest Tools never stands between the user and the VM's screen,
 * where the guest may ask what to do as it is asked to shut down (Plasma's
 * logout screen): not modal, out of sight while the guest shuts down, no
 * window of its own coming up meanwhile, one per VM.  And the status bar
 * says how Shut Down reached the guest.
 */
class TestGuestToolsDialog : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    VmStore *m_store = nullptr;
    QList<Vm *> m_started;
    QWidget *m_window = nullptr;

    Vm *vm() const { return m_store->find("tools"); }
    /* with a qcow2 disk: a snapshot before the install */
    Vm *diskVm() const { return m_store->find("disk"); }
    /* the same, with a stand-in for qemu-img whose listing takes its time */
    Vm *slowVm() const { return m_store->find("slow"); }

    static QList<GuestToolsDialog *> dialogs()
    {
        QList<GuestToolsDialog *> list;
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (auto *d = qobject_cast<GuestToolsDialog *>(top)) {
                list << d;
            }
        }
        return list;
    }

    /* The windows of the app on the screen but the main window's stand-in */
    QStringList shownWindows() const
    {
        QStringList shown;
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top != m_window && top->isVisible()) {
                shown << top->metaObject()->className();
            }
        }
        return shown;
    }

    /* What @widget says: the texts of its labels in sight */
    static QString textOf(const QWidget *widget)
    {
        QStringList texts;
        for (const QLabel *label : widget->findChildren<QLabel *>()) {
            if (label->isVisibleTo(widget->window()) && !label->text().isEmpty()) {
                texts << label->text();
            }
        }
        return texts.join(' ');
    }

    static QPushButton *button(QWidget *dialog, const QString &text)
    {
        for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
            if (b->text().remove('&') == text && b->isVisible()) {
                return b;
            }
        }
        return nullptr;
    }

    void writeMedium()
    {
        QDir().mkpath(dataDir());
        const QString image = dataDir() + "/vitrine-guest-tools-fc44.img";
        QFile img(image), json(dataDir() + "/vitrine-guest-tools-fc44.json");
        QVERIFY(img.open(QIODevice::WriteOnly));
        img.write(QByteArray(1 << 20, '\0'));
        QVERIFY(json.open(QIODevice::WriteOnly));
        json.write(R"({"label": "VITRINETOOL", "mediumId": "32fe6c83b34e87e8", "fedora": "44",
                      "tools": "0.1.0-1.fc44", "packages": []})");
    }

    static bool writeFile(const QString &path, const QByteArray &data, bool executable = false)
    {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            return false;
        }
        file.close();
        return !executable ||
               file.setPermissions(file.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
    }

    /*
     * The guest tools' builder making the medium again, with stand-ins for
     * its scripts that hold the first package until releaseBuild(@name)
     */
    GuestToolsBuilder *startHeldBuild(const QString &name)
    {
        const QString dir = m_tmp.filePath(name);
        const QByteArray rpms = QByteArray(kRpmsStandIn).replace("@HASH@", kInputsHash);
        if (!writeFile(dir + "/guest/tools/vitrine-guest-tools.spec",
                       "Name: vitrine-guest-tools\nVersion: 0.1.0\n") ||
            !writeFile(dir + "/guest/tools/install", "#!/bin/bash\n", true) ||
            !writeFile(dir + "/guest/mesa/mesa.spec", "Name: mesa\nVersion:        26.2.3\nRelease: 1\n") ||
            !writeFile(dir + "/guest/kwin/kwin.spec", "Name: kwin\nVersion: %{kf6_version}\n") ||
            !writeFile(dir + "/guest/rpm-build-inside.sh", "#!/bin/bash\n", true) ||
            !writeFile(dir + "/guest/build-rpms.sh", rpms, true) ||
            !writeFile(dir + "/guest/build-medium.sh", kMediumStandIn, true) ||
            !writeFile(dir + "/meminfo", "MemAvailable:   64000000 kB\n")) {
            return nullptr;
        }
        qputenv("GATE", ("tools:" + dir + "/go").toUtf8());
        GuestToolsBuilder *b = GuestToolsBuilder::instance();
        b->setGuestDir(dir + "/guest");
        b->setRpmsDir(dir + "/rpms");
        b->setCacheDir(dir + "/cache");
        b->setMeminfo(dir + "/meminfo");
        b->setMemoryPoll(50);
        b->start();
        return b->isRunning() ? b : nullptr;
    }

    void releaseBuild(const QString &name)
    {
        QVERIFY(writeFile(m_tmp.filePath(name + "/go"), "go"));
        qunsetenv("GATE");
    }

    /* @machine (vm() by default) running, its dialog open, Restart and Install clicked */
    GuestToolsDialog *restartAndInstall(Vm *machine = nullptr)
    {
        Vm *v = machine ? machine : vm();
        v->runner()->start(v->args());
        if (!QTest::qWaitFor([v]() { return v->runner()->state() == VmRunner::State::Running; },
                             20000)) {
            return nullptr;
        }
        GuestToolsDialog::run(m_window, v);
        GuestToolsDialog *dialog = GuestToolsDialog::of(v);
        QPushButton *install = dialog ? button(dialog, "Restart and Install") : nullptr;
        if (!install || !install->isEnabled()) {
            return nullptr;
        }
        install->click();
        return dialog;
    }

private slots:
    void initTestCase()
    {
        /* what the offscreen windows cannot do, said for each one */
        static const QtMessageHandler previous = qInstallMessageHandler(
            [](QtMsgType type, const QMessageLogContext &context, const QString &message) {
            if (!message.startsWith("This plugin does not support")) {
                previous(type, context, message);
            }
        });
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_tmp.isValid());
        if (!QFileInfo(testQemu()).isExecutable()) {
            QSKIP("no QEMU build, set VITRINE_TEST_QEMU");
        }
        Paths::setQemuBinary(testQemu());
        QDir(dataDir()).removeRecursively();
        writeMedium();
        QVERIFY(medium().isValid());
        QDir().mkpath(m_tmp.filePath("vms/tools"));
        QFile args(m_tmp.filePath("vms/tools/vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly));
        args.write(kVm);
        args.close();
        /* and one with a qcow2 disk */
        QDir().mkpath(m_tmp.filePath("vms/disk"));
        QFile diskArgs(m_tmp.filePath("vms/disk/vm.args"));
        QVERIFY(diskArgs.open(QIODevice::WriteOnly));
        diskArgs.write(QByteArray(kVm).replace("Tools Test", "Disk Test") +
                       "-drive file=disk.qcow2,format=qcow2,if=virtio\n");
        diskArgs.close();
        if (!Paths::qemuImg().isEmpty()) {
            QProcess img;
            img.start(Paths::qemuImg(), {"create", "-q", "-f", "qcow2",
                                         m_tmp.filePath("vms/disk/disk.qcow2"), "16M"});
            QVERIFY(img.waitForFinished() && img.exitCode() == 0);
        }
        /* and one whose qemu-img is a stand-in, beside its QEMU's: snapshots
           at once, a listing that takes 30 s */
        const QString standIn = m_tmp.filePath("standin");
        QVERIFY(writeFile(standIn + "/qemu-system-x86_64", "#!/bin/sh\nexit 1\n", true));
        QVERIFY(writeFile(standIn + "/qemu-img",
                          "#!/bin/sh\n"
                          "case $1 in\n"
                          "info) echo $$ > \"$(dirname \"$0\")/info.pid\"; exec sleep 30 ;;\n"
                          "esac\n",
                          true));
        QVERIFY(writeFile(m_tmp.filePath("vms/slow/vm.args"),
                          "#qemu " + QFile::encodeName(standIn) + "/qemu-system-x86_64\n" +
                              QByteArray(kVm).replace("Tools Test", "Slow Test") +
                              "-drive file=disk.qcow2,format=qcow2,if=virtio\n"));
        QVERIFY(writeFile(m_tmp.filePath("vms/slow/disk.qcow2"), {}));
        m_store = new VmStore(m_tmp.filePath("vms"), this);
        QVERIFY(vm());
        QVERIFY(diskVm());
        QVERIFY(slowVm());
        GuestToolsDialog::setStarter([this](Vm *v) { m_started << v; });
        m_window = new QWidget;
        m_window->resize(800, 600);
        m_window->show();
    }

    void cleanupTestCase()
    {
        for (Vm *v : m_store ? m_store->vms() : QList<Vm *>()) {
            v->runner()->forceOff();
            const bool off = QTest::qWaitFor([v]() { return !v->runner()->isActive(); }, 15000);
            Q_UNUSED(off);
            setPending(v->id(), Pending::None);
        }
        delete m_window;
        QDir(dataDir()).removeRecursively();
        Paths::setQemuBinary({});
    }

    void cleanup()
    {
        for (Vm *v : m_store->vms()) {
            if (v->runner()->isActive()) {
                v->runner()->forceOff();
                QTRY_VERIFY_WITH_TIMEOUT(!v->runner()->isActive(), 15000);
            }
            setPending(v->id(), Pending::None);
        }
        for (GuestToolsDialog *d : dialogs()) {
            delete d;
        }
        GuestToolsDialog::setShutdownWait(5 * 60 * 1000);
        m_started.clear();
    }

    /* Not modal, one per VM */
    void notModal()
    {
        GuestToolsDialog::run(m_window, vm());
        GuestToolsDialog *dialog = GuestToolsDialog::of(vm());
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        QVERIFY(!dialog->isModal());
        QCOMPARE(dialog->windowModality(), Qt::NonModal);
        QVERIFY(!QApplication::activeModalWidget());
        /* its own, again */
        GuestToolsDialog::run(m_window, vm());
        QCOMPARE(dialogs().size(), 1);
        /* the VM off: Install, which starts it */
        QVERIFY(button(dialog, "Install"));
        QVERIFY(!button(dialog, "Restart and Install"));
    }

    /*
     * Restart and Install: out of sight at once, for as long as the guest
     * takes to shut down - this one never does, as no guest answers the
     * power button: nothing of the app comes up meanwhile; once it is off,
     * the VM starts again with the tools, and the dialog is gone
     */
    void outOfTheWay()
    {
        GuestToolsDialog *dialog = restartAndInstall();
        QVERIFY(dialog);
        QVERIFY(dialog->isHidden());
        QVERIFY(shownWindows().isEmpty());
        QCOMPARE(pending(vm()->id()), Pending::Bootstrap);
        /* no agent in the guest: the power button, which the guest may answer on its screen */
        QTRY_COMPARE(vm()->runner()->shutdownWay(), GuestShutdown::Way::PowerButton);
        QTest::qWait(1000);
        QVERIFY2(shownWindows().isEmpty(), qPrintable(shownWindows().join(", ")));
        QVERIFY(!QApplication::activeModalWidget());
        QVERIFY(m_started.isEmpty());

        /* asked for again: the same, waiting, not modal */
        GuestToolsDialog::run(m_window, vm());
        QCOMPARE(dialogs(), QList<GuestToolsDialog *>{dialog});
        QVERIFY(dialog->isVisible());
        QVERIFY(!dialog->isModal());
        QVERIFY(button(dialog, "Cancel"));
        dialog->hide();

        /* off at last (the user answered the guest, or forced it off) */
        const QPointer<GuestToolsDialog> guard(dialog);
        vm()->runner()->forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(m_started.size(), 1, 15000);
        QCOMPARE(m_started.first(), vm());
        QTRY_VERIFY(!guard);
        QCOMPARE(pending(vm()->id()), Pending::Bootstrap);
    }

    /* Cancel while the guest shuts down: no install, no restart */
    void cancelWhileWaiting()
    {
        GuestToolsDialog *dialog = restartAndInstall();
        QVERIFY(dialog);
        const QPointer<GuestToolsDialog> guard(dialog);
        GuestToolsDialog::run(m_window, vm());
        QPushButton *cancel = button(dialog, "Cancel");
        QVERIFY(cancel);
        cancel->click();
        QTRY_VERIFY(!guard);
        QCOMPARE(pending(vm()->id()), Pending::None);
        vm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QTest::qWait(200);
        QVERIFY(m_started.isEmpty());
    }

    /* The install cancelled with the banner's Cancel while it waits out of sight */
    void cancelledElsewhere()
    {
        GuestToolsDialog *dialog = restartAndInstall();
        QVERIFY(dialog);
        const QPointer<GuestToolsDialog> guard(dialog);
        GuestToolsBanner banner;
        banner.setVm(vm());
        QPushButton *cancel = button(&banner, "Cancel");
        QVERIFY(cancel);
        cancel->click();
        QTRY_VERIFY(!guard);
        QCOMPARE(pending(vm()->id()), Pending::None);
        vm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QTest::qWait(200);
        QVERIFY(m_started.isEmpty());
    }

    /*
     * A guest that never shuts down (the user left its question): the
     * dialog gives up after a while, the install pending for the next
     * start, and the VM does not start again whenever it stops later
     */
    void givesUp()
    {
        GuestToolsDialog::setShutdownWait(500);
        GuestToolsDialog *dialog = restartAndInstall();
        QVERIFY(dialog);
        const QPointer<GuestToolsDialog> guard(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(!guard, 5000);
        QVERIFY(shownWindows().isEmpty());
        QCOMPARE(pending(vm()->id()), Pending::Bootstrap);
        vm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QTest::qWait(200);
        QVERIFY(m_started.isEmpty());
    }

    /* QEMU gone by itself (a crash) is no shutdown: no restart */
    void noRestartAfterACrash()
    {
        GuestToolsDialog *dialog = restartAndInstall();
        QVERIFY(dialog);
        const QPointer<GuestToolsDialog> guard(dialog);
        QVERIFY(vm()->runner()->pid() > 0);
        ::kill(pid_t(vm()->runner()->pid()), SIGKILL);
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QVERIFY(!vm()->runner()->errorString().isEmpty());
        QTRY_VERIFY(!guard);
        QVERIFY(m_started.isEmpty());
        QCOMPARE(pending(vm()->id()), Pending::Bootstrap);
    }

    /* The window closes once its VMs are off (Close > Shut Down): no restart */
    void stayOff()
    {
        GuestToolsDialog *dialog = restartAndInstall();
        QVERIFY(dialog);
        const QPointer<GuestToolsDialog> guard(dialog);
        GuestToolsDialog::stayOff(vm());
        QTRY_VERIFY(!guard);
        vm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QTest::qWait(200);
        QVERIFY(m_started.isEmpty());
        QCOMPARE(pending(vm()->id()), Pending::Bootstrap);
    }

    /*
     * Cancel while the snapshot is written (the banner's, with the VM off):
     * not before qemu-img is done with the disk, then no start
     */
    void cancelDuringSnapshot()
    {
        if (Paths::qemuImg().isEmpty()) {
            QSKIP("no qemu-img");
        }
        GuestToolsDialog *dialog = restartAndInstall(diskVm());
        QVERIFY(dialog);
        const QPointer<GuestToolsDialog> guard(dialog);
        bool cancelled = false;
        /* after the dialog's own: the snapshot is under way then */
        connect(diskVm()->runner(), &VmRunner::stateChanged, this, [&](VmRunner::State state) {
            if (state == VmRunner::State::Stopped && guard && !cancelled) {
                cancelled = true;
                guard->reject();
                QVERIFY(guard);
            }
        });
        diskVm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(cancelled, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!guard, 15000);
        QVERIFY(m_started.isEmpty());
        QCOMPARE(pending(diskVm()->id()), Pending::None);
        /* written whole */
        QProcess img;
        img.start(Paths::qemuImg(), {"snapshot", "-l", diskVm()->dir() + "/disk.qcow2"});
        QVERIFY(img.waitForFinished());
        QVERIFY(img.readAllStandardOutput().contains("Before guest tools"));
    }

    /*
     * Install with the VM off, a snapshot first: once the snapshot is
     * written the VM starts and the dialog goes, with the listing of the
     * snapshots that followed the snapshot still under way - here 30 s, the
     * stand-in for qemu-img's.  It ends with the dialog, without QProcess's
     * warning of a process destroyed while it runs, which came at the
     * starts of VMs given their tools this way.
     */
    void listingAtTheStart()
    {
        static QStringList warnings;
        static QtMessageHandler previous = nullptr;
        warnings.clear();
        previous = qInstallMessageHandler(
            [](QtMsgType type, const QMessageLogContext &context, const QString &message) {
            if (message.contains("QProcess")) {
                warnings << message;
            }
            previous(type, context, message);
        });
        const auto restore = qScopeGuard([]() { qInstallMessageHandler(previous); });
        const QString pidFile = m_tmp.filePath("standin/info.pid");
        QFile::remove(pidFile);

        GuestToolsDialog::run(m_window, slowVm());
        GuestToolsDialog *dialog = GuestToolsDialog::of(slowVm());
        QVERIFY(dialog);
        const QPointer<GuestToolsDialog> guard(dialog);
        QPushButton *install = button(dialog, "Install");
        QVERIFY(install && install->isEnabled());
        install->click();
        QTRY_VERIFY_WITH_TIMEOUT(!guard, 10000);
        QCOMPARE(m_started, QList<Vm *>{slowVm()});
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        /* the listing is over, if it got as far as the stand-in */
        QFile f(pidFile);
        if (f.open(QIODevice::ReadOnly)) {
            const pid_t pid = f.readAll().trimmed().toInt();
            QVERIFY(pid <= 0 || ::kill(pid, 0) != 0);
        }
    }

    /*
     * The medium made by hand while the dialog is open (guest/build-medium.sh
     * in a terminal): once vitrine is back in front, the dialog's banner and
     * Install follow it, not before
     */
    void mediumMadeByHand()
    {
        const QString image = dataDir() + "/vitrine-guest-tools-fc44.img";
        const QString manifest = dataDir() + "/vitrine-guest-tools-fc44.json";
        QVERIFY(QFile::remove(image) && QFile::remove(manifest));
        /* the medium as before for the next tests, seen gone meanwhile */
        const auto restore = qScopeGuard([this]() {
            writeMedium();
            GuestToolsBuilder::instance()->checkMedium();
        });
        /* as the main window does when vitrine comes back in front */
        GuestToolsBuilder::instance()->checkMedium();
        GuestToolsDialog::run(m_window, vm());
        GuestToolsDialog *dialog = GuestToolsDialog::of(vm());
        QVERIFY(dialog);
        auto *banner = dialog->findChild<GuestToolsBuildBanner *>();
        QPushButton *install = button(dialog, "Install");
        QVERIFY(banner && install);
        QVERIFY(!install->isEnabled());
        QVERIFY2(textOf(banner).contains("not built yet"), qPrintable(textOf(banner)));

        /* what build-medium.sh leaves, of other sources than the first one */
        QVERIFY(writeFile(image, QByteArray(1 << 20, '\0')));
        QVERIFY(writeFile(manifest, R"({"label": "VITRINETOOL", "mediumId": "a1b2c3d4e5f60718",
                                        "fedora": "44", "tools": "0.1.0-2.fc44", "packages": []})"));
        QVERIFY(!install->isEnabled());
        comeBack();
        QVERIFY(install->isEnabled());
        QVERIFY2(!textOf(banner).contains("not built yet"), qPrintable(textOf(banner)));
    }

    /* What the buttons say follows the VM, which may start or stop meanwhile */
    void followsTheVm()
    {
        GuestToolsDialog::run(m_window, vm());
        GuestToolsDialog *dialog = GuestToolsDialog::of(vm());
        QVERIFY(dialog && button(dialog, "Install"));
        vm()->runner()->start(vm()->args());
        QTRY_COMPARE_WITH_TIMEOUT(vm()->runner()->state(), VmRunner::State::Running, 20000);
        QVERIFY(button(dialog, "Restart and Install"));
        QVERIFY(button(dialog, "Restart and Install")->isEnabled());
        vm()->runner()->pause();
        QTRY_COMPARE(vm()->runner()->state(), VmRunner::State::Paused);
        QVERIFY(!button(dialog, "Restart and Install")->isEnabled());
        vm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QVERIFY(button(dialog, "Install") && button(dialog, "Install")->isEnabled());
    }

    /* The status bar: through which agent, or the button and the guest's question */
    void notice()
    {
        using Way = GuestShutdown::Way;
        QStatusBar bar;
        ShutdownNotice::follow(vm(), &bar);

        QVERIFY(ShutdownNotice::text("Desk", Way::None).isEmpty());
        QVERIFY(ShutdownNotice::text("Desk", Way::ToolsAgent).contains("guest tools"));
        QVERIFY(ShutdownNotice::text("Desk", Way::GuestAgent).contains("guest agent"));
        QVERIFY(ShutdownNotice::text("Desk", Way::PowerButton).contains("answer it on its screen"));
        QVERIFY(ShutdownNotice::timeout(Way::PowerButton) > ShutdownNotice::timeout(Way::GuestAgent));

        vm()->runner()->start(vm()->args());
        QTRY_COMPARE_WITH_TIMEOUT(vm()->runner()->state(), VmRunner::State::Running, 20000);
        vm()->runner()->powerdown();
        QTRY_COMPARE(bar.currentMessage(), ShutdownNotice::text(vm()->name(), Way::PowerButton));
        /* the request over (forced off): taken back */
        vm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QCOMPARE(bar.currentMessage(), QString());
        /* not what came after it */
        bar.showMessage("Something else");
        vm()->runner()->start(vm()->args());
        QTRY_COMPARE_WITH_TIMEOUT(vm()->runner()->state(), VmRunner::State::Running, 20000);
        vm()->runner()->forceOff();
        QTRY_VERIFY_WITH_TIMEOUT(!vm()->runner()->isActive(), 15000);
        QCOMPARE(bar.currentMessage(), "Something else");
    }
    /*
     * While a build makes the medium again, Install waits for it: the user
     * saw Install open during one, which would have put in the tools the
     * build replaces.  Install When Built, Attach Medium Only off; the VM
     * starts once the build is done
     */
    void installWhenBuilt()
    {
        GuestToolsBuilder *b = startHeldBuild("when-built");
        QVERIFY(b);
        GuestToolsDialog::run(m_window, vm());
        GuestToolsDialog *dialog = GuestToolsDialog::of(vm());
        QVERIFY(dialog);
        QPushButton *install = button(dialog, "Install When Built");
        QVERIFY(install);
        QVERIFY(install->isEnabled());
        QPushButton *medium = button(dialog, "Attach Medium Only");
        QVERIFY(medium);
        QVERIFY(!medium->isEnabled());
        install->click();
        QVERIFY(m_started.isEmpty());
        releaseBuild("when-built");
        QTRY_VERIFY_WITH_TIMEOUT(!b->isRunning(), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(m_started.size(), 1, 10000);
        QCOMPARE(m_started.first(), vm());
    }

    /* Install When Built, then Cancel: nothing starts once the build is done */
    void cancelWhenBuilt()
    {
        GuestToolsBuilder *b = startHeldBuild("cancel-built");
        QVERIFY(b);
        GuestToolsDialog::run(m_window, vm());
        GuestToolsDialog *dialog = GuestToolsDialog::of(vm());
        QVERIFY(dialog);
        QPushButton *install = button(dialog, "Install When Built");
        QVERIFY(install);
        install->click();
        dialog->reject();
        releaseBuild("cancel-built");
        QTRY_VERIFY_WITH_TIMEOUT(!b->isRunning(), 30000);
        QTest::qWait(500);
        QVERIFY(m_started.isEmpty());
    }
};

QTEST_MAIN(TestGuestToolsDialog)
#include "test_guesttoolsdialog.moc"
