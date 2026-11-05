// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/argspage.h"

#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <utility>

#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "gtk/referencepanel.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/argscheck.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

/* The tags of the text, as the Qt editor's formats */
static const char *const kTags[] = {"option", "unknown", "key", "comment", "directive", "invalid"};

static QString optionName(const QString &word)
{
    return word.mid(word.startsWith("--") ? 2 : 1);
}

static void setMargins(GtkWidget *widget, int margin)
{
    gtk_widget_set_margin_start(widget, margin);
    gtk_widget_set_margin_end(widget, margin);
    gtk_widget_set_margin_top(widget, margin);
    gtk_widget_set_margin_bottom(widget, margin);
}

ArgumentsPage::ArgumentsPage(const QString &vmDir)
    : SettingsPage(adw_overlay_split_view_new()), m_vmDir(vmDir), m_timer(new QTimer(qt())),
      m_reference(std::make_unique<ReferencePanel>()), m_buffer(gtk_text_buffer_new(nullptr)),
      m_view(gtk_text_view_new_with_buffer(m_buffer)), m_problemsScroll(gtk_scrolled_window_new()),
      m_problemsList(gtk_list_box_new()), m_split(ADW_OVERLAY_SPLIT_VIEW(widget())),
      m_completion(gtk_popover_new()), m_completionList(gtk_list_box_new())
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *note = Gtk4::caption("The QEMU command line of this VM, one option per line; lines "
                                    "starting with # are comments. The other pages edit these "
                                    "same lines. Ctrl+Space completes option and device names.");
    GtkWidget *toggle = gtk_toggle_button_new();
    GtkWidget *scroll = gtk_scrolled_window_new();

    /* the reference beside the text, at the end; over it in a narrow window */
    adw_overlay_split_view_set_sidebar_position(m_split, GTK_PACK_END);
    adw_overlay_split_view_set_min_sidebar_width(m_split, 260);
    adw_overlay_split_view_set_max_sidebar_width(m_split, 420);
    adw_overlay_split_view_set_sidebar_width_fraction(m_split, 0.42);
    setMargins(m_reference->widget(), 12);
    gtk_widget_set_margin_start(m_reference->widget(), 0);
    adw_overlay_split_view_set_sidebar(m_split, m_reference->widget());
    adw_overlay_split_view_set_content(m_split, column);
    m_reference->setInsert(guarded<void(const QString &)>(
        [this](const QString &line) { insertLine(line); }));

    setMargins(column, 12);
    gtk_widget_set_hexpand(note, true);
    gtk_box_append(GTK_BOX(top), note);
    gtk_button_set_icon_name(GTK_BUTTON(toggle), "accessories-dictionary-symbolic");
    gtk_widget_set_tooltip_text(toggle, "QEMU Reference");
    gtk_widget_set_valign(toggle, GTK_ALIGN_START);
    g_object_bind_property(toggle, "active", m_split, "show-sidebar",
                           GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), true);
    gtk_box_append(GTK_BOX(top), toggle);
    gtk_box_append(GTK_BOX(column), top);

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
    gtk_box_append(GTK_BOX(column), scroll);
    for (const char *name : kTags) {
        gtk_text_buffer_create_tag(m_buffer, name, nullptr);
    }
    updateColors();

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
    gtk_box_append(GTK_BOX(column), m_problemsScroll);

    /* completion: a list at the cursor, which the keys go on to the text from */
    {
        GtkWidget *list = gtk_scrolled_window_new();
        m_completionScroll = list;
        /* the keys stay in the text: the list never takes them */
        gtk_widget_set_focusable(m_completionList, false);
        gtk_widget_set_can_focus(list, false);
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_completionList), GTK_SELECTION_BROWSE);
        gtk_widget_add_css_class(m_completionList, "navigation-sidebar");
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list), m_completionList);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list), GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(list), 280);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(list), true);
        gtk_widget_set_size_request(list, 420, -1);
        gtk_popover_set_child(GTK_POPOVER(m_completion), list);
        gtk_popover_set_autohide(GTK_POPOVER(m_completion), false);
        gtk_popover_set_has_arrow(GTK_POPOVER(m_completion), false);
        gtk_popover_set_position(GTK_POPOVER(m_completion), GTK_POS_BOTTOM);
        gtk_widget_set_halign(m_completion, GTK_ALIGN_START);
        gtk_widget_set_parent(m_completion, m_view);
        on<void(GtkListBoxRow *)>(m_completionList, "row-activated", [this](GtkListBoxRow *row) {
            insertCompletion(m_shown.value(gtk_list_box_row_get_index(row)));
        });
    }
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    on<gboolean(guint, guint, GdkModifierType)>(
        keys, "key-pressed", [this](guint keyval, guint, GdkModifierType state) -> gboolean {
            return completionKey(keyval, state);
        });
    gtk_widget_add_controller(m_view, keys);
    on<void(GParamSpec *)>(m_view, "notify::has-focus", [this](GParamSpec *) {
        if (!gtk_widget_has_focus(m_view)) {
            hideCompletion();
        }
    });

    m_timer->setSingleShot(true);
    m_timer->setInterval(300);
    QObject::connect(m_timer, &QTimer::timeout, qt(), [this]() { check(); });
    on<void()>(m_buffer, "changed", [this]() {
        highlight();
        if (m_loading) {
            return;
        }
        m_timer->start();
        if (std::exchange(m_typed, false) && !m_completing) {
            complete(false);
        }
        changed();
    });
    on<void(GtkListBoxRow *)>(m_problemsList, "row-activated", [this](GtkListBoxRow *row) {
        goToLine(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line")));
    });
    /* the colors of light and dark */
    AdwStyleManager *style = adw_style_manager_get_default();
    m_darkHandler = Gtk4::connect<void(GParamSpec *)>(
        style, "notify::dark", guarded<void(GParamSpec *)>([this](GParamSpec *) { updateColors(); }));
    m_accentHandler = Gtk4::connect<void(GParamSpec *)>(
        style, "notify::accent-color-rgba",
        guarded<void(GParamSpec *)>([this](GParamSpec *) { updateColors(); }));
    setDocs(QemuDocs::preferred());
}

