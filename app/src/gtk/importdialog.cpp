// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/importdialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstring>

#include "core/firmware.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmstore.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"

using Gtk4::utf8;

/* A text area, monospace, never wrapped: a script, or the arguments made of it */
static GtkWidget *textArea(GtkTextBuffer *buffer, bool editable, const char *name)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new_with_buffer(buffer);

    gtk_widget_set_name(view, name);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), true);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    for (const auto margin : {&gtk_text_view_set_left_margin, &gtk_text_view_set_right_margin,
                              &gtk_text_view_set_top_margin, &gtk_text_view_set_bottom_margin}) {
        margin(GTK_TEXT_VIEW(view), 8);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_add_css_class(scroll, "card");
    gtk_widget_set_overflow(scroll, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(scroll, -1, 100);
    return scroll;
}

ImportDialog::ImportDialog(VmStore *store, const QemuInfo *info, Presenter presenter,
                           Created created)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_store(store), m_info(info),
      m_created(std::move(created)), m_script(gtk_text_buffer_new(nullptr)),
      m_preview(gtk_text_buffer_new(nullptr)), m_missing(ADW_BANNER(adw_banner_new("")))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
    GtkWidget *open = gtk_button_new_with_mnemonic("_Open Script…");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *intro = Gtk4::caption("Open the script you start the VM with, or paste its QEMU "
                                     "command line. The new VM gets the options of the command. "
                                     "Its disks stay where they are; its firmware files are copied "
                                     "into its folder, to go wherever it goes.");
    GtkWidget *rows = gtk_list_box_new();
    GtkWidget *chooseDir = Gtk4::rowButton("folder-open-symbolic", "Choose the Folder");
    GtkWidget *scriptLabel = Gtk4::label("Script or command line");
    GtkWidget *previewLabel = Gtk4::label("The arguments of the new VM");
    GtkWidget *script = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *preview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    adw_dialog_set_title(dialog(), "Import a VM");
    adw_dialog_set_content_width(dialog(), 760);
    adw_dialog_set_content_height(dialog(), 680);
    m_create = gtk_button_new_with_mnemonic("C_reate VM");
    gtk_widget_add_css_class(m_create, "suggested-action");
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_create);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), open);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    /* the files the command names but are not where it runs from */
    adw_banner_set_button_label(m_missing, "Choose Their _Folder…");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_missing));

    for (const auto side : {&gtk_widget_set_margin_start, &gtk_widget_set_margin_end,
                            &gtk_widget_set_margin_top, &gtk_widget_set_margin_bottom}) {
        side(box, 12);
    }
    gtk_box_append(GTK_BOX(box), intro);

    gtk_widget_add_css_class(scriptLabel, "heading");
    gtk_widget_add_css_class(previewLabel, "heading");
    gtk_box_append(GTK_BOX(script), scriptLabel);
    GtkWidget *scriptArea = textArea(m_script, true, "script");
    gtk_widget_set_vexpand(scriptArea, true);
    gtk_box_append(GTK_BOX(script), scriptArea);
    gtk_box_append(GTK_BOX(preview), previewLabel);
    GtkWidget *previewArea = textArea(m_preview, false, "preview");
    gtk_widget_set_vexpand(previewArea, true);
    gtk_box_append(GTK_BOX(preview), previewArea);
    gtk_paned_set_start_child(GTK_PANED(paned), script);
    gtk_paned_set_end_child(GTK_PANED(paned), preview);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), false);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), false);
    gtk_widget_set_vexpand(paned, true);
    gtk_box_append(GTK_BOX(box), paned);

    m_notes = Gtk4::label({}, true);
    gtk_widget_add_css_class(m_notes, "caption");
    gtk_box_append(GTK_BOX(box), m_notes);

    /* where it runs from, and the VM's name */
    m_baseDir = Gtk4::entryRow("Runs From", QDir::homePath());
    gtk_widget_set_name(m_baseDir, "baseDir");
    gtk_widget_set_tooltip_text(m_baseDir, "Where the script runs from: relative paths in the "
                                           "command are relative to this folder");
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(m_baseDir), chooseDir);
    m_name = Gtk4::entryRow("Name");
    gtk_widget_set_name(m_name, "name");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(rows, "boxed-list");
    gtk_list_box_append(GTK_LIST_BOX(rows), m_baseDir);
    gtk_list_box_append(GTK_LIST_BOX(rows), m_name);
    gtk_box_append(GTK_BOX(box), rows);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), box);
    adw_dialog_set_child(dialog(), view);

    on<void()>(cancel, "clicked", [this]() { close(); });
    on<void()>(m_create, "clicked", [this]() { create(); });
    on<void()>(open, "clicked", [this, open]() {
        Gtk4::chooseFile(open, "Open Script", {}, Gtk4::text(m_baseDir),
                         guarded<void(const QString &)>([this](const QString &path) { load(path); }));
    });
    /* the script runs from where the files are */
    on<void()>(m_missing, "button-clicked", [this]() {
        Gtk4::chooseFolder(GTK_WIDGET(dialog()),
                           QString("The Folder of %1")
                               .arg(m_result ? m_result->missing.value(0) : QString()),
                           Gtk4::text(m_baseDir),
                           guarded<void(const QString &)>(
                               [this](const QString &dir) { setBaseDir(dir); }));
    });
    on<void()>(chooseDir, "clicked", [this]() {
        Gtk4::chooseFolder(GTK_WIDGET(dialog()), "The Folder the Script Runs From",
                           Gtk4::text(m_baseDir),
                           guarded<void(const QString &)>(
                               [this](const QString &dir) { setBaseDir(dir); }));
    });
    on<void()>(m_script, "changed", [this]() { update(); });
    on<void()>(m_baseDir, "changed", [this]() { update(); });
    on<void()>(m_name, "changed", [this]() {
        if (!m_settingName) {
            m_nameEdited = true;
        }
        gtk_widget_set_sensitive(m_create, canCreate());
    });
    update();
}

