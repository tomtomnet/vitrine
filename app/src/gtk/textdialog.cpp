// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/textdialog.h"

#include "gtk/widgets.h"

using Gtk4::utf8;

TextDialog::TextDialog(const QString &title, const QString &subtitle, const QString &text,
                       Presenter presenter)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_text(text),
      m_toasts(ADW_TOAST_OVERLAY(adw_toast_overlay_new()))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *textView = gtk_text_view_new();
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView));

    adw_dialog_set_title(dialog(), utf8(subtitle.isEmpty() ? title : subtitle + " — " + title)
                                       .constData());
    adw_dialog_set_content_width(dialog(), 900);
    adw_dialog_set_content_height(dialog(), 560);
    adw_header_bar_set_title_widget(
        ADW_HEADER_BAR(header),
        adw_window_title_new(utf8(title).constData(), utf8(subtitle).constData()));
    gtk_widget_set_tooltip_text(copy, "Copy All");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), copy);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    gtk_text_buffer_set_text(buffer, utf8(text).constData(), -1);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textView), false);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textView), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(textView), true);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textView), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textView), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(textView), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(textView), 12);
    gtk_widget_set_name(textView, "text");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), textView);
    gtk_widget_set_vexpand(scroll, true);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);

    adw_toast_overlay_set_child(m_toasts, view);
    adw_dialog_set_child(dialog(), GTK_WIDGET(m_toasts));

    on<void()>(copy, "clicked", [this]() { this->copy(); });
}

void TextDialog::copy()
{
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(dialog())), utf8(m_text).constData());
    adw_toast_overlay_add_toast(m_toasts, adw_toast_new("Copied"));
}
