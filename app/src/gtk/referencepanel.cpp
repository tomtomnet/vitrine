// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/referencepanel.h"

#include <QRegularExpression>
#include <QTimer>

#include <memory>

#include "core/qemuinfo.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;
using namespace QemuReference;

/* What a row of the results is: a group's title, or one of its entries */
static bool isHeader(GtkListBoxRow *row)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "header"));
}

static Kind kindOf(GtkListBoxRow *row)
{
    return Kind(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind")));
}

static QString nameOf(GtkListBoxRow *row)
{
    return QString::fromUtf8(static_cast<const char *>(g_object_get_data(G_OBJECT(row), "name")));
}

ReferencePanel::ReferencePanel()
    : m_timer(new QTimer(&m_qt)), m_root(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6)),
      m_search(gtk_search_entry_new()), m_status(Gtk4::caption({})),
      m_results(gtk_list_box_new()), m_doc(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)),
      m_use(gtk_button_new_with_mnemonic("_Insert"))
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget *results = gtk_scrolled_window_new();
    GtkWidget *doc = gtk_scrolled_window_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    g_object_ref_sink(m_root);
    gtk_widget_set_name(m_root, "reference");
    gtk_widget_set_name(m_search, "search");
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(m_search),
                                          "Search options, devices, properties…");
    gtk_box_append(GTK_BOX(m_root), m_search);
    gtk_widget_set_visible(m_status, false);
    gtk_box_append(GTK_BOX(m_root), m_status);

    gtk_widget_set_name(m_results, "results");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_results), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(m_results, "navigation-sidebar");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results), m_results);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(results), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(results, "card");
    gtk_widget_set_overflow(results, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(results, -1, 120);

    for (const auto side : {&gtk_widget_set_margin_start, &gtk_widget_set_margin_end,
                            &gtk_widget_set_margin_top, &gtk_widget_set_margin_bottom}) {
        side(m_doc, 12);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(doc), m_doc);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(doc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(doc, "card");
    gtk_widget_set_overflow(doc, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(doc, -1, 120);

    gtk_paned_set_start_child(GTK_PANED(paned), results);
    gtk_paned_set_end_child(GTK_PANED(paned), doc);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), false);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), false);
    gtk_paned_set_position(GTK_PANED(paned), 240);
    gtk_widget_set_vexpand(paned, true);
    gtk_box_append(GTK_BOX(m_root), paned);

    gtk_widget_set_tooltip_text(m_use, "Add a line for this entry to the arguments");
    gtk_widget_set_sensitive(m_use, false);
    gtk_widget_set_hexpand(buttons, true);
    gtk_widget_set_halign(m_use, GTK_ALIGN_END);
    gtk_widget_set_hexpand(m_use, true);
    gtk_box_append(GTK_BOX(buttons), m_use);
    gtk_box_append(GTK_BOX(m_root), buttons);

    m_timer->setSingleShot(true);
    m_timer->setInterval(150);
    QObject::connect(m_timer, &QTimer::timeout, &m_qt, [this]() { refresh(); });
    on<void()>(m_search, "search-changed", [this]() { m_timer->start(); });
    /* Enter: to the first entry found */
    on<void()>(m_search, "activate", [this]() {
        refresh();
        for (GtkWidget *row = gtk_widget_get_first_child(m_results); row;
             row = gtk_widget_get_next_sibling(row)) {
            if (GTK_IS_LIST_BOX_ROW(row) && !isHeader(GTK_LIST_BOX_ROW(row))) {
                gtk_list_box_select_row(GTK_LIST_BOX(m_results), GTK_LIST_BOX_ROW(row));
                gtk_widget_grab_focus(row);
                break;
            }
        }
    });
    on<void(GtkListBoxRow *)>(m_results, "row-selected", [this](GtkListBoxRow *) {
        if (m_filling) {
            return;
        }
        /* the click that selects an entry activates it too: not an Insert */
        m_justSelected = true;
        auto *clear = new std::function<void()>(guarded<void()>([this]() { m_justSelected = false; }));
        g_idle_add_once(
            [](gpointer data) {
                std::unique_ptr<std::function<void()>> clear(static_cast<std::function<void()> *>(data));
                (*clear)();
            },
            clear);
        showCurrent();
    });
    on<void(GtkListBoxRow *)>(m_results, "row-activated", [this](GtkListBoxRow *row) {
        if (!isHeader(row)) {
            /* a second click, or Enter: the Qt panel's double click */
            if (!m_justSelected) {
                use();
            }
            return;
        }
        /* a group's title: open or closed, without a search */
        const int kind = kindOf(row);
        if (m_opened.contains(kind)) {
            m_opened.remove(kind);
        } else {
            m_opened.insert(kind);
        }
        refresh();
    });
    on<void()>(m_use, "clicked", [this]() { use(); });
    setDocs(QemuDocs::preferred());
}

