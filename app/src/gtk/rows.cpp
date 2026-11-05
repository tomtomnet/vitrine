// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/rows.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <memory>

#include "gtk/widgets.h"

namespace Gtk4 {

GtkWidget *caption(const QString &text)
{
    GtkWidget *widget = label(text);

    gtk_widget_add_css_class(widget, "dim-label");
    gtk_widget_add_css_class(widget, "caption");
    return widget;
}

AdwPreferencesGroup *group(const QString &title, const QString &description)
{
    auto *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    if (!title.isEmpty()) {
        adw_preferences_group_set_title(group, utf8(title.toHtmlEscaped()).constData());
    }
    if (!description.isEmpty()) {
        adw_preferences_group_set_description(group, utf8(description.toHtmlEscaped()).constData());
    }
    return group;
}

void setTitle(GtkWidget *row, const QString &text)
{
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), utf8(text.toHtmlEscaped()).constData());
}

void setSubtitle(GtkWidget *row, const QString &text)
{
    if (ADW_IS_EXPANDER_ROW(row)) {
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), utf8(text.toHtmlEscaped()).constData());
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), utf8(text.toHtmlEscaped()).constData());
    }
}

GtkWidget *entryRow(const QString &title, const QString &text)
{
    GtkWidget *row = adw_entry_row_new();

    setTitle(row, title);
    setText(row, text);
    return row;
}

GtkWidget *spinRow(const QString &title, double min, double max, double step,
                   const QString &subtitle)
{
    GtkWidget *row = adw_spin_row_new_with_range(min, max, step);

    setTitle(row, title);
    if (!subtitle.isEmpty()) {
        setSubtitle(row, subtitle);
    }
    return row;
}

GtkWidget *switchRow(const QString &title, const QString &subtitle)
{
    GtkWidget *row = adw_switch_row_new();

    setTitle(row, title);
    if (!subtitle.isEmpty()) {
        setSubtitle(row, subtitle);
    }
    return row;
}

GtkWidget *comboRow(const QString &title, const QStringList &items, const QString &subtitle)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(nullptr);

    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    g_object_unref(model);
    setTitle(row, title);
    setItems(row, items);
    if (!subtitle.isEmpty()) {
        setSubtitle(row, subtitle);
    }
    return row;
}

GtkWidget *actionRow(const QString &title, const QString &subtitle)
{
    GtkWidget *row = adw_action_row_new();

    setTitle(row, title);
    if (!subtitle.isEmpty()) {
        setSubtitle(row, subtitle);
    }
    return row;
}

GtkWidget *buttonRow(const QString &title, const char *icon)
{
    GtkWidget *row = adw_button_row_new();

    /* a mnemonic in it, as in a button */
    adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(row), true);
    setTitle(row, title);
    if (icon) {
        adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(row), icon);
    }
    return row;
}

GtkWidget *rowButton(const char *icon, const QString &tooltip)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon);

    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_tooltip_text(button, utf8(tooltip).constData());
    return button;
}

GtkWidget *rowTextButton(const QString &text, const QString &tooltip)
{
    GtkWidget *button = gtk_button_new_with_mnemonic(utf8(text).constData());

    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    if (!tooltip.isEmpty()) {
        gtk_widget_set_tooltip_text(button, utf8(tooltip).constData());
    }
    return button;
}

void setUnit(GtkWidget *spinRow, const QString &unit)
{
    /* GTK's spin buttons take digits only, by default: the unit too */
    adw_spin_row_set_numeric(ADW_SPIN_ROW(spinRow), false);
    connect<gboolean()>(spinRow, "output", [spinRow, unit]() -> gboolean {
        GtkAdjustment *adjustment = adw_spin_row_get_adjustment(ADW_SPIN_ROW(spinRow));
        const auto number = qint64(gtk_adjustment_get_value(adjustment));
        setText(spinRow, QString("%1 %2").arg(number).arg(unit));
        return true;
    });
    connect<gint(gdouble *)>(spinRow, "input", [spinRow](gdouble *value) -> gint {
        static const QRegularExpression number("^\\s*(\\d+)");
        const QRegularExpressionMatch m = number.match(text(spinRow));
        if (!m.hasMatch()) {
            return GTK_INPUT_ERROR;
        }
        *value = m.captured(1).toDouble();
        return true;
    });
}

int value(GtkWidget *spinRow)
{
    return int(adw_spin_row_get_value(ADW_SPIN_ROW(spinRow)));
}

void setValue(GtkWidget *spinRow, double value)
{
    adw_spin_row_set_value(ADW_SPIN_ROW(spinRow), value);
}

void setRange(GtkWidget *spinRow, double min, double max)
{
    adw_spin_row_set_range(ADW_SPIN_ROW(spinRow), min, max);
}

QString text(GtkWidget *editable)
{
    return QString::fromUtf8(gtk_editable_get_text(GTK_EDITABLE(editable)));
}

void setText(GtkWidget *editable, const QString &text)
{
    gtk_editable_set_text(GTK_EDITABLE(editable), utf8(text).constData());
}

void setItems(GtkWidget *comboRow, const QStringList &items)
{
    auto *model = GTK_STRING_LIST(adw_combo_row_get_model(ADW_COMBO_ROW(comboRow)));
    QList<QByteArray> bytes;
    QList<const char *> strings;

    for (const QString &item : items) {
        bytes << utf8(item);
    }
    for (const QByteArray &b : std::as_const(bytes)) {
        strings << b.constData();
    }
    strings << nullptr;
    gtk_string_list_splice(model, 0, g_list_model_get_n_items(G_LIST_MODEL(model)),
                           strings.constData());
}