ArgumentsPage::~ArgumentsPage()
{
    AdwStyleManager *style = adw_style_manager_get_default();
    g_signal_handler_disconnect(style, m_darkHandler);
    g_signal_handler_disconnect(style, m_accentHandler);
    /* a child of the text, which does not know it */
    gtk_widget_unparent(m_completion);
    g_object_unref(m_buffer);
}

void ArgumentsPage::addNarrowSetters(AdwBreakpoint *narrow)
{
    adw_breakpoint_add_setters(narrow, G_OBJECT(m_split), "collapsed", true, "show-sidebar", false,
                               nullptr);
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
    m_context = Context::None;
    m_reference->setDocs(docs);
    QObject::connect(docs, &QemuDocs::changed, qt(), [this]() {
        m_context = Context::None;
        check();
    });
}

void ArgumentsPage::check()
{
    const QString text = this->text();

    m_timer->stop();
    /* the QEMU of the #qemu line, else the preferred one */
    setDocs(QemuDocs::of(VmConfig::qemuBinary(ArgsFile::parse(text))));
    highlight();

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

void ArgumentsPage::placeCursor(int line, int offset)
{
    GtkTextIter iter;

    gtk_text_buffer_get_iter_at_line_offset(m_buffer, &iter, line, offset);
    gtk_text_buffer_place_cursor(m_buffer, &iter);
}

void ArgumentsPage::insertLine(const QString &line)
{
    GtkTextIter cursor, start, end;
    GtkTextMark *insert = gtk_text_buffer_get_insert(m_buffer);

    gtk_text_buffer_get_iter_at_mark(m_buffer, &cursor, insert);
    start = end = cursor;
    gtk_text_iter_set_line_offset(&start, 0);
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }
    char *current = gtk_text_buffer_get_text(m_buffer, &start, &end, true);
    const bool blank = QString::fromUtf8(current).trimmed().isEmpty();
    g_free(current);

    gtk_text_buffer_begin_user_action(m_buffer);
    if (blank) {
        gtk_text_buffer_delete(m_buffer, &start, &end);
        gtk_text_buffer_insert(m_buffer, &start, utf8(line).constData(), -1);
    } else {
        gtk_text_buffer_insert(m_buffer, &end, utf8("\n" + line).constData(), -1);
    }
    gtk_text_buffer_end_user_action(m_buffer);
    gtk_text_buffer_get_iter_at_mark(m_buffer, &cursor, insert);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(m_view), insert, 0.1, false, 0, 0);
    gtk_widget_grab_focus(m_view);
}

/* Highlighting: the Qt editor's ArgsHighlighter, on the text's tags */

