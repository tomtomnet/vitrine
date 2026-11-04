// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/vmpage.h"

#include <QTimer>

#include <cstring>

#include "core/gpucontexts.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/widgets.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

/* The menu of the VM's other actions, in the header bar: the window's actions */
static GMenuModel *machineMenu()
{
    GMenu *menu = g_menu_new();
    GMenu *power = g_menu_new();
    GMenu *files = g_menu_new();
    GMenu *vm = g_menu_new();

    g_menu_append(power, "_Reset…", "win.reset");
    g_menu_append(power, "_Force Off…", "win.force-off");
    g_menu_append(files, "_Settings", "win.settings");
    g_menu_append(files, "Show _Log", "win.show-log");
    g_menu_append(files, "_Open Folder", "win.open-folder");
    g_menu_append(files, "Show Comma_nd Line", "win.command-line");
    g_menu_append(vm, "_Clone…", "win.clone");
    g_menu_append(vm, "Re_move…", "win.remove");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(power));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(files));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(vm));
    g_object_unref(power);
    g_object_unref(files);
    g_object_unref(vm);
    return G_MENU_MODEL(menu);
}

static GtkWidget *iconButton(const char *icon, const char *tooltip, const char *action)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon);

    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
    return button;
}

VmPage::VmPage()
    : m_contexts(new GpuContexts(&m_qt)), m_growing(new QTimer(&m_qt)),
      m_noticeTimer(new QTimer(&m_qt)), m_root(adw_toolbar_view_new()),
      m_header(ADW_HEADER_BAR(adw_header_bar_new())), m_views(ADW_VIEW_STACK(adw_view_stack_new())),
      m_title(ADW_WINDOW_TITLE(adw_window_title_new("", ""))),
      m_switcherBar(ADW_VIEW_SWITCHER_BAR(adw_view_switcher_bar_new())),
      m_notice(ADW_BANNER(adw_banner_new(""))), m_closing(ADW_BANNER(adw_banner_new(""))),
      m_details(ADW_PREFERENCES_PAGE(adw_preferences_page_new()))
{
    /* its widgets live as long as the page, in the window or out of it once closed */
    g_object_ref_sink(m_root);
    buildHeader();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(m_root), GTK_WIDGET(m_header));
    /* notices over the views, not over the screen; what a closing window waits for first */
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(m_root), GTK_WIDGET(m_closing));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(m_root), GTK_WIDGET(m_notice));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(m_root), GTK_WIDGET(m_views));
    adw_view_switcher_bar_set_stack(m_switcherBar, m_views);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(m_root), GTK_WIDGET(m_switcherBar));
    /* the header bar's title while narrow (addNarrowSetters()): kept meanwhile */
    g_object_ref_sink(m_title);

    adw_view_stack_add_titled_with_icon(m_views, buildConsole(), "console", "Console",
                                        "computer-symbolic");

    /* Details: a header, the last error, a note, then the sections */
    {
        AdwPreferencesGroup *header = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *icon = gtk_image_new_from_icon_name("computer-symbolic");

        gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
        gtk_widget_add_css_class(icon, "dim-label");
        m_detailsName = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(m_detailsName), 0);
        gtk_label_set_wrap(GTK_LABEL(m_detailsName), true);
        gtk_widget_add_css_class(m_detailsName, "title-2");
        m_detailsState = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(m_detailsState), 0);
        gtk_widget_add_css_class(m_detailsState, "dim-label");
        gtk_widget_set_valign(titles, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(titles), m_detailsName);
        gtk_box_append(GTK_BOX(titles), m_detailsState);
        gtk_box_append(GTK_BOX(row), icon);
        gtk_box_append(GTK_BOX(row), titles);
        adw_preferences_group_add(header, row);
        adw_preferences_page_add(m_details, header);

        m_detailsNote = ADW_BANNER(adw_banner_new(""));
        adw_preferences_page_set_banner(m_details, m_detailsNote);

        m_errorGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(m_errorGroup, "Last Run");
        m_errorRow = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_errorRow),
                                      "The last run ended with an error");
        adw_action_row_set_subtitle_selectable(m_errorRow, true);
        adw_action_row_set_subtitle_lines(m_errorRow, 0);
        adw_action_row_add_prefix(m_errorRow, gtk_image_new_from_icon_name("dialog-warning-symbolic"));
        GtkWidget *log = gtk_button_new_with_mnemonic("Show _Log");
        gtk_widget_set_valign(log, GTK_ALIGN_CENTER);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(log), "win.show-log");
        adw_action_row_add_suffix(m_errorRow, log);
        adw_preferences_group_add(m_errorGroup, GTK_WIDGET(m_errorRow));
        gtk_widget_set_visible(GTK_WIDGET(m_errorGroup), false);
        adw_preferences_page_add(m_details, m_errorGroup);
    }
    adw_view_stack_add_titled_with_icon(m_views, GTK_WIDGET(m_details), "details", "Details",
                                        "document-properties-symbolic");

    /* the disks fill up as the VM runs, and its guest starts drawing */
    m_growing->setInterval(5000);
    QObject::connect(m_growing, &QTimer::timeout, &m_qt, [this]() {
        if (m_vm && m_vm->runner()->isActive() && gtk_widget_get_mapped(GTK_WIDGET(m_details))) {
            const VmConfig::Graphics g = VmConfig::graphics(m_vm->args());
            if (g.nativeContext || g.venus) {
                m_contexts->update(m_vm->runner()->qmp());
            }
            updateDetails();
        }
    });
    m_growing->start();
    QObject::connect(m_contexts, &GpuContexts::changed, &m_qt, [this]() { updateDetails(); });
    m_noticeTimer->setSingleShot(true);
    QObject::connect(m_noticeTimer, &QTimer::timeout, &m_qt,
                     [this]() { adw_banner_set_revealed(m_notice, false); });
}

