// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/hostkdealerts.h"

#include <QList>
#include <QRegularExpression>

#include <memory>
#include <utility>

#include "core/hostkde.h"
#include "gtk/markup.h"
#include "gtk/widgets.h"
#include "ui/hostkdetext.h"

using Gtk4::utf8;
using HostKde::Status;

namespace HostKdeAlerts {

AdwDialog *alert(const QString &heading, const QString &html)
{
    static const QRegularExpression pre("<pre>(.*?)</pre>",
                                        QRegularExpression::DotMatchesEverythingOption);
    AdwDialog *dialog = adw_alert_dialog_new(utf8(heading).constData(), nullptr);
    auto *alert = ADW_ALERT_DIALOG(dialog);
    QString body = html;
    QString commands;

    /* the commands to type from a text console: apart, in a monospace font, to copy */
    if (const QRegularExpressionMatch m = pre.match(html); m.hasMatch()) {
        commands = Markup::plain(m.captured(1));
        body.remove(m.capturedStart(), m.capturedLength());
    }
    adw_alert_dialog_set_body_use_markup(alert, true);
    adw_alert_dialog_set_body(alert, utf8(Markup::pango(body)).constData());
    adw_alert_dialog_set_prefer_wide_layout(alert, true);
    if (!commands.isEmpty()) {
        GtkWidget *label = Gtk4::label(commands, true);
        gtk_widget_add_css_class(label, "monospace");
        gtk_widget_set_name(label, "commands");
        adw_alert_dialog_set_extra_child(alert, label);
    }
    return dialog;
}

/* @dialog asks before @action (destructive, as in the Qt app); Cancel is the default */
static void addResponses(AdwDialog *dialog, const QString &action)
{
    auto *alert = ADW_ALERT_DIALOG(dialog);

    adw_alert_dialog_add_response(alert, "cancel", "_Cancel");
    adw_alert_dialog_add_response(alert, "go", utf8(action).constData());
    adw_alert_dialog_set_response_appearance(alert, "go", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(alert, "cancel");
    adw_alert_dialog_set_default_response(alert, "cancel");
}

/* The warnings before packages that are not Vitrine's go, one after the other; then @done */
static void confirmForeign(const Present &present, const QList<Status> &statuses, qsizetype from,
                           std::function<void()> done)
{
    for (qsizetype i = from; i < statuses.size(); i++) {
        const QString foreign = HostKdeDialog::foreignText(statuses[i]);
        if (foreign.isEmpty()) {
            continue;
        }
        AdwDialog *dialog =
            alert(QString("Replace Another %1").arg(statuses[i].target.title), foreign);
        addResponses(dialog, "_Replace It");
        Gtk4::onResponse(dialog, [present, statuses, i, done](const QString &response) {
            if (response == "go") {
                confirmForeign(present, statuses, i + 1, done);
            }
        });
        present(dialog);
        return;
    }
    done();
}

void install(const Present &present, const QString &id)
{
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    const Status status = monitor->status(id);
    const QStringList packages = HostKdeDialog::replaced(status);
    const QString title = QString("Install Vitrine's %1").arg(status.target.title);
    QString why;

    if (monitor->isBusy()) {
        return;
    }
    if (!status.canInstall(&why)) {
        present(Gtk4::alert(title, why));
        return;
    }
    AdwDialog *dialog = alert(title, HostKdeDialog::installText(status, packages));
    addResponses(dialog, "_Install…");
    Gtk4::onResponse(dialog, [present, id, status](const QString &response) {
        if (response != "go") {
            return;
        }
        confirmForeign(present, {status}, 0, [id, status]() {
            HostKdeMonitor *monitor = HostKdeMonitor::instance();
            if (!monitor->isBusy()) {
                monitor->install(id, status.build);
            }
        });
    });
    present(dialog);
}

void installAll(const Present &present, const QStringList &ids)
{
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    QList<Status> statuses;
    QStringList ready;
    QStringList titles;

    if (monitor->isBusy()) {
        return;
    }
    for (const QString &id : ids) {
        const Status s = monitor->status(id);
        if (s.canInstall()) {
            statuses << s;
            ready << id;
            titles << s.target.title;
        }
    }
    if (ready.size() <= 1) {
        if (!ready.isEmpty()) {
            install(present, ready.first());
        }
        return;
    }
    AdwDialog *dialog = alert(QString("Install Vitrine's %1").arg(HostKdeDialog::names(titles)),
                              HostKdeDialog::installAllText(statuses));
    addResponses(dialog, "_Install…");
    Gtk4::onResponse(dialog, [present, ready, statuses](const QString &response) {
        if (response != "go") {
            return;
        }
        /* a package that is not Vitrine's: its own warning before it goes */
        confirmForeign(present, statuses, 0, [ready, statuses]() {
            HostKdeMonitor *monitor = HostKdeMonitor::instance();
            QList<std::pair<QString, HostKde::Build>> builds;
            if (monitor->isBusy()) {
                return;
            }
            for (qsizetype i = 0; i < ready.size(); i++) {
                builds << std::make_pair(ready[i], statuses[i].build);
            }
            monitor->install(builds);
        });
    });
    present(dialog);
}

void revert(const Present &present, const QStringList &ids)
{
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    QList<Status> statuses;
    QStringList titles;
    QString why;

    if (monitor->isBusy() || ids.isEmpty()) {
        return;
    }
    for (const QString &id : ids) {
        statuses << monitor->status(id);
        titles << statuses.last().target.title;
    }
    const QString title =
        QString("Revert to the Distribution's %1").arg(HostKdeDialog::names(titles));
    for (const Status &s : std::as_const(statuses)) {
        if (!s.canRevert(&why)) {
            present(Gtk4::alert(title, why));
            return;
        }
    }
    AdwDialog *dialog = alert(title, HostKdeDialog::revertText(statuses));
    addResponses(dialog, "_Revert");
    Gtk4::onResponse(dialog, [ids](const QString &response) {
        HostKdeMonitor *monitor = HostKdeMonitor::instance();
        if (response == "go" && !monitor->isBusy()) {
            monitor->revert(ids);
        }
    });
    present(dialog);
}

}
