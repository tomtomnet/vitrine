// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/statsbutton.h"
#include "gtk/vmconsole.h"
#include "gtkview/vmdisplay.h"

#include <QRegularExpression>
#include <QTimer>

#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/richtext.h"
#include "gtk/widgets.h"

using Gtk4::utf8;
using Sampler = VmStats::Sampler;

/* In widget names: stat-cpu, the Qt app's keys */
static const char *const kKeys[StatsText::kStats] = {"cpu", "memory", "disk", "network",
                                                     "gpu", "frames", "mainloop"};

/* A table's cell, as plain text: its entities decoded, the padding of no-break spaces gone */
static QString cellText(QString html)
{
    static const QRegularExpression tag("<[^>]*>");

    html.remove(tag);
    html.replace("&nbsp;", " ");
    html.replace("&lt;", "<");
    html.replace("&gt;", ">");
    html.replace("&quot;", "\"");
    html.replace("&#39;", "'");
    html.replace("&amp;", "&");
    return html.trimmed();
}

QList<StatsButton::Block> StatsButton::blocks(const QString &html)
{
    static const QRegularExpression table("<table>(.*?)</table>",
                                          QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression row("<tr>(.*?)</tr>", QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression cell("<td[^>]*>(.*?)</td>",
                                         QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression lineBreak("<br\\s*/?>");
    QList<Block> list;
    qsizetype from = 0;
    /* the text between tables: a line each, apart by <br> */
    const auto lines = [&list](const QString &part) {
        for (const QString &line : part.split(lineBreak)) {
            const QString markup = Gtk4::markup(line);
            if (!markup.isEmpty()) {
                list << Block{markup, {}};
            }
        }
    };

    for (auto it = table.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        Block block;

        lines(html.mid(from, m.capturedStart() - from));
        from = m.capturedEnd();
        for (auto r = row.globalMatch(m.captured(1)); r.hasNext();) {
            QStringList cells;
            for (auto c = cell.globalMatch(r.next().captured(1)); c.hasNext();) {
                cells << cellText(c.next().captured(1));
            }
            while (cells.size() < 3) {
                cells << QString();
            }
            block.rows << cells;
        }
        if (!block.rows.isEmpty()) {
            list << block;
        }
    }
    lines(html.mid(from));
    return list;
}

StatsButton::StatsButton(std::function<Vm *()> vm, std::function<VmConsole *()> console)
    : m_vmOf(std::move(vm)), m_consoleOf(std::move(console)), m_sampler(new Sampler(&m_qt)),
      m_refresh(new QTimer(&m_qt)), m_button(gtk_menu_button_new()), m_popover(gtk_popover_new()),
      m_stack(gtk_stack_new())
{
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *rows = gtk_list_box_new();
    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic");
    GtkWidget *scroll = gtk_scrolled_window_new();

    /* the header bar's: kept until this goes */
    g_object_ref_sink(m_button);
    gtk_widget_set_name(m_button, "statistics");
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_button), "power-profile-performance-symbolic");
    gtk_widget_set_tooltip_text(m_button, "Statistics");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(m_button), m_popover);

    /* the list: each statistic, its value; a row opens its details */
    m_title = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(m_title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(m_title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(m_title, "heading");
    gtk_widget_set_margin_start(m_title, 6);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(rows, "boxed-list");
    for (int i = 0; i < StatsText::kStats; i++) {
        const auto stat = StatsText::Stat(i);
        GtkWidget *row = adw_action_row_new();
        GtkWidget *go = gtk_image_new_from_icon_name("go-next-symbolic");

        gtk_widget_set_name(row, (QByteArray("stat-") + kKeys[i]).constData());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      utf8(StatsText::name(stat).toHtmlEscaped()).constData());
        gtk_widget_set_tooltip_text(row, utf8(StatsText::tip(stat)).constData());
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
        m_values[i] = gtk_label_new(nullptr);
        gtk_widget_add_css_class(m_values[i], "numeric");
        gtk_widget_add_css_class(m_values[i], "dim-label");
        gtk_label_set_ellipsize(GTK_LABEL(m_values[i]), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(m_values[i]), 30);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), m_values[i]);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), go);
        gtk_list_box_append(GTK_LIST_BOX(rows), row);
        m_rows[i] = row;
    }
    on<void(GtkListBoxRow *)>(rows, "row-activated", [this](GtkListBoxRow *row) {
        for (int i = 0; i < StatsText::kStats; i++) {
            if (GTK_WIDGET(row) == m_rows[i]) {
                showDetails(i);
            }
        }
    });
    gtk_box_append(GTK_BOX(list), m_title);
    gtk_box_append(GTK_BOX(list), rows);

    /* a statistic's details: the Qt app's tooltip, its tables as a grid */
    gtk_widget_add_css_class(back, "flat");
    gtk_widget_set_tooltip_text(back, "Back");
    on<void()>(back, "clicked", [this]() { showDetails(-1); });
    m_detailsTitle = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(m_detailsTitle), 0);
    gtk_widget_add_css_class(m_detailsTitle, "heading");
    gtk_box_append(GTK_BOX(header), back);
    gtk_box_append(GTK_BOX(header), m_detailsTitle);
    m_detailsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(m_detailsBox, 6);
    gtk_widget_set_margin_end(m_detailsBox, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m_detailsBox);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 480);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_box_append(GTK_BOX(details), header);
    gtk_box_append(GTK_BOX(details), scroll);

    gtk_stack_add_named(GTK_STACK(m_stack), list, "list");
    gtk_stack_add_named(GTK_STACK(m_stack), details, "details");
    gtk_stack_set_transition_type(GTK_STACK(m_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_vhomogeneous(GTK_STACK(m_stack), false);
    gtk_stack_set_interpolate_size(GTK_STACK(m_stack), true);
    gtk_widget_set_size_request(m_stack, 380, -1);
    gtk_widget_set_margin_start(m_stack, 6);
    gtk_widget_set_margin_end(m_stack, 6);
    gtk_widget_set_margin_top(m_stack, 6);
    gtk_widget_set_margin_bottom(m_stack, 6);
    gtk_popover_set_child(GTK_POPOVER(m_popover), m_stack);
    /* read while it shows, from the list again the next time */
    on<void(GParamSpec *)>(m_popover, "notify::visible", [this](GParamSpec *) {
        if (!gtk_widget_get_visible(m_popover)) {
            showDetails(-1);
        }
        updateSources();
        refresh();
    });

    /* the sampler's readers tell one by one: one refresh for those of a moment */
    m_refresh->setSingleShot(true);
    m_refresh->setInterval(0);
    QObject::connect(m_refresh, &QTimer::timeout, &m_qt, [this]() { refresh(); });
    QObject::connect(m_sampler, &Sampler::changed, m_refresh, qOverload<>(&QTimer::start));
    gtk_widget_set_visible(m_button, false);
}

StatsButton::~StatsButton()
{
    stopCallbacks();
    m_sampler->setVm(nullptr);
    /* out of the page's header bar, which outlives the window */
    if (GtkWidget *bar = gtk_widget_get_ancestor(m_button, ADW_TYPE_HEADER_BAR)) {
        adw_header_bar_remove(ADW_HEADER_BAR(bar), m_button);
    }
    g_object_unref(m_button);
}

void StatsButton::update()
{
    Vm *vm = m_vmOf ? m_vmOf() : nullptr;

    if (vm != m_vm) {
        m_vm = vm;
        m_sampler->setVm(vm);
        showDetails(-1);
    }
    const bool runs = vm && vm->runner()->isActive();
    gtk_widget_set_visible(m_button, runs);
    if (!runs) {
        gtk_popover_popdown(GTK_POPOVER(m_popover));
    }
    updateSources();
    refresh();
}

void StatsButton::showPopover(bool shown)
{
    if (shown) {
        gtk_menu_button_popup(GTK_MENU_BUTTON(m_button));
    } else {
        gtk_menu_button_popdown(GTK_MENU_BUTTON(m_button));
    }
}

void StatsButton::showDetails(int stat)
{
    if (stat == m_details) {
        return;
    }
    m_details = stat;
    m_detailsHtml.clear();
    gtk_stack_set_visible_child_name(GTK_STACK(m_stack), stat < 0 ? "list" : "details");
    if (stat >= 0) {
        gtk_label_set_text(GTK_LABEL(m_detailsTitle),
                           utf8(StatsText::name(StatsText::Stat(stat))).constData());
    }
    updateSources();
    refresh();
}

Sampler::Sources StatsButton::sources() const
{
    return m_sampler->sources();
}

QString StatsButton::value(StatsText::Stat stat) const
{
    return QString::fromUtf8(gtk_label_get_text(GTK_LABEL(m_values[stat])));
}

QString StatsButton::note(StatsText::Stat stat) const
{
    return QString::fromUtf8(adw_action_row_get_subtitle(ADW_ACTION_ROW(m_rows[stat])));
}

QString StatsButton::detailsText() const
{
    return m_detailsHtml;
}

/* Only while it shows: the list's numbers, and the details' extra ones */
void StatsButton::updateSources()
{
    Sampler::Sources sources;
    const VmConsole *console = m_consoleOf ? m_consoleOf() : nullptr;
    const bool screen = console && console->vm() == m_vm && console->display();

    if (!m_vm || !m_vm->runner()->isActive() || !gtk_widget_get_visible(m_popover)) {
        m_sampler->setSources({});
        return;
    }
    sources = Sampler::Cpu | Sampler::Memory | Sampler::Disk | Sampler::Network | Sampler::Gpu;
    /* the screen in vitrine's window measures its own frames; QEMU's SDL display, its */
    if (!screen) {
        sources |= Sampler::Display;
    }
    if (m_details == StatsText::Cpu || m_details == StatsText::MainLoop) {
        sources |= Sampler::Kvm;
    }
    if (m_details == StatsText::Memory) {
        sources |= Sampler::MemoryDetails;
    }
    m_sampler->setSources(sources);
}

VmStats::Snapshot StatsButton::snapshot() const
{
    VmStats::Snapshot s = m_sampler->snapshot();
    VmConsole *console = m_consoleOf ? m_consoleOf() : nullptr;
    VmDisplay *display = console && console->vm() == m_vm ? console->display() : nullptr;

    if (!display) {
        return s;
    }
    /* the monitor the window is on */
    double refreshHz = 0;
    if (GtkNative *native = gtk_widget_get_native(m_button)) {
        GdkSurface *surface = gtk_native_get_surface(native);
        GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(gdk_surface_get_display(surface),
                                                                          surface)
                                      : nullptr;
        refreshHz = monitor ? gdk_monitor_get_refresh_rate(monitor) / 1000.0 : 0;
    }
    StatsText::mergeScreen(&s, console->stats(), display->guestSize().width(),
                           display->guestSize().height(), refreshHz);
    return s;
}

void StatsButton::refresh()
{
    if (!m_vm) {
        return;
    }
    const VmStats::Snapshot s = snapshot();

    gtk_label_set_text(GTK_LABEL(m_title), utf8(m_vm->name()).constData());
    for (int i = 0; i < StatsText::kStats; i++) {
        const auto stat = StatsText::Stat(i);
        const QString value = StatsText::value(stat, s);
        const QString note = StatsText::note(stat, m_vm, m_sampler->guestSource(), s);
        gtk_label_set_text(GTK_LABEL(m_values[i]), value.isEmpty() ? "—" : utf8(value).constData());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(m_rows[i]), utf8(note.toHtmlEscaped()).constData());
    }
    if (m_details >= 0) {
        fillDetails(s);
    }
}