int selected(GtkWidget *comboRow)
{
    const guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(comboRow));
    return index == GTK_INVALID_LIST_POSITION ? -1 : int(index);
}

void setSelected(GtkWidget *comboRow, int index)
{
    adw_combo_row_set_selected(ADW_COMBO_ROW(comboRow),
                               index < 0 ? GTK_INVALID_LIST_POSITION : guint(index));
}

QString itemText(GtkWidget *comboRow, int index)
{
    auto *model = GTK_STRING_LIST(adw_combo_row_get_model(ADW_COMBO_ROW(comboRow)));
    const char *text = index < 0 ? nullptr : gtk_string_list_get_string(model, guint(index));

    return text ? QString::fromUtf8(text) : QString();
}

void addSuggestions(GtkWidget *entryRow, const QString &tooltip,
                    std::function<QList<std::pair<QString, QString>>()> items)
{
    GtkWidget *button = gtk_menu_button_new();
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *search = gtk_search_entry_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *list = gtk_list_box_new();

    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-list-symbolic");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_tooltip_text(button, utf8(tooltip).constData());
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "navigation-sidebar");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_widget_set_size_request(scroll, 360, -1);
    gtk_box_append(GTK_BOX(box), search);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(entryRow), button);

    /* what the search finds: in the name or the description */
    gtk_list_box_set_filter_func(
        GTK_LIST_BOX(list),
        [](GtkListBoxRow *row, gpointer data) -> gboolean {
            const QString words = text(GTK_WIDGET(data)).trimmed();
            const auto *haystack = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "text"));
            return words.isEmpty() ||
                   QString::fromUtf8(haystack).contains(words, Qt::CaseInsensitive);
        },
        search, nullptr);
    connect<void()>(search, "search-changed",
                    [list]() { gtk_list_box_invalidate_filter(GTK_LIST_BOX(list)); });
    connect<void()>(popover, "show", [list, search, items]() {
        while (GtkWidget *row = gtk_widget_get_first_child(list)) {
            gtk_list_box_remove(GTK_LIST_BOX(list), row);
        }
        for (const auto &[name, description] : items()) {
            GtkWidget *row = actionRow(name, description);
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 2);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(utf8(name).constData()), g_free);
            g_object_set_data_full(G_OBJECT(row), "text",
                                   g_strdup(utf8(name + ' ' + description).constData()), g_free);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
        setText(search, {});
        gtk_widget_grab_focus(search);
    });
    connect<void(GtkListBoxRow *)>(list, "row-activated", [entryRow, popover](GtkListBoxRow *row) {
        setText(entryRow,
                QString::fromUtf8(static_cast<const char *>(g_object_get_data(G_OBJECT(row), "name"))));
        gtk_popover_popdown(GTK_POPOVER(popover));
    });
}

/* The window of @widget, for a dialog of its own */
static GtkWindow *windowOf(GtkWidget *widget)
{
    GtkRoot *root = widget ? gtk_widget_get_root(widget) : nullptr;
    return GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr;
}

using Chosen = std::function<void(const QString &path)>;

static GtkFileDialog *fileDialog(const QString &title, const QString &start, bool folder)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    const QFileInfo info(start);

    gtk_file_dialog_set_title(dialog, utf8(title).constData());
    gtk_file_dialog_set_modal(dialog, true);
    if (start.isEmpty()) {
        return dialog;
    }
    /* the file itself, else the folder it is in, if there */
    if (!folder && info.isFile()) {
        GFile *file = g_file_new_for_path(QFile::encodeName(info.absoluteFilePath()).constData());
        gtk_file_dialog_set_initial_file(dialog, file);
        g_object_unref(file);
    } else {
        const QString dir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
        if (QFileInfo(dir).isDir()) {
            GFile *file = g_file_new_for_path(QFile::encodeName(dir).constData());
            gtk_file_dialog_set_initial_folder(dialog, file);
            g_object_unref(file);
        }
    }
    return dialog;
}

static void finish(GFile *file, Chosen *chosen)
{
    if (file) {
        if (char *path = g_file_get_path(file)) {
            (*chosen)(QFile::decodeName(path));
            g_free(path);
        }
        g_object_unref(file);
    }
}

void chooseFile(GtkWidget *parent, const QString &title, const QList<FileFilter> &filters,
                const QString &start, Chosen chosen)
{
    GtkFileDialog *dialog = fileDialog(title, start, false);

    if (!filters.isEmpty()) {
        GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
        for (const FileFilter &f : filters) {
            GtkFileFilter *filter = gtk_file_filter_new();
            gtk_file_filter_set_name(filter, utf8(f.name).constData());
            for (const QString &pattern : f.patterns) {
                gtk_file_filter_add_pattern(filter, utf8(pattern).constData());
            }
            g_list_store_append(store, filter);
            g_object_unref(filter);
        }
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(store));
        g_object_unref(store);
    }
    gtk_file_dialog_open(
        dialog, windowOf(parent), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Chosen> chosen(static_cast<Chosen *>(data));
            GError *error = nullptr;
            finish(gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error),
                   chosen.get());
            g_clear_error(&error);
        },
        new Chosen(std::move(chosen)));
    /* the call keeps it until it answers */
    g_object_unref(dialog);
}

void chooseFolder(GtkWidget *parent, const QString &title, const QString &start, Chosen chosen)
{
    GtkFileDialog *dialog = fileDialog(title, start, true);

    gtk_file_dialog_select_folder(
        dialog, windowOf(parent), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Chosen> chosen(static_cast<Chosen *>(data));
            GError *error = nullptr;
            finish(gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error),
                   chosen.get());
            g_clear_error(&error);
        },
        new Chosen(std::move(chosen)));
    g_object_unref(dialog);
}

}
