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

QString names(const QStringList &packages)
{
    if (packages.size() <= 1) {
        return packages.value(0);
    }
    return tr("%1 and %2").arg(packages.mid(0, packages.size() - 1).join(", "), packages.last());
}

QStringList replaced(const Status &status)
{
    return status.names.isEmpty() ? QStringList{status.target.package} : status.names;
}

QString consoleCommands(const QStringList &packages)
{
    const QString list = packages.join(' ');
    return "sudo dnf versionlock delete " + list + "\nsudo dnf distro-sync " + list;
}

/* What the package is, and what its set changes, in a sentence or two */
static QString what(const Status &status)
{
    const QString version = status.build.version.toHtmlEscaped();
    const QString set = status.build.series.toHtmlEscaped();
    const QString package = status.target.package;

    if (package == "kwin") {
        return tr("This replaces KWin, this computer's compositor, with Vitrine's build: KWin %1 "
                  "with the research's host set %2. VMs get steadier frames and quicker input, and "
                  "the pointer grab (Ctrl+Alt+G) works inside Vitrine's Desktop view.")
            .arg(version, set);
    }
    if (package == "kf6-ksvg") {
        return tr("This replaces KSvg, the KDE framework that draws Plasma's themed frames, with "
                  "Vitrine's build: KSvg %1 with the research's host set %2. Plasma's popups stop "
                  "uploading their frames again each time they open.")
            .arg(version, set);
    }
    if (package == "kf6-kirigami") {
        return tr("This replaces Kirigami, the KDE framework of the controls Plasma and many "
                  "programs are made of, with Vitrine's build: Kirigami %1 with the research's host "
                  "set %2. Their items react to a change of the frame contrast only when it changes, "
                  "not each time Plasma's style sets it again.")
            .arg(version, set);
    }
    if (package == "qt6-qtbase") {
        return tr("This replaces Qt's base libraries, which every Qt program uses, the desktop "
                  "too, with Vitrine's build: Qt %1 with the research's host set %2. A window being "
                  "resized reuses its shared memory for the next sizes instead of new memory for "
                  "each, so resizes show more sizes a second.")
            .arg(version, set);
    }
    return tr("This replaces %1 with Vitrine's build: %1 %2 with the research's host set %3.")
        .arg(status.target.title.toHtmlEscaped(), version, set);
}

/* What the version lock means for it, and when it must be built again */
static QString lock(const Status &status)
{
    const QString package = status.target.package;
    const QString title = status.target.title.toHtmlEscaped();

    if (package == "kwin") {
        return tr("It must match the installed Plasma and Qt exactly. These packages are "
                  "version-locked so that updates leave them alone, but Plasma and Qt updates still "
                  "come in: after one, KWin must be built and installed again, or reverted. Vitrine "
                  "tells you when.");
    }
    if (package == "qt6-qtbase") {
        return tr("These packages are version-locked so that updates leave them alone, and so, in "
                  "effect, is the rest of Qt: Qt's other modules need the same version of Qt base, "
                  "so no Qt update comes in until Qt base is built again for it (once the research "
                  "has a set for it) or reverted. Vitrine tells you when one waits. After a Qt "
                  "update, Vitrine's KWin and Kirigami, if installed, must be built again too: they "
                  "use Qt's private API.");
    }
    QString text = tr("These packages are version-locked so that updates leave them alone; the "
                      "other KDE Frameworks still update. When they move to a new release, %1 must "
                      "be built again for it (once the research has a set for it) or reverted: "
                      "upstream does not support frameworks of different releases together. Vitrine "
                      "tells you when.")
                       .arg(title);
    if (status.target.against.contains("qt6-qtbase")) {
        text += ' ' + tr("%1 uses Qt's private API: after a Qt update it must be built again too.")
                          .arg(title);
    }
    return text;
}

