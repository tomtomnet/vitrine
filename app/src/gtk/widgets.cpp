// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/widgets.h"

#include <QFileInfo>

namespace Gtk4 {

GtkWidget *label(const QString &text, bool selectable)
{
    GtkWidget *label = gtk_label_new(utf8(text).constData());

    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_selectable(GTK_LABEL(label), selectable);
    return label;
}

AdwDialog *alert(const QString &heading, const QString &body,
                 const QList<std::pair<QString, QString>> &responses, const QString &suggested,
                 bool destructive)
{
    AdwDialog *dialog = adw_alert_dialog_new(utf8(heading).constData(),
                                             body.isEmpty() ? nullptr : utf8(body).constData());
    auto *alert = ADW_ALERT_DIALOG(dialog);

    for (const auto &[id, text] : responses) {
        adw_alert_dialog_add_response(alert, utf8(id).constData(), utf8(text).constData());
    }
    if (!responses.isEmpty()) {
        adw_alert_dialog_set_close_response(alert, utf8(responses.first().first).constData());
    }
    if (!suggested.isEmpty()) {
        adw_alert_dialog_set_response_appearance(
            alert, utf8(suggested).constData(),
            destructive ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(alert, utf8(suggested).constData());
    } else if (!responses.isEmpty()) {
        /* the focus on it, not on a text below the body, which would show all selected */
        adw_alert_dialog_set_default_response(alert, utf8(responses.first().first).constData());
    }
    return dialog;
}

void onResponse(AdwDialog *dialog, std::function<void(const QString &response)> done)
{
    connect<void(const char *)>(dialog, "response", [done = std::move(done)](const char *id) {
        done(QString::fromUtf8(id));
    });
}

void setDetails(AdwDialog *dialog, const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    /* not selectable: it would take the focus, all of it selected (the VM's Details have it) */
    GtkWidget *details = label(text);
    gtk_widget_add_css_class(details, "monospace");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), details);
}

void openPath(GtkWidget *parent, const QString &path)
{
    GFile *file = g_file_new_for_path(QFile::encodeName(path).constData());
    GtkFileLauncher *launcher = gtk_file_launcher_new(file);
    GtkRoot *root = parent ? gtk_widget_get_root(parent) : nullptr;

    gtk_file_launcher_launch(launcher, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr, nullptr,
                             nullptr, nullptr);
    g_object_unref(launcher);
    g_object_unref(file);
}

}