void ArgumentsPage::updateColors()
{
    AdwStyleManager *style = adw_style_manager_get_default();
    const bool dark = adw_style_manager_get_dark(style);
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(m_buffer);
    const GdkRGBA dim = dark ? GdkRGBA{1, 1, 1, 0.55f} : GdkRGBA{0, 0, 0, 0.55f};
    const GdkRGBA dimmer = dark ? GdkRGBA{1, 1, 1, 0.45f} : GdkRGBA{0, 0, 0, 0.45f};
    const GdkRGBA red = dark ? GdkRGBA{1.0f, 0.42f, 0.42f, 1} : GdkRGBA{0.85f, 0.27f, 0.33f, 1};
    const GdkRGBA teal = dark ? GdkRGBA{0.30f, 0.82f, 0.69f, 1} : GdkRGBA{0, 0.48f, 0.37f, 1};
    /* the accent as text: the standalone shade, readable on the view's background */
    GdkRGBA option;
    adw_accent_color_to_standalone_rgba(adw_style_manager_get_accent_color(style), dark, &option);

    g_object_set(gtk_text_tag_table_lookup(table, "option"), "weight", PANGO_WEIGHT_BOLD,
                 "foreground-rgba", &option, nullptr);
    g_object_set(gtk_text_tag_table_lookup(table, "unknown"), "weight", PANGO_WEIGHT_BOLD,
                 "foreground-rgba", &option, "underline", PANGO_UNDERLINE_ERROR, "underline-rgba",
                 &red, nullptr);
    g_object_set(gtk_text_tag_table_lookup(table, "key"), "foreground-rgba", &dim, nullptr);
    g_object_set(gtk_text_tag_table_lookup(table, "comment"), "foreground-rgba", &dimmer, "style",
                 PANGO_STYLE_ITALIC, nullptr);
    g_object_set(gtk_text_tag_table_lookup(table, "directive"), "weight", PANGO_WEIGHT_BOLD,
                 "foreground-rgba", &teal, nullptr);
    g_object_set(gtk_text_tag_table_lookup(table, "invalid"), "underline", PANGO_UNDERLINE_ERROR,
                 "underline-rgba", &red, nullptr);
}

void ArgumentsPage::tag(int line, int from, int length, const char *name)
{
    GtkTextIter start, end;

    gtk_text_buffer_get_iter_at_line_offset(m_buffer, &start, line, from);
    gtk_text_buffer_get_iter_at_line_offset(m_buffer, &end, line, from + length);
    gtk_text_buffer_apply_tag_by_name(m_buffer, name, &start, &end);
}

void ArgumentsPage::highlightKeys(int line, const QString &text, int from)
{
    static const QRegularExpression key("(?:^|,)([A-Za-z0-9_.-]+=)");

    for (auto it = key.globalMatch(text, from); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        tag(line, int(m.capturedStart(1)), int(m.capturedLength(1)), "key");
    }
}

void ArgumentsPage::highlight()
{
    GtkTextIter start, end;
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;
    const QStringList lines = text().split('\n');

    gtk_text_buffer_get_bounds(m_buffer, &start, &end);
    for (const char *name : kTags) {
        gtk_text_buffer_remove_tag_by_name(m_buffer, name, &start, &end);
    }
    for (int n = 0; n < lines.size(); n++) {
        highlightLine(n, lines[n], info);
    }
}

void ArgumentsPage::highlightLine(int line, const QString &text, const QemuInfo *info)
{
    int start = 0;

    while (start < text.size() && text[start].isSpace()) {
        start++;
    }
    if (start == text.size()) {
        return;
    }
    if (text[start] == '#') {
        const QStringView rest = QStringView(text).mid(start);
        auto isDirective = [&](QStringView word) {
            return rest.startsWith(word) && (rest.size() == word.size() || rest[word.size()].isSpace());
        };
        if (isDirective(u"#share")) {
            tag(line, start, 6, "directive");
            highlightKeys(line, text, start + 7);
        } else if (isDirective(u"#qemu")) {
            tag(line, start, 5, "directive");
        } else if (isDirective(u"#env")) {
            tag(line, start, 4, "directive");
        } else if (isDirective(u"#guest")) {
            tag(line, start, 6, "directive");
            highlightKeys(line, text, start + 7);
        } else {
            tag(line, start, int(text.size()) - start, "comment");
        }
        return;
    }
    if (text[start] != '-') {
        tag(line, start, int(text.size()) - start, "invalid");
        return;
    }

    int end = start;
    while (end < text.size() && !text[end].isSpace()) {
        end++;
    }
    const QString name = optionName(text.mid(start, end - start));
    const bool unknown = info && !info->options.isEmpty() && !info->option(name);

    tag(line, start, end - start, unknown ? "unknown" : "option");
    highlightKeys(line, text, end);

    if (name == "device" && info && !info->devices.isEmpty()) {
        /* the driver */
        int from = end;
        while (from < text.size() && text[from].isSpace()) {
            from++;
        }
        int to = from;
        while (to < text.size() && text[to] != ',' && !text[to].isSpace()) {
            to++;
        }
        const QString driver = text.mid(from, to - from);
        if (!driver.isEmpty() && !driver.contains('=') && driver != "help" && !info->device(driver)) {
            tag(line, from, to - from, "invalid");
        }
    }
}

