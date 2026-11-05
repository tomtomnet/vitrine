// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/clonedialog.h"

#include <QFileInfo>

#include "core/tasks.h"
#include "core/vmcloner.h"
#include "core/vmstore.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"

using Gtk4::utf8;

static QString bytesText(qint64 bytes)
{
    return bytes >= (1LL << 30) ? QString("%1 GiB").arg(double(bytes) / (1LL << 30), 0, 'f', 1)
           : bytes >= (1LL << 20) ? QString("%1 MiB").arg(bytes >> 20)
                                  : QString("%1 KiB").arg(qMax<qint64>(1, bytes >> 10));
}

CloneDialog::CloneDialog(VmStore *store, Vm *source, VmCloner *cloner, Start start,
                         Presenter presenter)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_source(source),
      m_start(std::move(start))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = Gtk4::group();
    const QList<std::pair<QString, qint64>> files = VmCloner::filesToCopy(source);
    QStringList names;
    qint64 total = 0;
    QString name = QString("%1 clone").arg(source->name());

    adw_dialog_set_title(dialog(), utf8(QString("Clone %1").arg(source->name())).constData());
    adw_dialog_set_content_width(dialog(), 520);
    m_ok = gtk_button_new_with_mnemonic("C_lone");
    gtk_widget_add_css_class(m_ok, "suggested-action");
    m_cancel = gtk_button_new_with_mnemonic("_Cancel");
    m_stop = gtk_button_new_with_mnemonic("_Stop");
    gtk_widget_add_css_class(m_stop, "destructive-action");
    m_close = gtk_button_new_with_mnemonic("C_lose");
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_cancel);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_stop);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_ok);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_close);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_dialog_set_default_widget(dialog(), m_ok);

    /* a name no other VM has */
    auto taken = [store](const QString &n) {
        for (const Vm *vm : store->vms()) {
            if (vm->name() == n) {
                return true;
            }
        }
        return false;
    };
    for (int i = 2; taken(name); i++) {
        name = QString("%1 clone %2").arg(source->name()).arg(i);
    }
    m_name = Gtk4::entryRow("Name", name);
    gtk_widget_set_name(m_name, "name");
    adw_entry_row_set_activates_default(ADW_ENTRY_ROW(m_name), true);
    adw_preferences_group_add(group, m_name);

    for (const auto &[file, bytes] : files) {
        names << QString("%1 (%2)").arg(QFileInfo(file).fileName(), bytesText(bytes));
        total += bytes;
    }
    GtkWidget *what = Gtk4::caption(
        files.isEmpty()
            ? QString("The new VM gets the same settings. It has no disk to copy.")
            : QString("The new VM gets the same settings, and copies of what the VM writes, %1 in "
                      "all: %2. On btrfs and XFS, the copies take no time nor space until a VM "
                      "changes them.")
                  .arg(bytesText(total), names.join(", ")));
    GtkWidget *shared = Gtk4::caption("CD/DVD images and the other files the VM only reads are "
                                      "shared. The network cards get new addresses.");
    gtk_widget_set_margin_top(what, 12);
    gtk_widget_set_margin_top(shared, 6);
    adw_preferences_group_add(group, what);
    adw_preferences_group_add(group, shared);

    m_progress = gtk_progress_bar_new();
    gtk_widget_set_margin_top(m_progress, 18);
    m_background = Gtk4::caption(QString("Closing this window does not stop the copy. %1 cannot "
                                         "start until it is done.")
                                     .arg(source->name()));
    gtk_widget_set_margin_top(m_background, 6);
    m_error = Gtk4::label({});
    gtk_widget_add_css_class(m_error, "error");
    gtk_widget_set_margin_top(m_error, 12);
    gtk_widget_set_visible(m_error, false);
    adw_preferences_group_add(group, m_progress);
    adw_preferences_group_add(group, m_background);
    adw_preferences_group_add(group, m_error);
    adw_preferences_page_add(page, group);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(page));
    adw_dialog_set_child(dialog(), view);

    on<void()>(m_ok, "clicked", [this]() { clone(); });
    on<void()>(m_cancel, "clicked", [this]() { close(); });
    on<void()>(m_close, "clicked", [this]() { forceClose(); });
    on<void()>(m_stop, "clicked", [this]() {
        if (m_cloner) {
            m_cloner->cancel();
        }
    });
    on<void()>(m_name, "changed", [this]() { update(); });
    if (cloner) {
        watch(cloner);
    }
    update();
}

QString CloneDialog::name() const
{
    return Gtk4::text(m_name).trimmed();
}

void CloneDialog::setName(const QString &name)
{
    Gtk4::setText(m_name, name);
}

void CloneDialog::clone()
{
    if ((m_cloner && m_cloner->isRunning()) || name().isEmpty() || !m_source || !m_start) {
        return;
    }
    gtk_widget_set_visible(m_error, false);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m_progress), 0);
    VmCloner *cloner = m_start(m_source, name());
    if (cloner) {
        watch(cloner);
    }
    update();
}

void CloneDialog::watch(VmCloner *cloner)
{
    m_cloner = cloner;
    QObject::connect(cloner, &VmCloner::progress, qt(), [this](qint64 done, qint64 total) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m_progress),
                                      total > 0 ? double(done) / double(total) : 0);
    });
    QObject::connect(cloner, &VmCloner::finished, qt(), [this, cloner](Vm *clone, const QString &error) {
        const Task *task = cloner->task();

        update();
        /* done: the window shows the clone; stopped: as Cancel was */
        if (clone || (task && task->state() == Task::State::Stopped)) {
            forceClose();
            return;
        }
        gtk_label_set_text(GTK_LABEL(m_error),
                           utf8(QString("The VM could not be cloned: %1").arg(error)).constData());
        gtk_widget_set_visible(m_error, true);
    });
}

void CloneDialog::update()
{
    const bool cloning = m_cloner && m_cloner->isRunning();

    gtk_widget_set_sensitive(m_name, !cloning);
    gtk_widget_set_visible(m_ok, !cloning);
    gtk_widget_set_sensitive(m_ok, !name().isEmpty());
    gtk_widget_set_visible(m_cancel, !cloning);
    gtk_widget_set_visible(m_stop, cloning);
    gtk_widget_set_visible(m_close, cloning);
    gtk_widget_set_visible(m_progress, cloning);
    gtk_widget_set_visible(m_background, cloning);
    adw_dialog_set_default_widget(dialog(), cloning ? m_close : m_ok);
}
