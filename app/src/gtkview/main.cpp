// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * vitrine-gtk-view: a running VM's screen in a GTK window, attached through
 * its display monitor (a QMP socket) as the GTK app's VM page will attach
 * it: the way to try the GTK view until it lands there.
 *
 *   vitrine-gtk-view [--console N] [--full-screen] [--stats] [--flips-wait 0|1]
 *                    [--flips-wait-toggle SECONDS] SOCKET
 *
 * --stats prints the frame statistics every second: of the frames presented,
 * "direct" ones the compositor scanned out as they were (the presentation
 * feedback's ZERO_COPY flag), "composited" ones it drew into its own frame;
 * the totals since the start end the line.  --flips-wait says what the
 * guest's agent would (VmDisplay::setFlipsWaitForRelease: 0 for a guest
 * with blob_flush_fence 3); unset, the view knows nothing, as before the
 * agent's first report.  --flips-wait-toggle turns it over every SECONDS,
 * for checks of the switch while the view shows.  Qt Core runs on GLib's
 * default context, which GTK's loop runs, as in the GTK app.
 *
 * The view's threads go to stderr as one line at its start and after each
 * full-screen switch ("vitrine: display: threads: main=TID render=TID ...", the
 * format in README.md): this program raises none of them - the GTK app has
 * vitrine-helper make them real-time while the VM is in front - and a bench
 * raises exactly those, by their ids.
 */
#include <QAbstractEventDispatcher>
#include <QCoreApplication>

#include "gtkview/displaywidget.h"
#include "gtkview/vmdisplay.h"

#include <adwaita.h>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/init.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/togglebutton.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

void usage()
{
    fprintf(stderr, "usage: vitrine-gtk-view [--console N] [--full-screen] [--stats] "
                    "[--flips-wait 0|1] [--flips-wait-toggle SECONDS] SOCKET\n");
}

void printStats(VmDisplay *view)
{
    /* the presented frames since the start: scanned out directly, composited */
    static long long totalDirect = 0, totalComposited = 0;
    const Stats::Summary s = view->takeStats();
    totalDirect += s.direct;
    totalComposited += s.presented - s.direct;
    fprintf(stderr,
            "stats: %.1fs updates %d drawn %d imports %d presented %d discarded %d "
            "present p50 %.2f ms p99 %.2f ms interval p50 %.2f ms inputs %d grab \"%s\" keyboard %d "
            "direct %d composited %d total-direct %lld total-composited %lld\n",
            s.seconds, s.updates, s.drawn, s.imports, s.presented, s.discarded, s.presentMsP50,
            s.presentMsP99, s.intervalMsP50, s.inputs, qPrintable(view->grabState()),
            view->hasKeyboard(), s.direct, s.presented - s.direct, totalDirect, totalComposited);
}

} // namespace