QStringList ArgumentsPage::tagsAt(int line, int offset) const
{
    GtkTextIter iter;
    QStringList names;

    gtk_text_buffer_get_iter_at_line_offset(m_buffer, &iter, line, offset);
    GSList *tags = gtk_text_iter_get_tags(&iter);
    for (GSList *t = tags; t; t = t->next) {
        char *name = nullptr;
        g_object_get(t->data, "name", &name, nullptr);
        names << QString::fromUtf8(name);
        g_free(name);
    }
    g_slist_free(tags);
    return names;
}

/* Completion: the Qt editor's, in a popover at the cursor */

void ArgumentsPage::fillCompletions(Context context)
{
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;
    QList<std::pair<QString, QString>> rows;

    if (context == m_context) {
        return;
    }
    m_context = context;
    m_names.clear();
    if (!info) {
        return;
    }
    if (context == Context::Option) {
        for (const QemuOptionDoc &o : info->options) {
            rows << std::pair(o.name, o.help.section('\n', 0, 0));
        }
    } else if (context == Context::Device) {
        for (const QemuDeviceDoc &d : info->devices) {
            if (d.userCreatable) {
                rows << std::pair(d.name, d.desc.isEmpty() ? d.category : d.desc);
                for (const QString &alias : d.aliases) {
                    rows << std::pair(alias, QString("alias of %1").arg(d.name));
                }
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    QString last;
    for (const auto &[name, desc] : rows) {
        if (name != last) {
            m_names << std::pair(name, desc);
        }
        last = name;
    }
}

void ArgumentsPage::complete(bool force)
{
    static const QRegularExpression deviceRe("^\\s*-{1,2}device\\s+([\\w.-]*)$");
    static const QRegularExpression optionRe("^\\s*-{1,2}([\\w.-]*)$");
    GtkTextIter cursor, start;
    QRegularExpressionMatch m;

    if (!m_docs || !m_docs->info()) {
        hideCompletion();
        return;
    }
    gtk_text_buffer_get_iter_at_mark(m_buffer, &cursor, gtk_text_buffer_get_insert(m_buffer));
    start = cursor;
    gtk_text_iter_set_line_offset(&start, 0);
    char *before = gtk_text_buffer_get_text(m_buffer, &start, &cursor, true);
    const QString text = QString::fromUtf8(before);
    g_free(before);

    if ((m = deviceRe.match(text)).hasMatch()) {
        fillCompletions(Context::Device);
    } else if ((m = optionRe.match(text)).hasMatch()) {
        fillCompletions(Context::Option);
    } else {
        hideCompletion();
        return;
    }
    m_prefix = m.captured(1);
    m_shown.clear();
    while (GtkWidget *row = gtk_widget_get_first_child(m_completionList)) {
        gtk_list_box_remove(GTK_LIST_BOX(m_completionList), row);
    }
    for (const auto &[name, desc] : std::as_const(m_names)) {
        if (!name.startsWith(m_prefix, Qt::CaseInsensitive)) {
            continue;
        }
        /* enough to choose from: typing more narrows it */
        if (m_shown.size() == 200) {
            break;
        }
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *label = gtk_label_new(utf8(name).constData());
        GtkWidget *about = gtk_label_new(utf8(desc).constData());
        gtk_widget_add_css_class(label, "monospace");
        gtk_label_set_xalign(GTK_LABEL(about), 0);
        gtk_label_set_ellipsize(GTK_LABEL(about), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(about, true);
        gtk_widget_add_css_class(about, "dim-label");
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), about);
        gtk_list_box_append(GTK_LIST_BOX(m_completionList), row);
        gtk_widget_set_focusable(gtk_widget_get_parent(row), false);
        m_shown << name;
    }
    if (m_shown.isEmpty() || (m_shown.size() == 1 && m_shown.first() == m_prefix && !force)) {
        hideCompletion();
        return;
    }
    gtk_list_box_select_row(GTK_LIST_BOX(m_completionList),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(m_completionList), 0));

    /* at the cursor, under its line */
    GdkRectangle location;
    int x = 0, y = 0;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(m_view), &cursor, &location);
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(m_view), GTK_TEXT_WINDOW_WIDGET, location.x,
                                          location.y, &x, &y);
    const GdkRectangle at = {x, y, 1, location.height};
    gtk_popover_set_pointing_to(GTK_POPOVER(m_completion), &at);
    if (!gtk_widget_get_visible(m_completion)) {
        gtk_popover_popup(GTK_POPOVER(m_completion));
    }
}

