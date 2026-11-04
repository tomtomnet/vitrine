// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/argspage.h"

#include <QTimer>

#include "core/vmconfig.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/argscheck.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

ArgumentsPage::ArgumentsPage(const QString &vmDir)
    : SettingsPage(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12)), m_vmDir(vmDir),
      m_timer(new QTimer(qt())), m_buffer(gtk_text_buffer_new(nullptr)),
      m_view(gtk_text_view_new_with_buffer(m_buffer)), m_problemsScroll(gtk_scrolled_window_new()),
      m_problemsList(gtk_list_box_new())
{
    GtkWidget *box = widget();
    GtkWidget *note = Gtk4::caption("The QEMU command line of this VM, one option per line; lines "
                                    "starting with # are comments. The other pages edit these "
                                    "same lines.");
    GtkWidget *scroll = gtk_scrolled_window_new();

    for (GtkWidget *edge : {box}) {
        gtk_widget_set_margin_start(edge, 12);
        gtk_widget_set_margin_end(edge, 12);
        gtk_widget_set_margin_top(edge, 12);
        gtk_widget_set_margin_bottom(edge, 12);
    }
    gtk_box_append(GTK_BOX(box), note);

    /* the text, in the fixed font, never wrapped: one option per line */
    gtk_widget_set_name(m_view, "args");
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(m_view), true);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(m_view), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(m_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(m_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(m_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(m_view), 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m_view);
    gtk_widget_add_css_class(scroll, "card");
    gtk_widget_set_overflow(scroll, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_vexpand(scroll, true);
    gtk_box_append(GTK_BOX(box), scroll);

    /* what is wrong with it, a few lines high, a click on one going there */
    gtk_widget_set_name(m_problemsList, "problems");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_problemsList), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(m_problemsList, "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(m_problemsScroll), m_problemsList);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(m_problemsScroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(m_problemsScroll), 180);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(m_problemsScroll), true);
    gtk_widget_set_visible(m_problemsScroll, false);
    gtk_box_append(GTK_BOX(box), m_problemsScroll);

    m_timer->setSingleShot(true);
    m_timer->setInterval(300);
    QObject::connect(m_timer, &QTimer::timeout, qt(), [this]() { check(); });
    on<void()>(m_buffer, "changed", [this]() {
        if (!m_loading) {
            m_timer->start();
            changed();
        }
    });
    on<void(GtkListBoxRow *)>(m_problemsList, "row-activated", [this](GtkListBoxRow *row) {
        goToLine(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line")));
    });
    setDocs(QemuDocs::preferred());
}

ArgumentsPage::~ArgumentsPage()
{
    g_object_unref(m_buffer);
}

QString ArgumentsPage::text() const
{
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(m_buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(m_buffer, &start, &end, true);
    const QString result = QString::fromUtf8(text);
    g_free(text);
    return result;
}

void ArgumentsPage::setText(const QString &text)
{
    gtk_text_buffer_set_text(m_buffer, utf8(text).constData(), -1);
}

void ArgumentsPage::load(const ArgsFile &args)
{
    {
        const Loading loading(m_loading);
        m_loaded = args.toText();
        setText(m_loaded);
    }
    /* the documentation of the VM's QEMU, at once */
    check();
}

void ArgumentsPage::save(ArgsFile &args)
{
    const QString text = this->text();

    if (text != m_loaded) {
        args = ArgsFile::parse(text);
        m_loaded = text;
    }
}

bool ArgumentsPage::isModified() const
{
    return text() != m_loaded;
}

void ArgumentsPage::setDocs(QemuDocs *docs)
{
    if (docs == m_docs) {
        return;
    }
    if (m_docs) {
        QObject::disconnect(m_docs, nullptr, qt(), nullptr);
    }
    m_docs = docs;
    QObject::connect(docs, &QemuDocs::changed, qt(), [this]() { check(); });
}

void ArgumentsPage::check()
{
    const QString text = this->text();

    m_timer->stop();
    /* the QEMU of the #qemu line, else the preferred one */
    setDocs(QemuDocs::of(VmConfig::qemuBinary(ArgsFile::parse(text))));

    const QList<ArgsProblem> problems = checkArgs(text, m_docs->info(), m_vmDir);
    while (GtkWidget *row = gtk_widget_get_first_child(m_problemsList)) {
        gtk_list_box_remove(GTK_LIST_BOX(m_problemsList), row);
    }
    m_problems.clear();
    for (const ArgsProblem &p : problems) {
        const QString line = QString("Line %1: %2").arg(p.line + 1).arg(p.message);
        GtkWidget *row = Gtk4::actionRow(line);
        GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");

        gtk_widget_add_css_class(icon, "warning");
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
        g_object_set_data(G_OBJECT(row), "line", GINT_TO_POINTER(p.line));
        gtk_list_box_append(GTK_LIST_BOX(m_problemsList), row);
        m_problems << line;
    }
    gtk_widget_set_visible(m_problemsScroll, !problems.isEmpty());
}

QStringList ArgumentsPage::problems() const
{
    return m_problems;
}

void ArgumentsPage::goToLine(int line)
{
    GtkTextIter iter;

    if (!gtk_text_buffer_get_iter_at_line(m_buffer, &iter, line)) {
        return;
    }
    if (!gtk_text_iter_ends_line(&iter)) {
        gtk_text_iter_forward_to_line_end(&iter);
    }
    gtk_text_buffer_place_cursor(m_buffer, &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(m_view), &iter, 0.25, false, 0, 0);
    gtk_widget_grab_focus(m_view);
}
