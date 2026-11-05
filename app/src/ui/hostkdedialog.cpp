// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkdedialog.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QStyle>

#include "ui/icons.h"
#include "ui/widgets.h"

using HostKde::Status;

namespace HostKdeDialog {

static QString tr(const char *text)
{
    return QCoreApplication::translate("HostKdeDialog", text);
}

bool install(QWidget *parent, const QString &id)
{
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    const Status status = monitor->status(id);
    const QStringList packages = replaced(status);
    const QString title = tr("Install Vitrine's %1").arg(status.target.title);
    QString why;

    if (monitor->isBusy()) {
        return false;
    }
    if (!status.canInstall(&why)) {
        Widgets::warn(parent, title, why);
        return false;
    }
    if (!Widgets::confirm(parent, QMessageBox::Warning, title, installText(status, packages),
                          tr("&Install…"),
                          Icons::themed({"run-install", "system-software-install"},
                                        QStyle::SP_DialogApplyButton))) {
        return false;
    }
    if (const QString foreign = foreignText(status);
        !foreign.isEmpty() &&
        !Widgets::confirm(parent, QMessageBox::Warning, tr("Replace Another %1").arg(status.target.title),
                          foreign, tr("&Replace It"),
                          Icons::themed({"edit-delete-shred", "edit-delete"},
                                        QStyle::SP_DialogDiscardButton))) {
        return false;
    }
    monitor->install(id, status.build);
    return true;
}

bool installAll(QWidget *parent, const QStringList &ids)
{
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    QList<Status> statuses;
    QStringList ready;
    QStringList titles;

    if (monitor->isBusy()) {
        return false;
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
        return !ready.isEmpty() && install(parent, ready.first());
    }
    const QString title = tr("Install Vitrine's %1").arg(names(titles));
    if (!Widgets::confirm(parent, QMessageBox::Warning, title, installAllText(statuses),
                          tr("&Install…"),
                          Icons::themed({"run-install", "system-software-install"},
                                        QStyle::SP_DialogApplyButton))) {
        return false;
    }
    /* a package that is not Vitrine's: its own warning before it goes */
    for (const Status &s : std::as_const(statuses)) {
        if (const QString foreign = foreignText(s);
            !foreign.isEmpty() &&
            !Widgets::confirm(parent, QMessageBox::Warning,
                              tr("Replace Another %1").arg(s.target.title), foreign,
                              tr("&Replace It"),
                              Icons::themed({"edit-delete-shred", "edit-delete"},
                                            QStyle::SP_DialogDiscardButton))) {
            return false;
        }
    }
    QList<std::pair<QString, HostKde::Build>> builds;
    for (qsizetype i = 0; i < ready.size(); i++) {
        builds << std::make_pair(ready[i], statuses[i].build);
    }
    monitor->install(builds);
    return true;
}

bool revert(QWidget *parent, const QStringList &ids)
{
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    QList<Status> statuses;
    QStringList titles;
    QString why;

    if (monitor->isBusy() || ids.isEmpty()) {
        return false;
    }
    for (const QString &id : ids) {
        statuses << monitor->status(id);
        titles << statuses.last().target.title;
    }
    const QString title = tr("Revert to the Distribution's %1").arg(names(titles));
    for (const Status &s : std::as_const(statuses)) {
        if (!s.canRevert(&why)) {
            Widgets::warn(parent, title, why);
            return false;
        }
    }
    if (!Widgets::confirm(parent, QMessageBox::Question, title, revertText(statuses), tr("&Revert"),
                          Icons::themed({"edit-undo"}, QStyle::SP_ArrowBack))) {
        return false;
    }
    monitor->revert(ids);
    return true;
}

}
