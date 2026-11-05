// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/logdialog.h"

#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include "gtk/widgets.h"

using Gtk4::utf8;

static const int kPollMs = 500;
/* the end of a long log: the runs before the last are rarely read */
static const qint64 kTailBytes = 1 << 20;
static const int kMaxLines = 20000;

LogDialog::LogDialog(const QString &title, const QString &path, Presenter presenter)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_path(path), m_timer(new QTimer(qt())),
      m_buffer(gtk_text_buffer_new(nullptr)), m_view(gtk_text_view_new_with_buffer(m_buffer)),
      m_scroll(gtk_scrolled_window_new()), m_stack(gtk_stack_new()),
      m_clear(gtk_button_new_from_icon_name("edit-clear-all-symbolic")),
      m_open(gtk_button_new_from_icon_name("document-open-symbolic"))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *empty = adw_status_page_new();

    adw_dialog_set_title(dialog(), utf8(title + " Log").constData());
    adw_dialog_set_content_width(dialog(), 900);
    adw_dialog_set_content_height(dialog(), 600);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    adw_window_title_new("Log", utf8(title).constData()));
    gtk_widget_set_tooltip_text(m_clear, "Empty the Log File");
    gtk_widget_set_tooltip_text(m_open, "Open in the Text Editor");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_clear);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_open);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    gtk_widget_set_name(m_view, "log");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(m_view), false);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(m_view), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(m_view), true);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(m_view), GTK_WRAP_NONE);
    for (const auto margin : {&gtk_text_view_set_left_margin, &gtk_text_view_set_right_margin,
                              &gtk_text_view_set_top_margin, &gtk_text_view_set_bottom_margin}) {
        margin(GTK_TEXT_VIEW(m_view), 12);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(m_scroll), m_view);
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "text-x-generic-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Log Yet");
    adw_status_page_set_description(ADW_STATUS_PAGE(empty),
                                    "QEMU writes here once the VM starts.");
    gtk_stack_add_named(GTK_STACK(m_stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(m_stack), m_scroll, "log");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), m_stack);
    adw_dialog_set_child(dialog(), view);

    m_timer->setInterval(kPollMs);
    QObject::connect(m_timer, &QTimer::timeout, qt(), [this]() { poll(); });
    on<void()>(m_clear, "clicked", [this]() { clear(); });
    on<void()>(m_open, "clicked", [this]() { Gtk4::openPath(m_view, m_path); });
    reload();
    m_timer->start();
}

LogDialog::~LogDialog()
{
    stopCallbacks();
    g_object_unref(m_buffer);
}

QString LogDialog::text() const
{
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(m_buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(m_buffer, &start, &end, true);
    const QString result = QString::fromUtf8(text);
    g_free(text);
    return result;
}

void LogDialog::update()
{
    const bool something = m_lines > 0 || !m_partial.isEmpty();

    gtk_stack_set_visible_child_name(GTK_STACK(m_stack), something ? "log" : "empty");
    gtk_widget_set_sensitive(m_clear, QFileInfo::exists(m_path));
    gtk_widget_set_sensitive(m_open, QFileInfo::exists(m_path));
}

void LogDialog::clear()
{
    QFile f(m_path);

    /* QEMU goes on at the new end: it appends */
    if (f.open(QIODevice::ReadWrite)) {
        f.resize(0);
    }
    reload();
}

void LogDialog::reload()
{
    QFile f(m_path);

    gtk_text_buffer_set_text(m_buffer, "", -1);
    m_offset = 0;
    m_lines = 0;
    m_partial.clear();
    if (m_path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        update();
        return;
    }
    /* from a line start */
    if (f.size() > kTailBytes) {
        f.seek(f.size() - kTailBytes);
        f.readLine();
    }
    m_offset = f.pos();
    append(f.readAll());
    m_offset = f.pos();
    /* at the end, as it opens */
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(m_buffer, &end);
    gtk_text_buffer_place_cursor(m_buffer, &end);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(m_view), gtk_text_buffer_get_insert(m_buffer), 0,
                                 false, 0, 0);
    update();
}

void LogDialog::poll()
{
    const QFileInfo info(m_path);
    QFile f(m_path);

    if (!info.exists() || info.size() < m_offset) {
        /* gone, emptied or started over */
        if (m_offset > 0 || m_lines > 0 || info.exists()) {
            reload();
        }
        return;
    }
    if (info.size() == m_offset || !f.open(QIODevice::ReadOnly) || !f.seek(m_offset)) {
        return;
    }
    append(f.readAll());
    m_offset = f.pos();
    update();
}

bool LogDialog::atEnd() const
{
    GtkAdjustment *scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(m_scroll));
    return gtk_adjustment_get_value(scroll) + gtk_adjustment_get_page_size(scroll) >=
           gtk_adjustment_get_upper(scroll) - 1;
}

/* the complete lines of @bytes, and what came before them */
void LogDialog::append(const QByteArray &bytes)
{
    const bool follow = atEnd();
    QByteArray text = m_partial + bytes;
    const qsizetype end = text.lastIndexOf('\n');
    GtkTextIter iter;

    if (end < 0) {
        m_partial = text;
        return;
    }
    m_partial = text.mid(end + 1);
    text.truncate(end + 1);
    gtk_text_buffer_get_end_iter(m_buffer, &iter);
    /* GTK takes valid UTF-8 only: what QEMU wrote, its bad bytes as U+FFFD */
    gtk_text_buffer_insert(m_buffer, &iter, QString::fromUtf8(text).toUtf8().constData(), -1);
    m_lines += int(text.count('\n'));
    /* the oldest lines go past the most kept */
    if (m_lines > kMaxLines) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(m_buffer, &start);
        gtk_text_buffer_get_iter_at_line(m_buffer, &cut, m_lines - kMaxLines);
        gtk_text_buffer_delete(m_buffer, &start, &cut);
        m_lines = kMaxLines;
    }
    if (follow) {
        gtk_text_buffer_get_end_iter(m_buffer, &iter);
        gtk_text_buffer_place_cursor(m_buffer, &iter);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(m_view), gtk_text_buffer_get_insert(m_buffer), 0,
                                     false, 0, 0);
    }
}
