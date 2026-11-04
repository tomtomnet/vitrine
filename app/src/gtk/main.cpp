// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/gtkbase.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

#include "core/managerinstance.h"
#include "core/paths.h"
#include "core/quitsignals.h"
#include "core/stackbuilder.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/gtkloop.h"
#include "ui/qemudocs.h"

static constexpr char kAppId[] = "org.vitrine.Vitrine";

/* The first window, bare: what the loop shows until the main window comes */
static GtkWidget *bareWindow(AdwApplication *app)
{
    GtkWidget *window = adw_application_window_new(GTK_APPLICATION(app));
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *page = adw_status_page_new();

    gtk_window_set_title(GTK_WINDOW(window), "Vitrine");
    gtk_window_set_default_size(GTK_WINDOW(window), 1060, 680);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), kAppId);
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "Vitrine");
    adw_status_page_set_description(ADW_STATUS_PAGE(page),
                                    "GTK 4 and libadwaita, on the core of vitrine");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), view);
    return window;
}

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

    GtkWidget *window = nullptr;
    loop.setActivated([&]() {
        if (!window) {
            window = bareWindow(loop.adw());
            /* null once closed */
            g_object_add_weak_pointer(G_OBJECT(window), reinterpret_cast<gpointer *>(&window));
        }
        gtk_window_present(GTK_WINDOW(window));
    });
    QString listenError;
    if (!instance.listen(&listenError)) {
        qWarning("Cannot listen on %s: %s", qPrintable(instance.socketPath()),
                 qPrintable(listenError));
    }
    /* the Qt app started meanwhile: this window, with its token to take the focus */
    QObject::connect(&instance, &ManagerInstance::activationRequested, &store,
                     [&](const QByteArray &token) {
        if (window) {
            if (!token.isEmpty()) {
                gtk_window_set_startup_id(GTK_WINDOW(window), token.constData());
            }
            gtk_window_present(GTK_WINDOW(window));
        }
    });
    /* SIGTERM, SIGINT and SIGHUP end it as closing the window does, without its questions */
    QuitSignals quit;
    QObject::connect(&quit, &QuitSignals::received, &store, [&]() {
        if (window) {
            gtk_window_destroy(GTK_WINDOW(window));
        }
        loop.quit(0);
    });
    return loop.exec();
}
