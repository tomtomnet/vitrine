// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkwindialog.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QStyle>

#include "ui/icons.h"
#include "ui/widgets.h"

using HostKwin::Status;

namespace HostKwinDialog {

static QString tr(const char *text)
{
    return QCoreApplication::translate("HostKwinDialog", text);
}

QString names(const QStringList &packages)
{
    if (packages.size() <= 1) {
        return packages.value(0);
    }
    return tr("%1 and %2").arg(packages.mid(0, packages.size() - 1).join(", "), packages.last());
}

QStringList replaced(const Status &status, const HostKwin::System &system)
{
    QStringList list;

    for (const HostKwin::Package &p : system.kwin) {
        if (status.build.rpm(p.name).name == p.name) {
            list << p.name;
        }
    }
    return list.isEmpty() ? QStringList{"kwin", "kwin-common", "kwin-libs"} : list;
}

QString consoleCommands(const QStringList &packages)
{
    const QString list = packages.join(' ');
    return "sudo dnf versionlock delete " + list + "\nsudo dnf distro-sync " + list;
}

QString installText(const Status &status, const QStringList &packages)
{
    return "<p>" +
           tr("This replaces KWin, this computer's compositor, with Vitrine's build: KWin %1 with "
              "the research's host set %2. VMs get steadier frames and quicker input, and the "
              "pointer grab (Ctrl+Alt+G) works inside Vitrine's Desktop view.")
               .arg(status.build.version.toHtmlEscaped(), status.build.series.toHtmlEscaped()) +
           "</p><ul><li>" +
           tr("It is installed as root, with the administrator's password, in place of %1.")
               .arg(names(packages).toHtmlEscaped()) +
           "</li><li>" +
           tr("It must match the installed Plasma and Qt exactly. These packages are "
              "version-locked so that updates leave them alone, but Plasma and Qt updates still "
              "come in: after one, KWin must be built and installed again, or reverted. Vitrine "
              "tells you when.") +
           "</li><li>" +
           tr("The set's run-time switches go on for every user of this computer "
              "(/etc/user-tmpfiles.d/vitrine-host-kwin.conf, whose comments say what each "
              "does); another KWin ignores them.") +
           "</li><li>" +
           tr("Revert… puts the distribution's KWin back, from your repositories, and removes "
              "the switches.") +
           "</li><li>" +
           tr("It takes effect at your next login: the running desktop is not restarted.") +
           "</li></ul><p>" +
           tr("If the desktop does not start after an update, log in on a text console "
              "(Ctrl+Alt+F3) and put the distribution's KWin back:") +
           "</p><pre>" + consoleCommands(packages).toHtmlEscaped() + "</pre>";
}

QString foreignText(const Status &status)
{
    if (status.owner != Status::Owner::Foreign) {
        return {};
    }
    if (status.kwin.isLocal()) {
        return "<p><b>" +
               tr("%1 (not Vitrine's) will be replaced.").arg(status.kwin.nevra().toHtmlEscaped()) +
               "</b></p><p>" +
               (status.locked ? tr("It was installed from a file, and it is version-locked: its "
                                   "lock goes too.")
                              : tr("It was installed from a file.")) +
               ' ' +
               tr("Vitrine cannot put it back: only whoever installed it can, from their own "
                  "packages.") +
               "</p>";
    }
    return "<p><b>" +
           tr("KWin is version-locked at %1, not by Vitrine.")
               .arg((status.lockedAt.isEmpty() ? status.kwin.evr() : status.lockedAt).toHtmlEscaped()) +
           "</b></p><p>" +
           tr("Installing removes that lock, and Vitrine's KWin replaces %1.")
               .arg(status.kwin.nevra().toHtmlEscaped()) +
           "</p>";
}

QString revertText(const QStringList &packages)
{
    return "<p>" +
           tr("This puts the distribution's KWin back: the version lock of %1 goes, and dnf "
              "installs the version your repositories have now (dnf distro-sync), with the "
              "administrator's password.")
               .arg(names(packages).toHtmlEscaped()) +
           "</p><p>" +
           tr("It takes effect at your next login: the running desktop is not restarted.") +
           "</p>";
}

bool install(QWidget *parent)
{
    HostKwinMonitor *monitor = HostKwinMonitor::instance();
    const Status status = monitor->status();
    const QStringList packages = replaced(status, monitor->system());
    QString why;

    if (monitor->isBusy()) {
        return false;
    }
    if (!status.canInstall(&why)) {
        Widgets::warn(parent, tr("Install Vitrine's KWin"), why);
        return false;
    }
    if (!Widgets::confirm(parent, QMessageBox::Warning, tr("Install Vitrine's KWin"),
                          installText(status, packages), tr("&Install…"),
                          Icons::themed({"run-install", "system-software-install"},
                                        QStyle::SP_DialogApplyButton))) {
        return false;
    }
    if (const QString foreign = foreignText(status);
        !foreign.isEmpty() &&
        !Widgets::confirm(parent, QMessageBox::Warning, tr("Replace Another KWin"), foreign,
                          tr("&Replace It"),
                          Icons::themed({"edit-delete-shred", "edit-delete"},
                                        QStyle::SP_DialogDiscardButton))) {
        return false;
    }
    monitor->install(status.build);
    return true;
}

bool revert(QWidget *parent)
{
    HostKwinMonitor *monitor = HostKwinMonitor::instance();
    const Status status = monitor->status();
    QStringList packages;
    QString why;

    if (monitor->isBusy()) {
        return false;
    }
    if (!status.canRevert(&why)) {
        Widgets::warn(parent, tr("Revert to the Distribution's KWin"), why);
        return false;
    }
    for (const HostKwin::Package &p : monitor->system().kwin) {
        packages << p.name;
    }
    if (!Widgets::confirm(parent, QMessageBox::Question, tr("Revert to the Distribution's KWin"),
                          revertText(packages), tr("&Revert"),
                          Icons::themed({"edit-undo"}, QStyle::SP_ArrowBack))) {
        return false;
    }
    monitor->revert();
    return true;
}

}
