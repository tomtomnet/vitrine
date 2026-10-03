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
class SettingsPage;
class SnapshotView;
class Vm;
class VmDetails;

/*
 * The VM selected in the list, beside it, in tabs: its details, its
 * settings, with their pages listed down the side as in a dialog, its
 * snapshots and its log.  The pages all edit a copy of the VM's arguments, which keeps their
 * changes from page to page until Apply saves them.
 */
class VmPane : public QWidget
{
    Q_OBJECT

public:
    enum Tab { Details, Settings, Snapshots, Logs };
    enum Page {
        General, System, Display, Storage, SharedFolders, PciDevices, UsbDevices, Arguments,
    };

    explicit VmPane(QWidget *parent = nullptr);
    ~VmPane() override;

    Vm *vm() const;
    /* Another VM gets new pages: the changes not applied to this one are lost */
    void setVm(Vm *vm);
    VmDetails *details() const { return m_details; }

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

private:
    void buildPages();
    void switchTo(int row);
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
    VmDetails *m_details;
    QListWidget *m_list;
    QLabel *m_title;
    QStackedWidget *m_stack;
    SnapshotView *m_snapshots;
    LogView *m_log;
    Banner *m_running;
    QPushButton *m_discard;
    QPushButton *m_apply;
};