ImportDialog::~ImportDialog()
{
    stopCallbacks();
    g_object_unref(m_script);
    g_object_unref(m_preview);
}

QString ImportDialog::textOf(GtkTextBuffer *buffer)
{
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, true);
    const QString result = QString::fromUtf8(text);
    g_free(text);
    return result;
}

void ImportDialog::load(const QString &path)
{
    QFile f(path);

    if (!f.open(QIODevice::ReadOnly)) {
        present(Gtk4::alert("Import a VM", f.errorString()));
        return;
    }
    setBaseDir(QFileInfo(path).absolutePath());
    if (!m_nameEdited) {
        /* start_fedora-kde.sh: fedora-kde */
        QString name = QFileInfo(path).completeBaseName();
        for (const char *prefix : {"start_", "start-", "run_", "run-"}) {
            if (name.startsWith(prefix) && name.size() > int(strlen(prefix))) {
                name = name.mid(int(strlen(prefix)));
            }
        }
        m_settingName = true;
        Gtk4::setText(m_name, name);
        m_settingName = false;
    }
    setScript(QString::fromUtf8(f.readAll()));
}

void ImportDialog::setScript(const QString &text)
{
    gtk_text_buffer_set_text(m_script, utf8(text).constData(), -1);
}

void ImportDialog::setBaseDir(const QString &dir)
{
    Gtk4::setText(m_baseDir, dir);
}

QString ImportDialog::name() const
{
    return Gtk4::text(m_name).trimmed();
}

void ImportDialog::setName(const QString &name)
{
    Gtk4::setText(m_name, name);
}

QString ImportDialog::preview() const
{
    return textOf(m_preview);
}

bool ImportDialog::canCreate() const
{
    return m_result && !name().isEmpty();
}

void ImportDialog::update()
{
    const QString script = textOf(m_script);
    std::function<bool(const QString &)> takesValue;

    if (m_info && !m_info->options.isEmpty()) {
        takesValue = [this](const QString &name) {
            const QemuOptionDoc *o = m_info->option(name);
            return o ? o->takesValue : true;
        };
    }
    m_result = Importer::importScript(script, Gtk4::text(m_baseDir), takesValue);

    if (!m_result) {
        gtk_text_buffer_set_text(m_preview, "", -1);
        gtk_label_set_text(GTK_LABEL(m_notes),
                           script.trimmed().isEmpty()
                               ? ""
                               : "No command runs QEMU (qemu-system-…) in this text.");
    } else {
        const QString name = VmConfig::name(m_result->args);
        if (!name.isEmpty() && !m_nameEdited) {
            m_settingName = true;
            Gtk4::setText(m_name, name);
            m_settingName = false;
        }
        gtk_text_buffer_set_text(m_preview, utf8(m_result->args.toText()).constData(), -1);
        QStringList notes;
        for (const QString &note : std::as_const(m_result->notes)) {
            notes << "• " + note;
        }
        gtk_label_set_text(GTK_LABEL(m_notes), utf8(notes.join('\n')).constData());
    }
    gtk_widget_set_visible(m_notes, !QString::fromUtf8(gtk_label_get_text(GTK_LABEL(m_notes))).isEmpty());
    if (m_result && !m_result->missing.isEmpty()) {
        adw_banner_set_title(
            m_missing,
            utf8(QString("Not found in %1: %2. Choose the folder the script runs from, where they "
                         "are; otherwise the VM looks for them in its own folder.")
                     .arg(Gtk4::text(m_baseDir), m_result->missing.join(", "))
                     .toHtmlEscaped())
                .constData());
        adw_banner_set_revealed(m_missing, true);
    } else {
        adw_banner_set_revealed(m_missing, false);
    }
    gtk_widget_set_sensitive(m_create, canCreate());
}

Vm *ImportDialog::create()
{
    QString error;

    if (!canCreate()) {
        return nullptr;
    }
    ArgsFile args = m_result->args;
    VmConfig::setName(args, name());
    Vm *vm = m_store->create(name(), &error);
    if (!vm || !FirmwareDb::copyIntoVm(args, vm->dir(), nullptr, &error) ||
        !vm->save(args, &error)) {
        if (vm) {
            m_store->remove(vm);
        }
        present(Gtk4::alert("Import a VM", QString("Cannot create the VM: %1").arg(error)));
        return nullptr;
    }
    const Created created = m_created;
    forceClose();
    if (created) {
        created(vm);
    }
    return vm;
}
