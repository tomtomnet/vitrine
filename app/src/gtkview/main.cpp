// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * vitrine-gtk-view: a running VM's screen in a GTK window, attached through
 * its display monitor (a QMP socket) as the GTK app's VM page will attach
 * it: the way to try the GTK view until it lands there.
 *
 *   vitrine-gtk-view [--console N] [--full-screen] [--stats] SOCKET
 *
 * --stats prints the frame statistics every second.  Qt Core runs on GLib's
 * default context, which GTK's loop runs, as in the GTK app.
 */
#include <QAbstractEventDispatcher>
#include <QCoreApplication>

#include "gtkview/displaywidget.h"
#include "gtkview/vmdisplay.h"

#include <adwaita.h>
#include <gtkmm/application.h>
#include <gtkmm/applicationwindow.h>
#include <gtkmm/button.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/init.h>
#include <gtkmm/label.h>
#include <glibmm/main.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

void usage()
{
    fprintf(stderr, "usage: vitrine-gtk-view [--console N] [--full-screen] [--stats] SOCKET\n");
}

void printStats(VmDisplay *view)
{
    const Stats::Summary s = view->takeStats();
    fprintf(stderr,
            "stats: %.1fs updates %d drawn %d imports %d presented %d discarded %d "
            "present p50 %.2f ms p99 %.2f ms interval p50 %.2f ms inputs %d grab \"%s\" keyboard %d\n",
            s.seconds, s.updates, s.drawn, s.imports, s.presented, s.discarded, s.presentMsP50,
            s.presentMsP99, s.intervalMsP50, s.inputs, qPrintable(view->grabState()),
            view->hasKeyboard());
}

} // namespace

int main(int argc, char **argv)
{
    QString socket;
    int console = -1;
    bool fullScreen = false, stats = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--console") && i + 1 < argc) {
            console = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--full-screen")) {
            fullScreen = true;
        } else if (!strcmp(argv[i], "--stats")) {
            stats = true;
        } else if (argv[i][0] != '-' && socket.isEmpty()) {
            socket = QString::fromLocal8Bit(argv[i]);
        } else {
            usage();
            return 2;
        }
    }
    if (socket.isEmpty()) {
        usage();
        return 2;
    }

    int qtArgc = 1;
    QCoreApplication qtApp(qtArgc, argv);
    if (!QAbstractEventDispatcher::instance()->inherits("QEventDispatcherGlib")) {
        fprintf(stderr, "vitrine-gtk-view: Qt does not run on GLib's main loop here\n");
        return 1;
    }

    /* libadwaita's application, for its style; the app's ID, for GNOME's
       grants (shortcuts inhibition), but not unique: the app may run too */
    Gtk::init_gtkmm_internals();   // gtkmm's wrappers, for an application it did not make
    auto app = Glib::wrap(GTK_APPLICATION(adw_application_new(
                              "org.vitrine.Vitrine", G_APPLICATION_NON_UNIQUE)),
                          false);
    std::unique_ptr<VmDisplay> view;
    Gtk::ApplicationWindow *window = nullptr;
    int status = 0;

    app->signal_activate().connect([&]() {
        view = std::make_unique<VmDisplay>();
        QString error;
        if (!view->attach(socket, &error, false, console)) {
            fprintf(stderr, "vitrine-gtk-view: %s\n", qPrintable(error));
            status = 1;
            view.reset();
            app->quit();
            return;
        }
        window = new Gtk::ApplicationWindow(app);
        window->set_title(view->vmName().toStdString());
        window->set_default_size(1024, 700);
        auto *header = Gtk::make_managed<Gtk::HeaderBar>();
        auto *full = Gtk::make_managed<Gtk::Button>();
        full->set_icon_name("view-fullscreen-symbolic");
        full->set_tooltip_text("Full Screen (Ctrl+Alt+F)");
        full->signal_clicked().connect([&]() { view->setFullScreen(true); });
        header->pack_end(*full);
        auto *grab = Gtk::make_managed<Gtk::Label>();
        grab->add_css_class("dim-label");
        header->pack_start(*grab);
        window->set_titlebar(*header);
        window->set_child(*view->widget());
        view->grabChanged.connect([&view, grab]() {
            grab->set_text(view->hasKeyboard() ? view->grabState().toStdString() : "");
        });
        view->fullScreenChanged.connect([](bool on) {
            fprintf(stderr, "vitrine-gtk-view: full screen %s\n", on ? "on" : "off");
        });
        view->screenInfoApplied.connect([]() {
            fprintf(stderr, "vitrine-gtk-view: QEMU took the screen's size\n");
        });
        window->signal_close_request().connect([&]() {
            /* the view off the window before GTK takes it down */
            window->unset_child();
            view.reset();
            return false;
        }, false);
        if (stats) {
            Glib::signal_timeout().connect_seconds([&]() {
                if (view) {
                    printStats(view.get());
                }
                return true;
            }, 1);
        }
        window->present();
        view->focus();
        if (fullScreen) {
            view->setFullScreen(true);
        }
    });
    status = app->run(1, argv) ?: status;
    view.reset();
    delete window;
    return status;
}
