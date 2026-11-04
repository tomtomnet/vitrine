// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/gtkbase.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>
#include <memory>

#include "core/managerinstance.h"
#include "core/paths.h"
#include "core/quitsignals.h"
#include "core/stackbuilder.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/gtkloop.h"
#include "gtk/mainwindow.h"
#include "ui/qemudocs.h"

static constexpr char kAppId[] = "org.vitrine.Vitrine";

int main(int argc, char **argv)
{
    /* the launcher's activation token, which GTK takes from the environment as it starts */
    const QByteArray token = qgetenv("XDG_ACTIVATION_TOKEN");
    QCoreApplication app(argc, argv);
    QString error;

    QCoreApplication::setApplicationName("vitrine");
    QCoreApplication::setApplicationVersion(VITRINE_VERSION);
    /* every window of the app by its ID, the desktop file's name: GNOME matches the two */
    g_set_prgname(kAppId);
    g_set_application_name("Vitrine");

    if (!GtkLoop::supported(&error)) {
        fprintf(stderr, "vitrine-gtk: %s\n", qPrintable(error));
        return 1;
    }
    /*
     * One manager at a time, this app or the Qt app (vitrine): a later one
     * shows the first one's window, with its activation token, and ends
     * before GTK is even set up
     */
    ManagerInstance instance(Paths::runtimeDir());
    if (!instance.lock()) {
        if (instance.activateRunning(token)) {
            return 0;
        }
        fprintf(stderr, "vitrine-gtk: another instance is running but does not "
                        "answer on %s\n", qPrintable(instance.socketPath()));
        return 1;
    }
    GtkLoop loop(kAppId);
    switch (loop.start(&error)) {
    case GtkLoop::Start::Primary:
        break;
    case GtkLoop::Start::Remote:
        /* a vitrine-gtk of another runtime folder on this bus: it shows its window */
        return 0;
    case GtkLoop::Start::Failed:
        fprintf(stderr, "vitrine-gtk: cannot register on the session bus: %s\n",
                qPrintable(error));
        return 1;
    }
    /* the icon of a build tree's app, not installed */
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (!gtk_icon_theme_has_icon(theme, kAppId)) {
        gtk_icon_theme_add_search_path(theme, VITRINE_GTK_BUILD_ICONS);
    }
    gtk_window_set_default_icon_name(kAppId);

    /*
     * The core as the Qt app's main() sets it up.  Not yet: the desktop's
     * GPU for the VMs' Automatic choice (HostGpus::setDesktopDevice(), from
     * linux-dmabuf's feedback over GDK's Wayland connection, once the GTK
     * view brings that protocol): the GPU the firmware showed its screen
     * on meanwhile, the desktop's on a laptop with two.
     */
    VmStore store(Paths::vmsDir());
    /* VMs outlive the manager: find those still running */
    for (Vm *vm : store.vms()) {
        vm->runner()->attach(vm->args());
    }
    QObject::connect(&store, &VmStore::added, &store,
                     [](Vm *vm) { vm->runner()->attach(vm->args()); });
    /* load the QEMU documentation in the background now */
    QemuDocs::preferred();
    /* the builds of Vitrine's QEMU nothing runs or names any more, now and a minute after each build */
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

    /* the window, at the first activation: GTK is set up by then */
    std::unique_ptr<MainWindow> window;
    loop.setActivated([&]() {
        if (!window) {
            window = std::make_unique<MainWindow>(&loop, &store);
        }
        window->present();
    });
    QString listenError;
    if (!instance.listen(&listenError)) {
        qWarning("Cannot listen on %s: %s", qPrintable(instance.socketPath()),
                 qPrintable(listenError));
    }
    /* the Qt app or another vitrine-gtk started meanwhile: this window, with its token */
    QObject::connect(&instance, &ManagerInstance::activationRequested, &store,
                     [&](const QByteArray &token) {
        if (window) {
            window->present(token);
        }
    });
    /*
     * SIGTERM (the session ending, kill), SIGINT (Ctrl+C) and SIGHUP (the
     * terminal closed) end vitrine as Close does, without its questions
     */
    QuitSignals quit;
    QObject::connect(&quit, &QuitSignals::received, &store, [&]() {
        if (window) {
            window->quitWithoutQuestions();
        } else {
            loop.quit(0);
        }
    });
    return loop.exec();
}
