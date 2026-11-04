// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QTimer>

#include <cstdio>

#include "core/hostgpus.h"
#include "core/managerinstance.h"
#include "core/paths.h"
#include "core/quitsignals.h"
#include "core/stackbuilder.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/desktopgpu.h"
#include "ui/icons.h"
#include "ui/mainwindow.h"
#include "ui/qemudocs.h"

int main(int argc, char **argv)
{
    /*
     * The desktop's scale as it is, 1.25 or 1.5 too: Qt 6's default, said
     * here because Qt's Wayland plugin takes the compositor's fractional
     * scale (wp_fractional_scale_v1) with this policy only, and rounds to a
     * whole one with any other.  QT_SCALE_FACTOR_ROUNDING_POLICY still wins.
     */
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);

    QApplication::setApplicationName("vitrine");
    QApplication::setApplicationDisplayName(QObject::tr("Vitrine"));
    QApplication::setApplicationVersion(VITRINE_VERSION);
    QApplication::setDesktopFileName("vitrine");
    /*
     * The windows' icon by its name in the icon theme, where installing
     * puts it: the compositor then draws it from the file.  Pixels sent
     * for an icon without a name (xdg-toplevel-icon) came out with red and
     * cyan fringes in KWin's title bar wherever the logo is light and half
     * transparent, as its glass is (Qt 6.11, KWin 6.7.91).  Not installed
     * anywhere: those pixels all the same.
     */
    QApplication::setWindowIcon(QIcon::fromTheme("vitrine", Icons::app()));

    /* one manager at a time, this app or vitrine-gtk: a second one shows the first one's window */
    ManagerInstance instance(Paths::runtimeDir());
    if (!instance.lock()) {
        if (instance.activateRunning(qgetenv("XDG_ACTIVATION_TOKEN"))) {
            return 0;
        }
        fprintf(stderr, "vitrine: another instance is running but does not "
                        "answer on %s\n", qPrintable(instance.socketPath()));
        return 1;
    }

    /* the GPU the desktop runs on, which the nested desktops render on by default */
    HostGpus::setDesktopDevice(DesktopGpu::mainDevice());

    VmStore store(Paths::vmsDir());
    /* VMs outlive the manager: find those still running */
    for (Vm *vm : store.vms()) {
        vm->runner()->attach(vm->args());
    }
    QObject::connect(&store, &VmStore::added, &store,
                     [](Vm *vm) { vm->runner()->attach(vm->args()); });
    /* load the QEMU documentation in the background now */
    QemuDocs::preferred();
    /*
     * The builds of Vitrine's QEMU nothing runs or names any more, now and
     * after each build: a minute after, once the VMs started meanwhile run
     * from the build they began with
     */
    const auto prune = [&store]() {
        QStringList keep = {Paths::customQemuBinary()};
        for (const Vm *vm : store.vms()) {
            keep << VmConfig::qemuBinary(vm->args());
        }
        StackBuilder::prune(Paths::stackDir(), keep);
    };
    prune();
    QObject::connect(StackBuilder::instance(), &StackBuilder::built, &store,
                     [&store, prune]() { QTimer::singleShot(60000, &store, prune); });

    MainWindow window(&store);
    QString error;
    if (!instance.listen(&error)) {
        qWarning("Cannot listen on %s: %s", qPrintable(instance.socketPath()), qPrintable(error));
    }
    QObject::connect(&instance, &ManagerInstance::activationRequested, &window,
                     [&window](const QByteArray &token) {
        if (!token.isEmpty()) {
            /* the Wayland plugin activates with it */
            qputenv("XDG_ACTIVATION_TOKEN", token);
        }
        window.bringToFront();
    });
    /*
     * SIGTERM (the session ending, kill), SIGINT (Ctrl+C) and SIGHUP (the
     * terminal closed) end vitrine as Close does, without its questions: a
     * crash otherwise, which the guests' nested sessions in the window
     * would see as their host gone, their KWin restarted several times
     * before they end.  A second one of the same ends vitrine at once.
     */
    QuitSignals quit;
    QObject::connect(&quit, &QuitSignals::received, &window, &MainWindow::quitWithoutQuestions);
    window.show();
    return app.exec();
}