ReferencePanel::~ReferencePanel()
{
    stopCallbacks();
    g_object_unref(m_root);
}

void ReferencePanel::setDocs(QemuDocs *docs)
{
    if (docs == m_docs) {
        return;
    }
    if (m_docs) {
        QObject::disconnect(m_docs, nullptr, &m_qt, nullptr);
    }
    m_docs = docs;
    QObject::connect(docs, &QemuDocs::changed, &m_qt, [this]() { refresh(); });
    QObject::connect(docs, &QemuDocs::propertiesLoaded, &m_qt, [this](const QString &device) {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(m_results));
        if (row && kindOf(row) == Device && nameOf(row) == device) {
            showCurrent();
        }
    });
    refresh();
}

void ReferencePanel::setSearchText(const QString &text)
{
    Gtk4::setText(m_search, text);
    refresh();
}

void ReferencePanel::refresh()
{
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;
    GtkListBoxRow *current = gtk_list_box_get_selected_row(GTK_LIST_BOX(m_results));
    const QString currentName = current ? nameOf(current) : QString();
    const int currentKind = current ? int(kindOf(current)) : -1;
    const QString query = Gtk4::text(m_search).trimmed();
    GtkListBoxRow *restore = nullptr;

    m_timer->stop();
    m_filling = true;
    while (GtkWidget *row = gtk_widget_get_first_child(m_results)) {
        gtk_list_box_remove(GTK_LIST_BOX(m_results), row);
    }
    if (!info) {
        gtk_label_set_text(GTK_LABEL(m_status), utf8(m_docs ? m_docs->status() : QString()).constData());
        gtk_widget_set_visible(m_status, true);
        m_filling = false;
        showCurrent();
        return;
    }
    gtk_widget_set_visible(m_status, false);

    for (const Group &group : search(*info, query)) {
        const bool open = !query.isEmpty() || group.kind == Option || m_opened.contains(group.kind);
        GtkWidget *header = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *title = gtk_label_new(nullptr);

        gtk_label_set_markup(GTK_LABEL(title),
                             utf8(QString("<b>%1</b> (%2)").arg(group.title).arg(group.total)).constData());
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_widget_set_hexpand(title, true);
        gtk_box_append(GTK_BOX(box), title);
        if (query.isEmpty()) {
            gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(open ? "pan-down-symbolic"
                                                                           : "pan-end-symbolic"));
        }
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(header), box);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(header), false);
        g_object_set_data(G_OBJECT(header), "header", GINT_TO_POINTER(1));
        g_object_set_data(G_OBJECT(header), "kind", GINT_TO_POINTER(int(group.kind)));
        gtk_list_box_append(GTK_LIST_BOX(m_results), header);
        if (!open) {
            continue;
        }
        for (const Hit &hit : group.hits) {
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            GtkWidget *name = gtk_label_new(utf8(hit.label).constData());
            GtkWidget *desc = gtk_label_new(utf8(hit.desc.trimmed()).constData());

            gtk_label_set_xalign(GTK_LABEL(name), 0);
            gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(name, "monospace");
            gtk_label_set_xalign(GTK_LABEL(desc), 0);
            gtk_label_set_ellipsize(GTK_LABEL(desc), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(desc, "dim-label");
            gtk_widget_add_css_class(desc, "caption");
            gtk_widget_set_margin_start(texts, 12);
            gtk_box_append(GTK_BOX(texts), name);
            if (!hit.desc.isEmpty()) {
                gtk_box_append(GTK_BOX(texts), desc);
            } else {
                g_object_ref_sink(desc);
                g_object_unref(desc);
            }
            gtk_widget_set_tooltip_text(row, utf8(hit.desc).constData());
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), texts);
            g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(int(hit.kind)));
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(utf8(hit.name).constData()), g_free);
            gtk_list_box_append(GTK_LIST_BOX(m_results), row);
            if (int(hit.kind) == currentKind && hit.name == currentName) {
                restore = GTK_LIST_BOX_ROW(row);
            }
        }
    }
    if (restore) {
        gtk_list_box_select_row(GTK_LIST_BOX(m_results), restore);
    }
    m_filling = false;
    showCurrent();
}

