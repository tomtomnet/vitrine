// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <optional>

#include "core/importer.h"

class Vm;
class VmStore;
struct QemuInfo;

/*
 * Import VM: a VM from the QEMU command of a launch script, or of a
 * command line pasted in, showing the arguments it gets before making it:
 * the Qt app's ImportDialog, on the core's Importer.  The disks stay where
 * they are; the firmware files are copied into the VM's folder.
 */
class ImportDialog : public Gtk4::Dialog
{
public:
    using Created = std::function<void(Vm *vm)>;

    /* @info, if loaded, tells which options take a value */
    ImportDialog(VmStore *store, const QemuInfo *info, Presenter presenter, Created created);
    ~ImportDialog() override;

    /* What Open Script does with the file chosen */
    void load(const QString &path);
    void setScript(const QString &text);
    void setBaseDir(const QString &dir);
    QString name() const;
    void setName(const QString &name);
    /* The arguments the VM would get, as the preview shows them */
    QString preview() const;
    bool canCreate() const;
    /* What Create VM does */
    Vm *create();

private:
    void update();
    static QString textOf(GtkTextBuffer *buffer);

    VmStore *m_store;
    const QemuInfo *m_info;
    Created m_created;
    std::optional<Importer::Result> m_result;
    bool m_nameEdited = false;
    bool m_settingName = false;

    GtkTextBuffer *m_script;
    GtkTextBuffer *m_preview;
    AdwBanner *m_missing;
    GtkWidget *m_notes;
    GtkWidget *m_baseDir;
    GtkWidget *m_name;
    GtkWidget *m_create;
};
