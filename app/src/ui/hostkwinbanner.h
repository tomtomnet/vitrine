// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>

class Banner;
class QMainWindow;

/*
 * Across the top of the main window while Vitrine's KWin is installed on
 * the host and no longer matches its Plasma or Qt: at the next login the
 * desktop would run it against other libraries, and might not start.
 * Rebuild… opens the Builds window on it, Revert… puts the distribution's
 * KWin back.  Hidden otherwise, which is nearly always.
 */
class HostKwinBanner : public QWidget
{
    Q_OBJECT

public:
    explicit HostKwinBanner(QWidget *parent = nullptr);

    /* Puts a banner over @window's central widget, which it keeps */
    static HostKwinBanner *addTo(QMainWindow *window);

    void refresh();
    /* What it says, for the tests; empty while hidden */
    QString text() const;

signals:
    void rebuildRequested();
    void revertRequested();

private:
    Banner *m_banner;
};