VmPage::~VmPage()
{
    g_object_unref(m_title);
    g_object_unref(m_root);
}

void VmPage::addNarrowSetters(AdwBreakpoint *narrow)
{
    GValue title = G_VALUE_INIT;

    g_value_init(&title, GTK_TYPE_WIDGET);
    g_value_set_object(&title, m_title);
    adw_breakpoint_add_setter(narrow, G_OBJECT(m_header), "title-widget", &title);
    g_value_unset(&title);
    adw_breakpoint_add_setters(narrow, G_OBJECT(m_switcherBar), "reveal", true, nullptr);
}

void VmPage::buildHeader()
{
    GtkWidget *switcher = adw_view_switcher_new();
    GtkWidget *menu = gtk_menu_button_new();
    GMenuModel *model = machineMenu();

    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), m_views);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(m_header, switcher);

    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu), model);
    gtk_widget_set_tooltip_text(menu, "Machine Menu");
    g_object_unref(model);

    m_start = gtk_button_new();
    {
        GtkWidget *content = adw_button_content_new();
        adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "media-playback-start-symbolic");
        adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "Start");
        gtk_button_set_child(GTK_BUTTON(m_start), content);
    }
    /* not suggested: the Console's page has the big one */
    gtk_widget_set_tooltip_text(m_start, "Start the VM (Ctrl+Enter)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(m_start), "win.start");
    m_pause = iconButton("media-playback-pause-symbolic", "Pause", "win.pause");
    m_shutDown = iconButton("system-shutdown-symbolic",
                            "Shut Down: ask the guest to shut down, through its guest tools or "
                            "guest agent, else with the power button",
                            "win.shut-down");

    adw_header_bar_pack_end(m_header, menu);
    adw_header_bar_pack_end(m_header, m_shutDown);
    adw_header_bar_pack_end(m_header, m_pause);
    adw_header_bar_pack_end(m_header, m_start);
}