void ReferencePanel::clearDoc()
{
    while (GtkWidget *child = gtk_widget_get_first_child(m_doc)) {
        gtk_box_remove(GTK_BOX(m_doc), child);
    }
}

void ReferencePanel::addBlock(const Block &block)
{
    GtkWidget *label = gtk_label_new(nullptr);
    const QByteArray markup = utf8(block.markup);

    /* markup of QEMU's documentation that Pango would refuse: its text alone */
    if (pango_parse_markup(markup.constData(), -1, 0, nullptr, nullptr, nullptr, nullptr)) {
        gtk_label_set_markup(GTK_LABEL(label), markup.constData());
    } else {
        QString text = block.markup;
        static const QRegularExpression tags("<[^>]*>");
        text.remove(tags);
        gtk_label_set_text(GTK_LABEL(label), utf8(text).constData());
    }
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(label), true);
    gtk_widget_set_margin_start(label, block.indent * 4);
    switch (block.type) {
    case Block::Heading:
        gtk_widget_add_css_class(label, "title-4");
        break;
    case Block::Code:
        gtk_widget_add_css_class(label, "monospace");
        break;
    case Block::Text:
        break;
    }
    gtk_box_append(GTK_BOX(m_doc), label);
}

void ReferencePanel::showCurrent()
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(m_results));
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;

    gtk_widget_set_sensitive(m_use, row && info);
    clearDoc();
    if (!info) {
        return;
    }
    if (!row) {
        addBlock({Block::Text, QString("The documentation of <b>%1</b>, QEMU %2.")
                                   .arg(m_docs->binary().toHtmlEscaped(), info->version.toHtmlEscaped())});
        addBlock({Block::Text, "Search it, pick an entry and see its description here."});
        return;
    }
    bool loading = false;
    for (const Block &block : describe(*info, kindOf(row), nameOf(row), &loading)) {
        addBlock(block);
    }
    if (loading) {
        m_docs->loadProperties(nameOf(row));
    }
}

void ReferencePanel::use()
{
    if (!chosenLine().isEmpty() && m_insert) {
        m_insert(chosenLine());
    }
}

QString ReferencePanel::chosenLine() const
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(m_results));
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;

    return row && info ? lineFor(info, kindOf(row), nameOf(row)) : QString();
}

QStringList ReferencePanel::groups() const
{
    QStringList list;

    for (GtkWidget *row = gtk_widget_get_first_child(m_results); row;
         row = gtk_widget_get_next_sibling(row)) {
        if (GTK_IS_LIST_BOX_ROW(row) && isHeader(GTK_LIST_BOX_ROW(row))) {
            GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
            list << QString::fromUtf8(gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(box))));
        }
    }
    return list;
}

bool ReferencePanel::choose(Kind kind, const QString &name)
{
    for (GtkWidget *row = gtk_widget_get_first_child(m_results); row;
         row = gtk_widget_get_next_sibling(row)) {
        if (GTK_IS_LIST_BOX_ROW(row) && !isHeader(GTK_LIST_BOX_ROW(row)) &&
            kindOf(GTK_LIST_BOX_ROW(row)) == kind && nameOf(GTK_LIST_BOX_ROW(row)) == name) {
            gtk_list_box_select_row(GTK_LIST_BOX(m_results), GTK_LIST_BOX_ROW(row));
            return true;
        }
    }
    return false;
}