int main(int argc, char **argv)
{
    QString socket;
    int console = -1, flipsWait = -1, flipsToggle = 0;
    bool fullScreen = false, stats = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--console") && i + 1 < argc) {
            console = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--full-screen")) {
            fullScreen = true;
        } else if (!strcmp(argv[i], "--stats")) {
            stats = true;
        } else if (!strcmp(argv[i], "--flips-wait") && i + 1 < argc &&
                   (!strcmp(argv[i + 1], "0") || !strcmp(argv[i + 1], "1"))) {
            flipsWait = argv[++i][0] == '1';
        } else if (!strcmp(argv[i], "--flips-wait-toggle") && i + 1 < argc &&
                   atoi(argv[i + 1]) > 0) {
            flipsToggle = atoi(argv[++i]);
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
    GtkWidget *window = nullptr, *toolbar = nullptr;
    int status = 0;

    app->signal_activate().connect([&]() {
        view = std::make_unique<VmDisplay>();
        if (flipsWait >= 0) {
            view->setFlipsWaitForRelease(flipsWait == 1);
        }
        QString error;
        if (!view->attach(socket, &error, false, console)) {
            fprintf(stderr, "vitrine-gtk-view: %s\n", qPrintable(error));
            status = 1;
            view.reset();
            app->quit();
            return;
        }
        /* libadwaita's window, as the app's: its dialogs show inside it */
        window = adw_application_window_new(app->gobj());
        g_object_ref(window);
        gtk_window_set_title(GTK_WINDOW(window), view->vmName().toUtf8().constData());
        gtk_window_set_default_size(GTK_WINDOW(window), 1024, 700);
        toolbar = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(view->widget()->gobj()));
        adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toolbar);

        auto *full = Gtk::make_managed<Gtk::Button>();
        full->set_icon_name("view-fullscreen-symbolic");
        full->set_tooltip_text("Full Screen (Ctrl+Alt+F)");
        full->signal_clicked().connect([&]() { view->setFullScreen(true); });
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(full->gobj()));
        /* an in-window dialog over the screen: the view covered while it shows */
        auto *dialog = Gtk::make_managed<Gtk::Button>();
        dialog->set_icon_name("dialog-information-symbolic");
        dialog->set_tooltip_text("A Dialog over the VM");
        dialog->signal_clicked().connect([&]() {
            AdwDialog *d = adw_alert_dialog_new("A Dialog over the VM",
                                                "The VM's screen shows its last frame under it.");
            adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "close", "Close");
            /* the view may be gone: the window closed with the dialog on it */
            g_signal_connect(d, "closed", G_CALLBACK(+[](AdwDialog *, gpointer v) {
                if (auto &display = *static_cast<std::unique_ptr<VmDisplay> *>(v)) {
                    display->setCovered(false);
                }
            }), &view);
            view->setCovered(true);
            adw_dialog_present(d, window);
        });
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(dialog->gobj()));
        /* the window's own shortcuts, which the view takes the keys before
           while it has the keyboard: an accelerator, and F10's primary menu */
        auto shortcut = Gio::SimpleAction::create("shortcut");
        shortcut->signal_activate().connect([](const Glib::VariantBase &) {
            fprintf(stderr, "vitrine-gtk-view: the window's Ctrl+Q ran\n");
        });
        app->add_action(shortcut);
        app->set_accel_for_action("app.shortcut", "<Control>q");
        auto menu = Gio::Menu::create();
        menu->append("Shortcut (Ctrl+Q)", "app.shortcut");
        auto *primary = Gtk::make_managed<Gtk::MenuButton>();
        primary->set_icon_name("open-menu-symbolic");
        primary->set_menu_model(menu);
        primary->set_primary(true);
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(primary->gobj()));
        /* input off, as for a paused VM: the keys are the window's */
        auto *input = Gtk::make_managed<Gtk::ToggleButton>();
        input->set_icon_name("input-keyboard-symbolic");
        input->set_tooltip_text("Input to the VM");
        input->set_active(true);
        input->signal_toggled().connect([&view, input]() {
            view->setInputEnabled(input->get_active());
            fprintf(stderr, "vitrine-gtk-view: input %s\n", input->get_active() ? "on" : "off");
        });
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(input->gobj()));
        /* a text field, as the app's window has: its input method (Wayland's
           text input) must be off while the view has the keyboard */
        auto *entry = Gtk::make_managed<Gtk::Entry>();
        entry->set_placeholder_text("Text");
        entry->set_width_chars(8);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(entry->gobj()));
        /* ellipsized: a longer state would widen the window, and the guest with it */
        auto *grab = Gtk::make_managed<Gtk::Label>();
        grab->add_css_class("dim-label");
        grab->set_ellipsize(Pango::EllipsizeMode::END);
        grab->set_width_chars(24);
        grab->set_max_width_chars(24);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(grab->gobj()));
        view->grabChanged.connect([&view, grab]() {
            grab->set_text(view->hasKeyboard() ? view->grabState().toStdString() : "");
            fprintf(stderr, "vitrine-gtk-view: keyboard %d, %s\n", view->hasKeyboard(),
                    qPrintable(view->grabState()));
        });
        view->fullScreenChanged.connect([](bool on) {
            fprintf(stderr, "vitrine-gtk-view: full screen %s\n", on ? "on" : "off");
        });
        view->screenInfoApplied.connect([]() {
            fprintf(stderr, "vitrine-gtk-view: QEMU took the screen's size\n");
        });
        Glib::wrap(GTK_WINDOW(window))->signal_close_request().connect([&]() {
            /* the view off the window before GTK takes it down */
            adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), nullptr);
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
        if (flipsToggle > 0) {
            Glib::signal_timeout().connect_seconds([&]() {
                if (view) {
                    flipsWait = flipsWait == 0;   // unset (-1) counts as 1
                    fprintf(stderr, "vitrine-gtk-view: flips wait %d\n", flipsWait);
                    view->setFlipsWaitForRelease(flipsWait == 1);
                }
                return true;
            }, flipsToggle);
        }
        gtk_window_present(GTK_WINDOW(window));
        view->focus();
        if (fullScreen) {
            view->setFullScreen(true);
        }
    });
    status = app->run(1, argv) ?: status;
    view.reset();
    if (window) {
        g_object_unref(window);
    }
    return status;
}