GtkWidget *VmPage::buildConsole()
{
    m_console = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(m_console, GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    /* Home: what the VM is, and Start */
    {
        GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
        GtkWidget *start = gtk_button_new_with_mnemonic("_Start");

        m_home = ADW_STATUS_PAGE(adw_status_page_new());
        adw_status_page_set_icon_name(m_home, "computer-symbolic");
        m_homeError = Gtk4::label({}, true);
        gtk_label_set_justify(GTK_LABEL(m_homeError), GTK_JUSTIFY_CENTER);
        gtk_label_set_xalign(GTK_LABEL(m_homeError), 0.5);
        gtk_widget_add_css_class(m_homeError, "error");
        gtk_widget_set_visible(m_homeError, false);
        gtk_widget_add_css_class(start, "pill");
        gtk_widget_add_css_class(start, "suggested-action");
        gtk_widget_set_halign(start, GTK_ALIGN_CENTER);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(start), "win.start");
        gtk_box_append(GTK_BOX(column), m_homeError);
        gtk_box_append(GTK_BOX(column), start);
        adw_status_page_set_child(m_home, column);
        gtk_stack_add_named(m_console, GTK_WIDGET(m_home), "home");
    }
    /* Starting, no screen, its own window */
    m_message = ADW_STATUS_PAGE(adw_status_page_new());
    gtk_stack_add_named(m_console, GTK_WIDGET(m_message), "message");
    /*
     * The screen: gtk-vmview's VmDisplay takes the place of this
     * placeholder, filling the area.  Until then the VM runs without it.
     */
    {
        GtkWidget *placeholder = adw_status_page_new();

        m_screen = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(m_screen, true);
        gtk_widget_set_vexpand(m_screen, true);
        gtk_widget_set_name(m_screen, "screen");
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(placeholder), "video-display-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(placeholder), "The Screen Goes Here");
        adw_status_page_set_description(
            ADW_STATUS_PAGE(placeholder),
            "The VM runs. This build of the GTK app does not show VM screens yet: the screen "
            "of a VM shown in Vitrine's window takes this place.");
        gtk_widget_set_vexpand(placeholder, true);
        gtk_box_append(GTK_BOX(m_screen), placeholder);
        gtk_stack_add_named(m_console, m_screen, "screen");
    }
    return GTK_WIDGET(m_console);
}

Vm *VmPage::vm() const
{
    return m_vm;
}

void VmPage::setVm(Vm *vm, const QString &error)
{
    if (vm != m_vm) {
        m_contexts->reset();
    }
    m_vm = vm;
    m_error = error;
    refresh();
}

void VmPage::setError(const QString &error)
{
    if (error != m_error) {
        m_error = error;
        refresh();
    }
}

VmPage::View VmPage::view() const
{
    const char *name = adw_view_stack_get_visible_child_name(m_views);
    return name && strcmp(name, "details") == 0 ? View::Details : View::Console;
}

void VmPage::setView(View view)
{
    adw_view_stack_set_visible_child_name(m_views, view == View::Details ? "details" : "console");
}

void VmPage::notify(const QString &text, int timeoutMs)
{
    adw_banner_set_title(m_notice, utf8(text.toHtmlEscaped()).constData());
    adw_banner_set_revealed(m_notice, true);
    m_noticeTimer->start(timeoutMs);
}

void VmPage::clearNotice(const QString &text)
{
    if (QString::fromUtf8(adw_banner_get_title(m_notice)) == text.toHtmlEscaped()) {
        m_noticeTimer->stop();
        adw_banner_set_revealed(m_notice, false);
    }
}

void VmPage::setClosing(const QString &text)
{
    if (!text.isEmpty()) {
        adw_banner_set_title(m_closing, utf8(text.toHtmlEscaped()).constData());
    }
    adw_banner_set_revealed(m_closing, !text.isEmpty());
}

void VmPage::setCovered(bool covered)
{
    /* gtk-vmview: VmDisplay::setCovered(covered) on the screen's display */
    m_covered = covered;
}

void VmPage::refresh()
{
    updateButtons();
    updateConsole();
    updateDetails();
}

