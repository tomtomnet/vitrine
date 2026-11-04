// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <utility>

namespace Gtk4 {

/* For the C API: UTF-8 that lives as long as the statement */
inline QByteArray utf8(const QString &text)
{
    return text.toUtf8();
}

/*
 * @function called for @signal of the GObject @instance: libadwaita's
 * signals, which have no C++ wrapper, and GTK's where the C object is at
 * hand.  @Signature is the signal's, without the instance and the user
 * data: void(guint) for AdwSidebar::activated, gboolean() for
 * GtkWindow::close-request.  The function goes with the connection.
 */
template <typename Signature>
struct Thunk;

template <typename R, typename... Args>
struct Thunk<R(Args...)> {
    std::function<R(Args...)> function;

    static R call(gpointer, Args... args, gpointer self)
    {
        return static_cast<Thunk *>(self)->function(args...);
    }
    static void destroy(gpointer self, GClosure *)
    {
        delete static_cast<Thunk *>(self);
    }
};

template <typename Signature, typename F>
gulong connect(gpointer instance, const char *signal, F &&function)
{
    using T = Thunk<Signature>;
    auto *thunk = new T{std::function<Signature>(std::forward<F>(function))};
    return g_signal_connect_data(instance, signal, G_CALLBACK(&T::call), thunk, &T::destroy,
                                 GConnectFlags(0));
}

/* A label of @text, wrapping, left aligned, its text selectable if @selectable */
GtkWidget *label(const QString &text, bool selectable = false);

/*
 * An alert with @heading and @body, and the responses @responses as
 * (id, label) pairs, the first the close response.  @appearance: the
 * response that is suggested, or destructive (ADW_RESPONSE_DESTRUCTIVE)
 * when @destructive.  Not shown: MainWindow::present() shows it.
 */
AdwDialog *alert(const QString &heading, const QString &body,
                 const QList<std::pair<QString, QString>> &responses = {{"close", "Close"}},
                 const QString &suggested = {}, bool destructive = false);
/* Its response, then the dialog goes */
void onResponse(AdwDialog *dialog, std::function<void(const QString &response)> done);
/* Text below the body of an alert: an error as QEMU said it */
void setDetails(AdwDialog *dialog, const QString &text);

/* @path in the desktop's app for it: its file manager, its text editor */
void openPath(GtkWidget *parent, const QString &path);

}
