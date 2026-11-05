// SPDX-License-Identifier: GPL-2.0-or-later
#include "updatetext.h"

#include <QCoreApplication>
#include <QLocale>

namespace UpdateText {

/* the Qt app's UpdateNotifier's words, in its translation context */
static QString tr(const char *text)
{
    return QCoreApplication::translate("UpdateNotifier", text);
}

/* "1 new commit", "12 new commits": English has no translation file to pick */
QString newCommits(int count)
{
    return count == 1 ? tr("1 new commit")
                      : tr("%1 new commits").arg(count);
}

QString results(const QList<UpdateCheck::Result> &results, const QString &sourceDir)
{
    QString text;

    for (const UpdateCheck::Result &r : results) {
        const QString name = "<b>" + r.project.name.toHtmlEscaped() + "</b>";
        if (r.newCommits < 0) {
            text += "<p>" + tr("%1: could not check (%2).").arg(name, r.error.toHtmlEscaped()) +
                    "</p>";
            continue;
        }
        if (r.newCommits == 0) {
            text += "<p>" + tr("%1 is up to date.").arg(name) + "</p>";
            continue;
        }
        text += "<p>" + tr("%1 has %2, the latest:").arg(name, newCommits(r.newCommits)) +
                "</p><ul>";
        for (const QString &subject : r.subjects) {
            text += "<li>" + subject.toHtmlEscaped() + "</li>";
        }
        text += "</ul><p>";
        text += tr("To update it: <code>git pull</code> in %1, then build and install it as the "
                   "first time. If it pins a new QEMU, Vitrine then offers to update its QEMU.")
                    .arg(sourceDir.isEmpty() ? tr("its folder")
                                             : "<code>" + sourceDir.toHtmlEscaped() + "</code>");
        if (!r.url.isEmpty()) {
            text += QString(" <a href=\"%1\">%2</a>").arg(r.url.toHtmlEscaped(),
                                                           tr("See the changes"));
        }
        text += "</p>";
    }
    return text.isEmpty() ? tr("Nothing checked yet.") : text;
}

QString waitText(const QDateTime &notBefore)
{
    return tr("GitHub asked to wait before it is asked again, until %1.")
        .arg(QLocale().toString(notBefore.toLocalTime().time(), QLocale::ShortFormat));
}

QString nothingToCheck()
{
    return tr("There is nothing to check: this copy of Vitrine was not built from a git "
              "checkout.");
}

}