void ArgumentsPage::hideCompletion()
{
    if (gtk_widget_get_visible(m_completion)) {
        gtk_popover_popdown(GTK_POPOVER(m_completion));
    }
}

QStringList ArgumentsPage::completions() const
{
    return gtk_widget_get_visible(m_completion) ? m_shown : QStringList();
}

void ArgumentsPage::insertCompletion(const QString &completion)
{
    GtkTextIter cursor, start;
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;

    if (completion.isEmpty()) {
        return;
    }
    hideCompletion();
    m_completing = true;
    gtk_text_buffer_get_iter_at_mark(m_buffer, &cursor, gtk_text_buffer_get_insert(m_buffer));
    start = cursor;
    gtk_text_iter_backward_chars(&start, int(m_prefix.size()));
    gtk_text_buffer_begin_user_action(m_buffer);
    gtk_text_buffer_delete(m_buffer, &start, &cursor);
    gtk_text_buffer_insert(m_buffer, &start, utf8(completion).constData(), -1);
    if (m_context == Context::Option && info) {
        const QemuOptionDoc *doc = info->option(completion);
        if (doc && doc->takesValue && gtk_text_iter_ends_line(&start)) {
            gtk_text_buffer_insert(m_buffer, &start, " ", -1);
        }
    }
    gtk_text_buffer_end_user_action(m_buffer);
    gtk_text_buffer_place_cursor(m_buffer, &start);
    m_completing = false;
    gtk_widget_grab_focus(m_view);
    if (m_context == Context::Option && completion == "device") {
        complete(true);
    }
}

/* The keys while the list shows, and those it follows; true when taken */
bool ArgumentsPage::completionKey(guint keyval, GdkModifierType state)
{
    const bool shown = gtk_widget_get_visible(m_completion);
    const bool control = state & GDK_CONTROL_MASK;

    if (control && keyval == GDK_KEY_space) {
        complete(true);
        return true;
    }
    if (shown) {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(m_completionList));
        const int index = row ? gtk_list_box_row_get_index(row) : -1;
        int to = index;

        switch (keyval) {
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_Tab:
            insertCompletion(m_shown.value(index));
            return true;
        case GDK_KEY_Escape:
            hideCompletion();
            return true;
        case GDK_KEY_Up:
        case GDK_KEY_KP_Up:
            to = qMax(0, index - 1);
            break;
        case GDK_KEY_Down:
        case GDK_KEY_KP_Down:
            to = qMin(int(m_shown.size()) - 1, index + 1);
            break;
        case GDK_KEY_Page_Up:
            to = qMax(0, index - 8);
            break;
        case GDK_KEY_Page_Down:
            to = qMin(int(m_shown.size()) - 1, index + 8);
            break;
        case GDK_KEY_BackSpace:
        case GDK_KEY_Delete:
            m_typed = true;
            return false;
        default:
            break;
        }
        if (to != index) {
            GtkListBoxRow *next = gtk_list_box_get_row_at_index(GTK_LIST_BOX(m_completionList), to);
            gtk_list_box_select_row(GTK_LIST_BOX(m_completionList), next);
            /* in sight in the list */
            const graphene_point_t origin = GRAPHENE_POINT_INIT(0, 0);
            graphene_point_t top;
            GtkAdjustment *scroll =
                gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(m_completionScroll));
            if (next && gtk_widget_compute_point(GTK_WIDGET(next), m_completionList, &origin, &top)) {
                const double height = gtk_widget_get_height(GTK_WIDGET(next));
                const double value = gtk_adjustment_get_value(scroll);
                const double page = gtk_adjustment_get_page_size(scroll);
                if (top.y < value) {
                    gtk_adjustment_set_value(scroll, top.y);
                } else if (top.y + height > value + page) {
                    gtk_adjustment_set_value(scroll, top.y + height - page);
                }
            }
            return true;
        }
    }
    /* a character typed: completion follows it, once in the text */
    const gunichar character = gdk_keyval_to_unicode(keyval);
    m_typed = character && g_unichar_isprint(character) &&
              !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK));
    return false;
}
