// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>
#include <QWidget>

class Banner;
class QMainWindow;
class QPushButton;

/*
 * Across the top of the main window while one of Vitrine's host packages is
 * installed and no longer matches the system: KWin after a Plasma or Qt
 * update (at the next login the desktop would run it against other
 * libraries, and might not start), KSvg or Kirigami once the KDE Frameworks
 * moved to another release (mixed releases, which upstream does not
 * support), Kirigami after a Qt update.  Rebuild… opens the Builds window on
 * the first of them that the recipe has a set for; Revert… puts the
 * distribution's back, all of them at once.  Hidden otherwise, which is
 * nearly always.
 */
class HostKdeBanner : public QWidget
{
    Q_OBJECT

public:
    explicit HostKdeBanner(QWidget *parent = nullptr);

    /* Puts a banner over @window's central widget, which it keeps */
    static HostKdeBanner *addTo(QMainWindow *window);

    void refresh();
    /* What it says, for the tests; empty while hidden */
    QString text() const;
    /* The components out of date, and the one Rebuild… is for (empty: none
       has a set for its new version) */
    QStringList outdated() const;
    QString rebuildable() const;

signals:
    void rebuildRequested(const QString &id);
    void revertRequested(const QStringList &ids);

private:
    Banner *m_banner;
};