void StatsButton::fillDetails(const VmStats::Snapshot &s)
{
    VmConsole *console = m_consoleOf ? m_consoleOf() : nullptr;
    StatsText::Context context;

    context.ramMiB = VmConfig::memoryMiB(m_vm->runner()->runArgs());
    context.guest = m_sampler->guestSource();
    context.toolsFor = StatsText::toolsAreFor(m_vm);
    if (console && console->vm() == m_vm && console->display()) {
        context.screen = &console->stats();
    }
    const QString html = StatsText::details(StatsText::Stat(m_details), s, context);
    if (html == m_detailsHtml) {
        return;
    }
    m_detailsHtml = html;
    while (GtkWidget *child = gtk_widget_get_first_child(m_detailsBox)) {
        gtk_box_remove(GTK_BOX(m_detailsBox), child);
    }
    for (const Block &block : blocks(html)) {
        if (!block.isTable()) {
            GtkWidget *label = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(label), utf8(block.markup).constData());
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_wrap(GTK_LABEL(label), true);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 56);
            gtk_box_append(GTK_BOX(m_detailsBox), label);
            continue;
        }
        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        gtk_widget_set_margin_start(grid, 12);
        for (qsizetype r = 0; r < block.rows.size(); r++) {
            const QStringList &cells = block.rows[r];
            for (int c = 0; c < 3; c++) {
                GtkWidget *label = gtk_label_new(utf8(cells[c]).constData());
                gtk_label_set_xalign(GTK_LABEL(label), c == 1 ? 1 : 0);
                if (c == 1) {
                    gtk_widget_add_css_class(label, "numeric");
                } else if (c == 2) {
                    gtk_widget_add_css_class(label, "dim-label");
                    gtk_label_set_wrap(GTK_LABEL(label), true);
                    gtk_label_set_max_width_chars(GTK_LABEL(label), 34);
                    gtk_widget_set_hexpand(label, true);
                }
                gtk_grid_attach(GTK_GRID(grid), label, c, int(r), 1, 1);
            }
        }
        gtk_box_append(GTK_BOX(m_detailsBox), grid);
    }
}
