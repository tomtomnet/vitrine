// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>

#include "core/guesttoolsbuilder.h"
#include "core/stackbuilder.h"

class Banner;
class BuildManager;
namespace Builds {
struct Component;
}
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * The builds of what Vitrine builds (BuildManager), in one window, not
 * modal: Vitrine's QEMU (virglrenderer, rutabaga_gfx, QEMU), the guest
 * tools (their packages and the medium) and, optional, the host's desktop
 * (the host's KWin), each with its state and why, when it was built and how
 * long a build of it takes; the selected one's source, patches by origin
 * (their text on demand) and log.  Build for the selected one, Update All
 * for what is out of date, Stop; Install… and Revert… for the host's KWin;
 * the cache's size, and Clear.  The builds go on when it closes.
 */
class BuildsWindow : public QDialog
{
    Q_OBJECT

public:
    explicit BuildsWindow(QWidget *parent = nullptr);
    ~BuildsWindow() override;

    /* The window over @from's window, on @component if given: the one open
       already if it is over that one, else a new one */
    static BuildsWindow *present(QWidget *from, const QString &component = {});

    /* The component shown: "qemu", or a group: "host", "guest", "system" */
    void select(const QString &component);
    QString selected() const;

    /* "Vitrine's build of 3 October 2026, QEMU 11.1.50 + 7 patches" */
    static QString describe(const StackBuilder::Build &build);
    /* What the state of Vitrine's QEMU means for the user, in a sentence or two */
    static QString explain(StackBuilder::State state, const StackBuilder::Build &build);
    /* "Built on 5 October 2026: vitrine-guest-tools 0.1.0-14, Mesa 26.2.3, KWin 6.7.5" */
    static QString describeGuestTools();
    static QString explain(GuestToolsBuilder::State state);
    /* "40 s", "18 min", "1 h 20 min": how long a build took */
    static QString duration(qint64 ms);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void fill();
    /* The tree as high as its rows, the details below the rest */
    void fitTree();
    /* Shown: the states again, the cache's size */
    void shown();
    /* Back in front: the states again, with what builds run by hand made
       meanwhile; not during a run */
    void lookAgain();
    void refreshItems();
    void refreshItem(const QString &id);
    void refreshGroup(const QString &group);
    void showDetails();
    void showSource();
    void showPatches();
    void showPatchText();
    void showLog();
    void showMissing();
    void showMemory();
    void updateButtons();
    void updateHeader();
    void updateCache();
    void clearCache();
    void buildSelected(bool clean = false);
    void contextMenu(const QPoint &at);
    /* The host's KWin's part of the details: what it is, the system's state */
    QString kwinSource(const Builds::Component &c);
    /* The host's KWin, or its group, is selected */
    bool kwinSelected() const;
    /* The size of @path, counted in the background; -1 until then */
    qint64 size(const QString &path);
    /* The components of the selection: one, or a group's */
    QStringList selectedComponents() const;

    BuildManager *m_manager;
    QSplitter *m_split = nullptr;
    Banner *m_memory;
    QLabel *m_header;
    QLabel *m_missing;
    QTreeWidget *m_tree;
    QTabWidget *m_tabs;
    QLabel *m_source;
    QTreeWidget *m_patches;
    QPlainTextEdit *m_patchText;
    QPlainTextEdit *m_log;
    QLabel *m_background;
    QLabel *m_cache;
    QPushButton *m_clear;
    QPushButton *m_build;
    QPushButton *m_all;
    QPushButton *m_stop;
    QPushButton *m_install;
    QPushButton *m_revert;
    /* the Log tab of the host's KWin shows its last install or revert */
    bool m_showInstall = false;
    QHash<QString, QTreeWidgetItem *> m_items;
    QHash<QString, QTreeWidgetItem *> m_groups;
    QString m_shownPatches;
    QString m_shownLog;
    QHash<QString, qint64> m_sizes;
    QElapsedTimer m_sourceShown;
    qint64 m_cacheSize = -1;
};
