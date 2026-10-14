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
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * The builds of what Vitrine builds (BuildManager), in one window, not
 * modal: Vitrine's QEMU (virglrenderer, QEMU) and the guest tools (their
 * packages and the medium), each with its state and why, when it was
 * built and how long a build of it takes; the selected one's source,
 * patches by origin (their text on demand) and log.  Build for the
 * selected one, Update All for what is out of date, Stop; the cache's
 * size, and Clear.  The builds go on when it closes.
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

    /* The component shown: "qemu", or a group: "host", "guest" */
    void select(const QString &component);
    QString selected() const;

    /* "Vitrine's build of 3 October 2026, QEMU 11.1.50 + 7 patches" */
    static QString describe(const StackBuilder::Build &build);
    /* What the state of Vitrine's QEMU means for the user, in a sentence or two */
    static QString explain(StackBuilder::State state, const StackBuilder::Build &build);
    /* "Built on 5 October 2026: vitrine-guest-tools 0.1.0-14, Mesa 26.2.3, KWin 6.7.5" */
    static QString describeGuestTools();
    static QString explain(GuestToolsBuilder::State state);
    /* "18 min", "1 h 20 min", "less than a minute" */
    static QString duration(qint64 ms);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void fill();
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
    /* The size of @path, counted in the background; -1 until then */
    qint64 size(const QString &path);
    /* The components of the selection: one, or a group's */
    QStringList selectedComponents() const;

    BuildManager *m_manager;
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
    QHash<QString, QTreeWidgetItem *> m_items;
    QHash<QString, QTreeWidgetItem *> m_groups;
    QString m_shownPatches;
    QString m_shownLog;
    QHash<QString, qint64> m_sizes;
    QElapsedTimer m_sourceShown;
    qint64 m_cacheSize = -1;
};
