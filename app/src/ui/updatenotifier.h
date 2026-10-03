// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>

#include "core/updatecheck.h"

class QTimer;
class QToolButton;
class QWidget;

/*
 * Tells when qemu-gui-manager, or the qemu-gui QEMU it builds, has new
 * commits on GitHub: by itself once a day at most, and when asked.  GitHub
 * lets 60 requests an hour through without an account; this makes two a
 * day, keeps the answer across restarts, and waits as long as GitHub asks
 * when it says there were too many.
 */
class UpdateNotifier : public QObject
{
    Q_OBJECT

public:
    explicit UpdateNotifier(QWidget *window);

    /* For the status bar: shown while there are updates */
    QToolButton *button() const { return m_button; }
    /* Help > Check for Updates */
    void checkNow();
    /* What runs changed, e.g. File > Build QEMU built: the answers kept follow */
    void revalidate();

    /* What this computer runs, as far as it can tell */
    static QList<UpdateCheck::Project> projects();

signals:
    void buildQemuRequested();

private:
    void maybeCheck();
    void start(bool asked);
    void finished(const QList<UpdateCheck::Result> &results);
    void showResults();
    void updateButton();
    void save();

    QWidget *m_window;
    UpdateCheck *m_check;
    QToolButton *m_button;
    QTimer *m_daily;
    QList<UpdateCheck::Result> m_results;
    bool m_asked = false;
};