void VmPage::updateButtons()
{
    const VmRunner::State state = m_vm ? m_vm->runner()->state() : VmRunner::State::Stopped;
    const bool paused = state == VmRunner::State::Paused;

    adw_window_title_set_title(m_title, m_vm ? utf8(m_vm->name()).constData() : "");
    adw_window_title_set_subtitle(m_title,
                                  m_vm ? utf8(VmSummary::stateText(m_vm)).constData() : "");

    /* Start while off; while on, Pause or Resume and Shut Down */
    gtk_widget_set_visible(m_start, state == VmRunner::State::Stopped);
    gtk_widget_set_visible(m_pause, state != VmRunner::State::Stopped);
    gtk_widget_set_visible(m_shutDown, state != VmRunner::State::Stopped);
    gtk_button_set_icon_name(GTK_BUTTON(m_pause), paused ? "media-playback-start-symbolic"
                                                         : "media-playback-pause-symbolic");
    gtk_widget_set_tooltip_text(m_pause, paused ? "Resume (Ctrl+P)" : "Pause (Ctrl+P)");
}

void VmPage::showConsolePage(const char *name)
{
    gtk_stack_set_visible_child_name(m_console, name);
}

void VmPage::updateConsole()
{
    if (!m_vm) {
        showConsolePage("home");
        return;
    }
    const VmRunner *runner = m_vm->runner();
    const VmRunner::State state = runner->state();

    adw_status_page_set_title(m_home, utf8(m_vm->name()).constData());
    adw_status_page_set_description(
        m_home, utf8((VmSummary::stateText(m_vm) + "\n" + VmSummary::homeText(m_vm->args()))
                         .toHtmlEscaped())
                    .constData());
    gtk_label_set_text(GTK_LABEL(m_homeError),
                       utf8(m_error.isEmpty() ? QString()
                                              : QString("The last run ended with an error: %1")
                                                    .arg(m_error.section('\n', 0, 0)))
                           .constData());
    gtk_widget_set_visible(m_homeError, !m_error.isEmpty());

    switch (state) {
    case VmRunner::State::Stopped:
    case VmRunner::State::Stopping:
        /* the guest is gone, or going: its screen with it */
        showConsolePage("home");
        return;
    case VmRunner::State::Starting:
        if (!runner->waitsForDisplay()) {
            /* the page's own frame clock turns it */
            AdwSpinnerPaintable *spinner = adw_spinner_paintable_new(GTK_WIDGET(m_message));
            adw_status_page_set_paintable(m_message, GDK_PAINTABLE(spinner));
            g_object_unref(spinner);
            adw_status_page_set_icon_name(m_message, nullptr);
            adw_status_page_set_title(m_message, "Starting…");
            adw_status_page_set_description(m_message, nullptr);
            showConsolePage("message");
            return;
        }
        break;
    case VmRunner::State::Running:
    case VmRunner::State::Paused:
        break;
    }
    if (!runner->displaySocket().isEmpty()) {
        showConsolePage("screen");
        return;
    }
    adw_status_page_set_paintable(m_message, nullptr);
    if (VmConfig::screen(runner->runArgs()) == VmConfig::Screen::OwnWindow) {
        adw_status_page_set_icon_name(m_message, "window-new-symbolic");
        adw_status_page_set_title(m_message, "Shown in Its Own Window");
        adw_status_page_set_description(
            m_message, "QEMU shows this VM in a window of its own, which stays open without "
                       "Vitrine.");
    } else {
        adw_status_page_set_icon_name(m_message, "computer-symbolic");
        adw_status_page_set_title(m_message, "No Screen");
        adw_status_page_set_description(
            m_message, "This VM runs without a display (-display none, -nographic or VNC, "
                       "for example).");
    }
    showConsolePage("message");
}

/* Its rows, in the same sections, keys and order: values may differ */
static bool sameShape(const QList<VmSummary::Section> &a, const QList<VmSummary::Section> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); i++) {
        if (a[i].title != b[i].title || a[i].rows.size() != b[i].rows.size()) {
            return false;
        }
        for (qsizetype j = 0; j < a[i].rows.size(); j++) {
            if (a[i].rows[j].key != b[i].rows[j].key ||
                a[i].rows[j].path.isEmpty() != b[i].rows[j].path.isEmpty()) {
                return false;
            }
        }
    }
    return true;
}

