// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

#include "core/updatecheck.h"

class QTimer;

/*
 * Whether Vitrine has new commits on GitHub, as the Qt app's
 * UpdateNotifier tells it: one anonymous request (UpdateCheck) by itself
 * a while after the start and then once a day at most, and when asked
 * (Check for Updates…); the answer kept across restarts; nothing asked
 * for as long as GitHub says to wait.  Vitrine's QEMU is pinned by Vitrine
 * itself: an update of Vitrine updates it.
 *
 * The news: a dot on the primary menu's buttons, and their tooltip, while
 * there are new commits, and a notice when they come; the answers, in the
 * shared words (UpdateText), in an alert.
 */
class UpdateNotifier
{
public:
    UpdateNotifier();
    ~UpdateNotifier();
    UpdateNotifier(const UpdateNotifier &) = delete;
    UpdateNotifier &operator=(const UpdateNotifier &) = delete;

    /* Check for Updates…: the answer of a moment ago, else a check */
    void checkNow();
    /* What runs changed (Vitrine built again): the answers kept follow */
    void revalidate();
    /* A primary menu's button, which shows the news */
    void addMenuButton(GtkWidget *button);
    /* "vitrine: 3 new commits"; empty without news */
    QString newsText() const;

    /* What this computer runs, as far as it can tell */
    static QList<UpdateCheck::Project> projects();

    /* How the answers are shown: over the main window */
    std::function<void(AdwDialog *dialog)> present;
    /* How news are told as they come */
    std::function<void(const QString &text)> notify;

private:
    void maybeCheck();
    void start(bool asked);
    void finished(const QList<UpdateCheck::Result> &results);
    void showResults();
    void updateButtons();
    void save();

    QObject m_qt;
    UpdateCheck *m_check;
    QTimer *m_daily;
    QList<UpdateCheck::Result> m_results;
    bool m_asked = false;
    QList<GtkWidget *> m_buttons;
    /* the news last told: told once */
    QString m_told;
};
