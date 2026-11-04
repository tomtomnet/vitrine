// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/settingspage.h"

#include <QList>

#include "core/vmconfig.h"

/*
 * The folders of this computer a VM uses, shared with virtiofs: the Qt
 * app's SharesPage, through the same core calls.  Each folder is a row
 * that opens on how the guest mounts it; Add and Edit ask in a dialog of
 * their own (ShareDialog), with the same checks.  virtiofs needs the
 * guest's memory to be shared memory: the banner says so, and fixes it.
 */
class SharesPage : public SettingsPage
{
public:
    SharesPage();

    QString title() const override { return "Shared Folders"; }
    const char *icon() const override { return "folder-remote-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

    /* What the dialog gives, for tests: @row -1 adds */
    void setShare(int row, const VmConfig::Share &share);
    void removeShare(int row);
    QList<VmConfig::Share> shares() const { return m_shares; }

private:
    void fill();
    void updateNotes();
    void edit(int row);

    AdwBanner *m_memory;
    AdwPreferencesGroup *m_missing;
    AdwPreferencesGroup *m_list;
    GtkWidget *m_add;
    QList<GtkWidget *> m_rows;

    QList<VmConfig::Share> m_shares;
    QList<VmConfig::Share> m_loaded;
    bool m_sharedMemory = false;
    bool m_fixMemory = false;
};