static QString rowText(const VmSummary::Row &row)
{
    return row.detail.isEmpty() ? row.value : row.value + "\n" + row.detail;
}

void VmPage::updateDetails()
{
    if (!m_vm) {
        return;
    }
    /* the documentation of the VM's QEMU tells the kinds of its devices */
    QemuDocs *docs = QemuDocs::forArgs(m_vm->args());
    if (docs != m_docs) {
        if (m_docs) {
            QObject::disconnect(m_docs, nullptr, &m_qt, nullptr);
        }
        m_docs = docs;
        QObject::connect(docs, &QemuDocs::changed, &m_qt, [this]() { updateDetails(); });
    }
    /* native context asked for: what the guest does with it, while it runs */
    const VmConfig::Graphics graphics = VmConfig::graphics(m_vm->args());
    if (!m_vm->runner()->isActive()) {
        m_contexts->reset();
    } else if ((graphics.nativeContext || graphics.venus) &&
               m_contexts->status() == GpuContexts::Status::Unknown) {
        m_contexts->update(m_vm->runner()->qmp());
    }

    gtk_label_set_text(GTK_LABEL(m_detailsName), utf8(m_vm->name()).constData());
    gtk_label_set_text(GTK_LABEL(m_detailsState), utf8(VmSummary::stateText(m_vm)).constData());
    adw_action_row_set_subtitle(m_errorRow, utf8(m_error.toHtmlEscaped()).constData());
    gtk_widget_set_visible(GTK_WIDGET(m_errorGroup), !m_error.isEmpty());

    QString note = VmSummary::keptOpen(m_vm)
                       ? QString("The guest has shut down. QEMU stays open because of the "
                                 "-no-shutdown option: Force Off closes it.")
                       : VmSummary::nativeContextNote(m_vm, m_contexts->status());
    adw_banner_set_title(m_detailsNote, utf8(note.toHtmlEscaped()).constData());
    adw_banner_set_revealed(m_detailsNote, !note.isEmpty());

    const QList<VmSummary::Section> sections =
        VmSummary::details(m_vm, docs->info(), m_contexts->status(), m_contexts->venusUsed());
    if (!sameShape(sections, m_shown)) {
        for (AdwPreferencesGroup *group : std::as_const(m_groups)) {
            adw_preferences_page_remove(m_details, group);
        }
        m_groups.clear();
        m_rows.clear();
        for (const VmSummary::Section &section : sections) {
            auto *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            QList<AdwActionRow *> rows;

            adw_preferences_group_set_title(group, utf8(section.title.toHtmlEscaped()).constData());
            for (const VmSummary::Row &row : section.rows) {
                auto *widget = ADW_ACTION_ROW(adw_action_row_new());

                /* the value first, the key over it: GNOME's property rows */
                gtk_widget_add_css_class(GTK_WIDGET(widget), "property");
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget),
                                              utf8(row.key.toHtmlEscaped()).constData());
                adw_action_row_set_subtitle_selectable(widget, true);
                if (!row.path.isEmpty()) {
                    GtkWidget *open = iconButton("folder-open-symbolic", "Open", nullptr);
                    const QString path = row.path;
                    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
                    gtk_widget_add_css_class(open, "flat");
                    Gtk4::connect<void()>(open, "clicked", [open, path]() { Gtk4::openPath(open, path); });
                    adw_action_row_add_suffix(widget, open);
                }
                adw_preferences_group_add(group, GTK_WIDGET(widget));
                rows << widget;
            }
            adw_preferences_page_add(m_details, group);
            m_groups << group;
            m_rows << rows;
        }
    }
    for (qsizetype i = 0; i < sections.size(); i++) {
        for (qsizetype j = 0; j < sections[i].rows.size(); j++) {
            adw_action_row_set_subtitle(m_rows[i][j],
                                        utf8(rowText(sections[i].rows[j]).toHtmlEscaped()).constData());
        }
    }
    m_shown = sections;
}
