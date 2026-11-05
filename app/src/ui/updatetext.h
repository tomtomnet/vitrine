// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include "core/updatecheck.h"

/*
 * What the check for updates says, in both apps: what GitHub answered of
 * each project (up to date; new commits, the latest subjects, how to
 * update, the page of the changes; or why it could not tell), and why
 * nothing was asked.  Rich text, in the Qt app's UpdateNotifier's
 * translation context; Qt Core only.
 */
namespace UpdateText {

/* "1 new commit", "12 new commits" */
QString newCommits(int count);
/* The answers, @sourceDir the checkout this copy was built from (empty: not known) */
QString results(const QList<UpdateCheck::Result> &results, const QString &sourceDir);
/* Asked again while GitHub asked to wait, until @notBefore */
QString waitText(const QDateTime &notBefore);
/* Asked, with nothing to check */
QString nothingToCheck();

}
