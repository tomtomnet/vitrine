// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/gtkloop.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QMetaObject>

GtkLoop::GtkLoop(const char *appId)
    : m_app(adw_application_new(appId, G_APPLICATION_DEFAULT_FLAGS))
{
    /* gtkmm's wrappers for objects made in C, as Gtk::Application::create() would */
    Gtk::init_gtkmm_internals();
    m_wrapped = Glib::wrap(GTK_APPLICATION(m_app), true);
    g_signal_connect(m_app, "activate", G_CALLBACK(activateThunk), this);
}

GtkLoop::~GtkLoop()
{
    g_signal_handlers_disconnect_by_data(m_app, this);
    m_wrapped.reset();
    /*
     * Kept once registered: GLib unregisters an application only at the
     * end of g_application_run(), and warns of one finalized while
     * registered.  The bus releases its name as the process ends.
     */
    if (!g_application_get_is_registered(G_APPLICATION(m_app))) {
        g_object_unref(m_app);
    }
}

bool GtkLoop::supported(QString *why)
{
    const QAbstractEventDispatcher *dispatcher = QAbstractEventDispatcher::instance();

    /* the main thread's: GLib's default main context (QEventDispatcherGlibPrivate) */
    if (dispatcher && dispatcher->inherits("QEventDispatcherGlib")) {
        return true;
    }
    if (why) {
        const QString name = dispatcher ? dispatcher->metaObject()->className() : "none";
        *why = qEnvironmentVariableIsSet("QT_NO_GLIB")
                   ? QString("QT_NO_GLIB is set: Qt's event loop then runs without GLib, and "
                             "GTK's would never run. Start it without QT_NO_GLIB.")
                   : QString("this Qt runs its event loop without GLib (%1), and GTK's would "
                             "never run: it needs a Qt built with GLib.").arg(name);
    }
    return false;
}

GtkLoop::Start GtkLoop::start(QString *error)
{
    GApplication *app = G_APPLICATION(m_app);
    GError *gerror = nullptr;

    if (!g_application_register(app, nullptr, &gerror)) {
        if (error) {
            *error = QString::fromUtf8(gerror->message);
        }
        g_error_free(gerror);
        return Start::Failed;
    }
    if (!g_application_get_is_remote(app)) {
        return Start::Primary;
    }
    /*
     * As a second g_application_run() does: the first instance activated,
     * with this one's activation token (GtkApplication adds it), the call
     * out on the bus before this one ends
     */
    g_application_activate(app);
    if (GDBusConnection *bus = g_application_get_dbus_connection(app)) {
        g_dbus_connection_flush_sync(bus, nullptr, nullptr);
    }
    return Start::Remote;
}

void GtkLoop::setActivated(std::function<void()> activated)
{
    m_activated = std::move(activated);
}

int GtkLoop::exec()
{
    GApplication *app = G_APPLICATION(m_app);

    /* as the windows' hold on the app ends g_application_run() */
    const gulong removed =
        g_signal_connect(m_app, "window-removed", G_CALLBACK(windowRemovedThunk), this);
    g_application_activate(app);
    const int status = m_quitting ? m_status : QCoreApplication::exec();
    g_signal_handler_disconnect(m_app, removed);

    /* what g_application_run() does once its loop ends */
    g_signal_emit_by_name(app, "shutdown");
    if (GDBusConnection *bus = g_application_get_dbus_connection(app)) {
        g_dbus_connection_flush_sync(bus, nullptr, nullptr);
    }
    g_settings_sync();
    return status;
}

void GtkLoop::quit(int status)
{
    if (m_quitting) {
        return;
    }
    m_quitting = true;
    m_status = status;
    /* queued: also when asked before exec() runs Qt's loop, which forgets an exit() before it */
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [status]() { QCoreApplication::exit(status); },
        Qt::QueuedConnection);
}

void GtkLoop::activateThunk(GApplication *, gpointer self)
{
    if (const auto &activated = static_cast<GtkLoop *>(self)->m_activated) {
        activated();
    }
}

void GtkLoop::windowRemovedThunk(GtkApplication *app, GtkWindow *, gpointer self)
{
    if (!gtk_application_get_windows(app)) {
        static_cast<GtkLoop *>(self)->quit(0);
    }
}
