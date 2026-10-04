// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

#include "core/argsfile.h"

class Banner;
class LogView;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class SettingsPage;
class SnapshotView;
class Vm;
class VmDetails;

/*
 * The VM selected in the list, beside it, in tabs: its console (its screen,
 * or a page to start it), its details, its settings, with their pages
 * listed down the side as in a dialog, its snapshots and its log.  The
 * pages all edit a copy of the VM's arguments, which keeps their
 * changes from page to page until Apply saves them.
 *
 * The simple pages are listed first; the advanced ones, down to the
 * command line itself, under Advanced, folded until opened.
 */
class VmPane : public QWidget
{
    Q_OBJECT

public:
    enum Tab { Console, Details, Settings, Snapshots, Logs };
    enum Page {
        General, Hardware, Display, Storage, SharedFolders, UsbDevices, Network,
        /* under Advanced */
        Machine, Boot, PciDevices, Arguments,
    };
    static constexpr Page FirstAdvanced = Machine;

    explicit VmPane(QWidget *parent = nullptr);
    ~VmPane() override;

    Vm *vm() const;
    /* Another VM gets new pages: the changes not applied to this one are lost */
    void setVm(Vm *vm);
    VmDetails *details() const { return m_details; }
    /*
     * What the Console tab shows: MainWindow's consoles, which outlive the
     * pages, as the screens in them must not be moved
     */
    void setConsole(QWidget *console);

    Tab tab() const;
    void setTab(Tab tab);
    /* The settings page shown, or to show once there is a VM */
    Page page() const;
    void setPage(Page page);

    /* Changes not applied yet */
    bool isModified() const;
    /*
     * Asks whether to apply the changes, if any, or discard them, with
     * @question, e.g. "Apply them before closing?": false if the user
     * cancels, or they cannot be saved
     */
    bool confirmChanges(const QString &question);
    bool apply();
    void discard();

signals:
    /* Start From It, on the Snapshots tab */
    void startFromSnapshot(const QString &name);
    /* Another tab shows, chosen or set */
    void tabChanged(Tab tab);

private:
    void buildPages();
    /* A row of a list of pages was selected */
    void pageChosen(QListWidget *list, int row);
    /* Makes @page current in its list, opening Advanced for it */
    void selectPage(int page);
    void showAdvanced(bool on);
    void switchTo(int page);
    void vmChanged();
    void updateFooter();
    void watchEdits(SettingsPage *page);

    QPointer<Vm> m_vm;
    /* The arguments the pages edit, and those they started from */
    ArgsFile m_args;
    QString m_loaded;
    QList<SettingsPage *> m_pages;
    int m_current = -1;
    Page m_page = General;
    QTimer *m_check;
    QTabWidget *m_tabs;
    QWidget *m_console;
    VmDetails *m_details;
    QWidget *m_side;
    QListWidget *m_list;
    QToolButton *m_more;
    QListWidget *m_advanced;
    QLabel *m_title;
    QStackedWidget *m_stack;
    SnapshotView *m_snapshots;
    LogView *m_log;
    Banner *m_running;
    QPushButton *m_discard;
    QPushButton *m_apply;
};
