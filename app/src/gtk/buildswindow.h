// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

#include "gtk/buildsmodel.h"
#include "gtk/guarded.h"

class BuildManager;

/*
 * The builds of what Vitrine builds (BuildManager), in a window of their
 * own, not modal, over the main window and destroyed with it: the VMs stay
 * at hand during a build, and closing it does not stop one.  The Qt app's
 * BuildsWindow, after the GNOME HIG (BuildsModel has its words and
 * states):
 *
 *  - the sidebar: an Overview, then the components by group (Vitrine's
 *    QEMU: virglrenderer, rutabaga_gfx, passt, QEMU; the guest tools of a
 *    Fedora release: their packages and the medium; the host's desktop,
 *    where offered: KWin, KSvg, Kirigami, Qt base), each with its state,
 *    a spinner while it builds;
 *  - the Overview: the run (what builds, how far, Stop) or what there is
 *    to build (Update All), each group's components with their state and
 *    when they were built, the group's Build (and the host desktop's
 *    Install All…), the guest tools' Fedora release, what the host lacks
 *    for the builds, the build cache and Clear…;
 *  - a component: Build or Update, Stop, a host package's Install… and
 *    Revert…, Build from Scratch, its output folder; its Overview (its state
 *    and why, where it comes from), its patches by origin (their text on
 *    demand), its log, as it comes.
 *
 * Waiting for memory before a heavy package, a banner says so, with Build
 * Anyway.  Narrow, it is one page at a time.
 */
class BuildsWindow : public Gtk4::Guarded
{
public:
    explicit BuildsWindow(GtkWindow *parent, BuildManager *manager = nullptr);
    ~BuildsWindow();
    BuildsWindow(const BuildsWindow &) = delete;
    BuildsWindow &operator=(const BuildsWindow &) = delete;

    /* Null once it is destroyed */
    GtkWindow *window() const { return m_destroyed ? nullptr : GTK_WINDOW(m_window); }
    /* In front, on @component if given (a component's id, a group's, "" for the Overview) */
    void present(const QString &component = {});
    void select(const QString &component);
    /* The component shown, "" for the Overview */
    QString selected() const;
    /* Vitrine back in front: the states again, what builds by hand made meanwhile; not during a run */
    void lookAgain();
    /* @dialog over this window */
    void presentDialog(AdwDialog *dialog);
    /* It closed: the window is gone, its owner lets it go */
    std::function<void()> closed;

    /* For tests: what the sidebar says of @id, and the Overview */
    QString itemSubtitle(const QString &id) const;
    QStringList items() const;
    QString runText() const;
    QString buildLabel() const;
    bool canBuild() const;
    QString cacheText() const;
    QString memoryText() const;
    /* What its Overview, patches and log show of the component selected (plain text) */
    QString detailsText() const;
    QStringList patchRows() const;
    QString logText() const;
    /* The buttons */
    void buildSelected(bool clean = false);
    void buildAll();
    void stop();

private:
    struct Item {
        QString id;
        AdwSidebarItem *item;
        /* the Look its suffix shows, -1 for none yet */
        int look;
    };
    /* A component's row on the Overview */
    struct Row {
        GtkWidget *row;
        GtkWidget *built;
        GtkWidget *progress;
        GtkWidget *icon;
    };
    struct GroupBox {
        BuildsModel::Group group;
        AdwPreferencesGroup *box;
        GtkWidget *build;
        GtkWidget *install;     // the host desktop's Install All…
        GtkWidget *release;     // the guest tools' Fedora release
    };

    void buildSidebar();
    GtkWidget *buildOverview();
    GtkWidget *buildComponentPage();
    void createActions();
    /* The components again, as the sources or the guest tools' release have them */
    void fill();
    void showSelected();
    void refreshAll();
    void refreshItem(const QString &id);
    void refreshRun();
    void refreshMemory();
    void refreshMissing();
    void refreshButtons();
    void refreshDetails();
    void refreshPatches();
    void refreshLog();
    void appendLog(const QString &text);
    void updateCache();
    void clearCache();
    void contextMenuFor(const QString &id);
    /* The size of @path, counted in the background; -1 until then */
    qint64 size(const QString &path);
    void openOutput();
    void copyOutputPath();
    void install(const QString &id);
    void revert();
    void setSeen();
    void saveSize();

    BuildManager *m_manager;
    QObject m_qt;
    bool m_filling = false;
    QString m_selected;
    QList<Item> m_items;
    QHash<QString, Row> m_rows;
    QList<GroupBox> m_groupBoxes;
    QHash<QString, qint64> m_sizes;
    qint64 m_cacheSize = -1;
    gint64 m_detailsShown = 0;
    /* the log shown: the component's, or a host package's install or revert */
    QString m_shownLog;
    bool m_showInstall = false;
    QStringList m_installIds;
    QString m_shownPatches;

    GtkWidget *m_window;
    bool m_destroyed = false;
    AdwNavigationSplitView *m_split;
    AdwSidebar *m_sidebar;
    AdwSidebarSection *m_overviewSection;
    QList<AdwSidebarSection *> m_sections;
    GSimpleActionGroup *m_actions;
    /* the Overview */
    AdwNavigationPage *m_overviewPage;
    AdwPreferencesPage *m_overview;
    AdwBanner *m_overviewMemory;
    AdwActionRow *m_runRow;
    GtkWidget *m_runSpinner;
    GtkWidget *m_runProgress;
    GtkWidget *m_background;
    GtkWidget *m_all;
    GtkWidget *m_stopAll;
    AdwPreferencesGroup *m_missingGroup;
    GtkWidget *m_missing;
    AdwPreferencesGroup *m_cacheGroup;
    AdwActionRow *m_cacheRow;
    GtkWidget *m_clear;
    /* a component */
    AdwNavigationPage *m_componentPage;
    GtkWidget *m_componentHeader = nullptr;
    AdwWindowTitle *m_componentTitle;
    AdwBanner *m_componentMemory;
    AdwViewStack *m_views;
    AdwViewSwitcherBar *m_switcherBar;
    GtkWidget *m_build;
    GtkWidget *m_stop;
    GtkWidget *m_menu;
    /* a host package's Install… and Revert…, in its Overview */
    GtkWidget *m_installRow = nullptr;
    GtkWidget *m_revertRow = nullptr;
    AdwPreferencesPage *m_details;
    QList<AdwPreferencesGroup *> m_detailGroups;
    GtkWidget *m_patchesStack;
    AdwPreferencesPage *m_patches;
    QList<AdwPreferencesGroup *> m_patchGroups;
    GtkWidget *m_logStack;
    GtkTextBuffer *m_log;
    GtkWidget *m_logView;
};