QString installText(const Status &status, const QStringList &packages)
{
    const bool kwin = status.target.package == "kwin";
    QString html = "<p>" + what(status) + "</p><ul><li>" +
                   tr("It is installed as root, with the administrator's password, in place of %1, "
                      "all of them from this build.")
                       .arg(names(packages).toHtmlEscaped()) +
                   "</li><li>" + lock(status) + "</li>";

    if (kwin) {
        html += "<li>" +
                tr("The set's run-time switches go on for every user of this computer "
                   "(/etc/user-tmpfiles.d/vitrine-host-kwin.conf, whose comments say what each "
                   "does); another KWin ignores them.") +
                "</li>";
    }
    html += "<li>" +
            (kwin ? tr("Revert… puts the distribution's KWin back, from your repositories, and "
                       "removes the switches.")
                  : tr("Revert… puts the distribution's packages back, from your repositories.")) +
            "</li><li>" +
            (kwin ? tr("It takes effect at your next login: the running desktop is not restarted.")
                  : tr("Programs started from now on use it, the desktop from your next login: "
                       "nothing is restarted.")) +
            "</li></ul><p>" +
            tr("If the desktop does not start afterwards, log in on a text console (Ctrl+Alt+F3) "
               "and put the distribution's packages back:") +
            "</p><pre>" + consoleCommands(packages).toHtmlEscaped() + "</pre>";
    return html;
}

QString foreignText(const Status &status)
{
    const QString title = status.target.title.toHtmlEscaped();

    if (status.owner != Status::Owner::Foreign) {
        return {};
    }
    if (status.installed.isLocal()) {
        return "<p><b>" +
               tr("%1 (not Vitrine's) will be replaced.").arg(status.installed.nevra().toHtmlEscaped()) +
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
           tr("%1 is version-locked at %2, not by Vitrine.")
               .arg(title, (status.lockedAt.isEmpty() ? status.installed.evr() : status.lockedAt)
                               .toHtmlEscaped()) +
           "</b></p><p>" +
           tr("Installing removes that lock, and Vitrine's %1 replaces %2.")
               .arg(title, status.installed.nevra().toHtmlEscaped()) +
           "</p>";
}

QString revertText(const QList<Status> &statuses)
{
    QStringList titles;
    QStringList packages;
    bool kwin = false;
    bool qt = false;

    for (const Status &s : statuses) {
        titles << s.target.title;
        packages << replaced(s);
        kwin = kwin || s.target.package == "kwin";
        qt = qt || s.target.package == "qt6-qtbase";
    }
    QString html = "<p>" +
                   tr("This puts the distribution's %1 back: the version locks of %2 go, and dnf "
                      "installs the version your repositories have now (dnf distro-sync), with the "
                      "administrator's password.")
                       .arg(names(titles).toHtmlEscaped(), names(packages).toHtmlEscaped()) +
                   "</p>";
    if (qt) {
        html += "<p>" + tr("The Qt updates the lock held back come in with your next update.") +
                "</p>";
    }
    html += "<p>" +
            (kwin ? tr("It takes effect at your next login: the running desktop is not restarted.")
                  : tr("Programs started from now on use them, the desktop from your next login: "
                       "nothing is restarted.")) +
            "</p>";
    return html;
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

QString installAllText(const QList<Status> &statuses)
{
    QStringList titles;
    QStringList packages;
    QString items;
    bool kwin = false;

    for (const Status &s : statuses) {
        titles << s.target.title;
        packages << replaced(s);
        kwin = kwin || s.target.package == "kwin";
        items += "<li><b>" + s.target.title.toHtmlEscaped() + "</b>: " + what(s) + ' ' + lock(s) +
                 "</li>";
    }
    QString html = "<p>" +
                   tr("This installs Vitrine's builds of %1 in one go, with the administrator's "
                      "password once, in place of the distribution's packages:")
                       .arg(names(titles).toHtmlEscaped()) +
                   "</p><ul>" + items + "</ul><ul><li>" +
                   tr("They are installed as root in one transaction, each package's installed "
                      "parts all from its build.") +
                   "</li>";
    if (kwin) {
        html += "<li>" +
                tr("KWin's run-time switches go on for every user of this computer "
                   "(/etc/user-tmpfiles.d/vitrine-host-kwin.conf, whose comments say what each "
                   "does); another KWin ignores them.") +
                "</li>";
    }
    html += "<li>" +
            tr("Revert… puts the distribution's packages back, from your repositories, one "
               "package or several at once.") +
            "</li><li>" +
            tr("Programs started from now on use them, the desktop from your next login: nothing "
               "is restarted.") +
            "</li></ul><p>" +
            tr("If the desktop does not start afterwards, log in on a text console (Ctrl+Alt+F3) "
               "and put the distribution's packages back:") +
            "</p><pre>" + consoleCommands(packages).toHtmlEscaped() + "</pre>";
    return html;
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
