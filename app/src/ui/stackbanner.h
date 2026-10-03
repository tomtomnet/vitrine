// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>

class Banner;
class QMainWindow;

/*
 * Across the top of the main window while Vitrine's QEMU needs the user:
 * not built yet, out of date, building, or its last build failed.  Hidden
 * when it is up to date, or when the preferences choose another QEMU.
 */
class StackBanner : public QWidget
{
    Q_OBJECT

public:
    explicit StackBanner(QWidget *parent = nullptr);

    /* Puts a banner over @window's central widget, which it keeps */
    static StackBanner *addTo(QMainWindow *window);

    void refresh();

signals:
    /* Build, Update or Show: the build window */
    void buildRequested();

private:
    Banner *m_warning;
    Banner *m_note;
    QString m_error;            // of the last build, if it failed
    int m_percent = -1;
};
