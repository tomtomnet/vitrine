// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

/*
 * The banners of what the app builds, over the main window's views:
 *
 *  - Vitrine's QEMU not built yet, out of date, building, or its last
 *    build failed, with Build…, Update…, Show or Show… (the Qt app's
 *    StackBanner);
 *  - the host's KDE packages of Vitrine's, installed and no longer
 *    matching the system (KWin after a Plasma or Qt update...), with
 *    Rebuild… where the recipe has a set for the new version, Revert…
 *    where it has none (the Qt app's HostKdeBanner, whose two buttons an
 *    AdwBanner cannot hold: the Builds window, on the package, has both).
 *
 * The texts are the Qt app's (BuildTexts::stackNote(), HostKdeNotice).
 * Each place has banners of its own, which say the same (addTo()).  A
 * place hides them at once, unanimated, while a VM's screen shows there
 * (setScreenShown()): a banner coming or going would resize the guest's
 * screen twice; the tasks and the Builds window tell the build meanwhile.
 */
class AppBanners
{
public:
    AppBanners();
    ~AppBanners();
    AppBanners(const AppBanners &) = delete;
    AppBanners &operator=(const AppBanners &) = delete;

    /* Banners for @view, as a top bar under those it has */
    void addTo(AdwToolbarView *view);
    /* @view shows a VM's screen, or no longer: its banners out of sight meanwhile */
    void setScreenShown(AdwToolbarView *view, bool shown);
    /* All again: vitrine is back in front, and builds by hand may have made something */
    void refresh();

    /* Build…, Update…, Show, Rebuild…: the Builds window on @component */
    std::function<void(const QString &component)> showBuilds;
    /* Revert…: the distribution's packages back, of @ids */
    std::function<void(const QStringList &ids)> revert;

    /* For tests: what they say (plain text) and their buttons; empty while hidden */
    QString stackText() const;
    QString stackButton() const;
    QString hostText() const;
    QString hostButton() const;
    /* Their buttons */
    void activateStack();
    void activateHost();

private:
    struct Place {
        AdwToolbarView *view;
        GtkWidget *box;
        AdwBanner *stack;
        AdwBanner *host;
    };
    void refreshStack();
    void refreshHost();
    void show(AdwBanner *banner, const QString &text, const QString &button);

    QObject m_qt;
    QList<Place> m_places;
    /* of the last build of Vitrine's QEMU, if it failed */
    QString m_error;
    int m_percent = -1;
    QString m_stackText;
    QString m_stackButton;
    QString m_hostText;
    QString m_hostButton;
    /* what Rebuild… is for, and what Revert… puts back */
    QString m_rebuild;
    QStringList m_outdated;
};
