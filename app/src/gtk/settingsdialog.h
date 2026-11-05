// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QList>
#include <QPointer>

#include <functional>
#include <memory>

#include "core/argsfile.h"
#include "core/firmwarefiles.h"

class SettingsPage;
class Vm;
class VmStore;

/*
 * The settings of a VM, after GNOME Settings: its pages in a sidebar, in
 * sections, the page beside it; narrow, the sidebar is a list of the
 * pages, a page shown over it.  It is the Qt app's Settings tab (VmPane):
 * the pages edit one copy of the VM's arguments, which keeps their
 * changes from page to page until Apply saves them; Ctrl+S applies too.
 * Closing with changes not applied asks whether to apply or discard them.
 * While the VM runs, a banner says the changes apply at its next start.
 *
 * The pages, as the Qt app has them: General, Hardware, Display, Storage,
 * Network, Shared Folders, USB Devices, then the advanced ones: Machine
 * (with the firmware and the boot), PCI Devices, and Arguments, the
 * command line itself.
 */
class SettingsDialog : public Gtk4::Dialog
{
public:
    /* The pages, in the Qt app's order (VmPane::Page), which names them the same */
    enum Page {
        General, Hardware, Display, Storage, SharedFolders, UsbDevices, Network, Machine,
        PciDevices, Arguments,
    };
    /* The page by the name the settings file keeps, and back; General for another */
    static const char *pageKey(Page page);
    static Page pageOf(const QString &key);

    /* What the window does for the pages: put new firmware copies in place, asking first */
    using ResetFirmware = std::function<void(Vm *vm, const QList<FirmwareFiles::File> &files,
                                             const QString &accept, std::function<void(bool)> done)>;

    SettingsDialog(Vm *vm, VmStore *store, Presenter presenter, ResetFirmware resetFirmware = {});
    ~SettingsDialog() override;

    Vm *vm() const;
    Page page() const;
    /* The page shown; narrow, over the list */
    void setPage(Page page);
    SettingsPage *pageWidget(Page page) const;

    /* Changes not applied yet */
    bool isModified() const;
    /* Saves them, or says why it cannot: true when saved */
    bool apply();
    void discard();
    /*
     * Asks whether to apply the changes, if any, or discard them, with
     * @question ("Apply them before closing?"); @done(true) once applied or
     * discarded, (false) if the user cancels or they cannot be saved
     */
    void confirmChanges(const QString &question, std::function<void(bool go)> done);

private:
    void addPage(Page page, std::unique_ptr<SettingsPage> settings);
    void switchTo(Page page);
    void vmChanged();
    void update();
    void closed() override;

    QPointer<Vm> m_vm;
    ResetFirmware m_resetFirmware;
    /* The arguments the pages edit, and those they started from */
    ArgsFile m_args;
    QString m_loaded;
    std::unique_ptr<SettingsPage> m_pages[Arguments + 1];
    int m_current = -1;
    /* Asking whether to apply the changes, on a close */
    bool m_asking = false;

    AdwNavigationSplitView *m_split;
    AdwNavigationPage *m_content;
    AdwViewStack *m_stack;
    AdwBanner *m_running;
    GtkWidget *m_apply;
};
