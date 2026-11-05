// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/buildswindow.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent>

#include <tuple>
#include <utility>

#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/hostkde.h"
#include "core/hostkdebuilder.h"
#include "core/paths.h"
#include "core/tasks.h"
#include "gtk/hostkdealerts.h"
#include "gtk/markup.h"
#include "gtk/rows.h"
#include "gtk/textdialog.h"
#include "gtk/widgets.h"
#include "ui/buildtexts.h"

using Builds::Component;
using Builds::Kind;
using BuildsModel::Group;
using BuildsModel::Look;
using Gtk4::utf8;

namespace {

/* The lines of a log the window shows: the manager keeps more */
constexpr int kLogLines = 5000;
/* The details of a component building are shown again at most so often */
constexpr gint64 kDetailsEveryUs = 2 * G_USEC_PER_SEC;

const char *lookIcon(Look look)
{
    switch (look) {
    case Look::Waiting:
        return "content-loading-symbolic";
    case Look::Building:
        return "system-run-symbolic";
    case Look::Failed:
        return "dialog-error-symbolic";
    case Look::Stopped:
        return "process-stop-symbolic";
    case Look::UpToDate:
        return "object-select-symbolic";
    case Look::ToBuild:
        return "software-update-available-symbolic";
    case Look::Warning:
        return "dialog-warning-symbolic";
    case Look::Information:
        return "dialog-information-symbolic";
    case Look::None:
        break;
    }
    return "package-x-generic-symbolic";
}

/* The colours GNOME gives states: done, to do, failed, to see to */
void setLook(GtkWidget *image, Look look)
{
    gtk_image_set_from_icon_name(GTK_IMAGE(image), lookIcon(look));
    for (const char *style : {"success", "accent", "error", "warning"}) {
        gtk_widget_remove_css_class(image, style);
    }
    const char *style = look == Look::UpToDate  ? "success"
                        : look == Look::ToBuild ? "accent"
                        : look == Look::Failed  ? "error"
                        : look == Look::Warning ? "warning"
                                                : nullptr;
    if (style) {
        gtk_widget_add_css_class(image, style);
    }
}

/* What a component is, in the sidebar */
const char *kindIcon(Kind kind)
{
    switch (kind) {
    case Kind::Host:
        return "applications-engineering-symbolic";
    case Kind::Medium:
        return "media-optical-symbolic";
    case Kind::System:
        return "computer-symbolic";
    case Kind::Package:
        break;
    }
    return "package-x-generic-symbolic";
}

/*
 * A label of rich text, wrapping.  Not selectable: a page shown narrow
 * gives the focus to its first selectable label, all of its text selected
 */
GtkWidget *richLabel(const QString &html)
{
    GtkWidget *label = Gtk4::label({});

    gtk_label_set_markup(GTK_LABEL(label), utf8(Markup::pango(html)).constData());
    return label;
}

void setTip(GtkWidget *widget, const QString &text)
{
    gtk_widget_set_tooltip_text(widget, text.isEmpty() ? nullptr : utf8(text).constData());
}

}

BuildsWindow::BuildsWindow(GtkWindow *parent, BuildManager *manager)
    : m_manager(manager ? manager : BuildManager::instance()), m_window(adw_window_new()),
      m_split(ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new())),
      m_sidebar(ADW_SIDEBAR(adw_sidebar_new())), m_overviewSection(adw_sidebar_section_new()),
      m_actions(g_simple_action_group_new())
{
    const QSettings settings(Paths::settingsPath(), QSettings::IniFormat);

    /*
     * The window is GTK's: no reference of this side's, or it would never
     * be disposed, and say "destroy", once closed.  Its pages and actions
     * are this side's, for as long as it lives
     */
    gtk_window_set_title(GTK_WINDOW(m_window), "Builds");
    gtk_window_set_transient_for(GTK_WINDOW(m_window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(m_window), true);
    gtk_widget_set_size_request(m_window, 360, 294);
    gtk_window_set_default_size(GTK_WINDOW(m_window), settings.value("gtk/builds-width", 980).toInt(),
                                settings.value("gtk/builds-height", 700).toInt());

    createActions();
    buildSidebar();
    m_overviewPage = ADW_NAVIGATION_PAGE(adw_navigation_page_new_with_tag(buildOverview(),
                                                                          "Overview", "overview"));
    m_componentPage = ADW_NAVIGATION_PAGE(adw_navigation_page_new_with_tag(buildComponentPage(),
                                                                           "Component", "component"));
    /* the one not shown waits, kept */
    g_object_ref_sink(m_overviewPage);
    g_object_ref_sink(m_componentPage);
    adw_navigation_split_view_set_content(m_split, m_overviewPage);
    /* room for "Guest Tools for Fedora 45" and its state */
    adw_navigation_split_view_set_sidebar_width_fraction(m_split, 0.3);
    adw_navigation_split_view_set_min_sidebar_width(m_split, 280);
    adw_navigation_split_view_set_max_sidebar_width(m_split, 360);
    adw_window_set_content(ADW_WINDOW(m_window), GTK_WIDGET(m_split));

    /* narrow: one page at a time, the sidebar as a page of boxed lists */
    AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 640sp"));
    adw_breakpoint_add_setters(narrow, G_OBJECT(m_split), "collapsed", true, nullptr);
    adw_breakpoint_add_setters(narrow, G_OBJECT(m_sidebar), "mode", ADW_SIDEBAR_MODE_PAGE, nullptr);
    adw_breakpoint_add_setters(narrow, G_OBJECT(m_switcherBar), "reveal", true, nullptr);
    {
        GValue title = G_VALUE_INIT;
        g_value_init(&title, GTK_TYPE_WIDGET);
        g_value_set_object(&title, m_componentTitle);
        adw_breakpoint_add_setter(narrow, G_OBJECT(m_componentHeader), "title-widget", &title);
        g_value_unset(&title);
    }
    adw_window_add_breakpoint(ADW_WINDOW(m_window), narrow);

    /* Escape and Ctrl+W close it, as a dialog */
    GtkEventController *keys = gtk_shortcut_controller_new();
    for (const char *trigger : {"Escape", "<Control>w"}) {
        gtk_shortcut_controller_add_shortcut(
            GTK_SHORTCUT_CONTROLLER(keys),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger),
                             gtk_named_action_new("window.close")));
    }
    gtk_widget_add_controller(m_window, keys);

    on<gboolean()>(m_window, "close-request", [this]() -> gboolean {
        saveSize();
        return false;
    });
    on<void()>(m_window, "destroy", [this]() {
        m_destroyed = true;
        if (closed) {
            closed();
        }
        stopCallbacks();
    });
    /* back in front: what was built meanwhile; a run over, seen here */
    on<void(GParamSpec *)>(m_window, "notify::is-active", [this](GParamSpec *) {
        if (gtk_window_is_active(GTK_WINDOW(m_window))) {
            lookAgain();
            setSeen();
        }
    });

    /* the run, its components, their state */
    QObject::connect(m_manager, &BuildManager::progressChanged, &m_qt, [this](const QString &id) {
        refreshItem(id);
        refreshRun();
        /* the details of the one building: when its state changes, else now and then */
        if (id == m_selected && g_get_monotonic_time() - m_detailsShown >= kDetailsEveryUs) {
            refreshDetails();
        }
    });
    QObject::connect(m_manager, &BuildManager::statusChanged, &m_qt, [this]() { refreshAll(); });
    QObject::connect(m_manager, &BuildManager::runStarted, &m_qt, [this]() {
        m_showInstall = false;
        m_shownLog.clear();
        refreshAll();
        refreshMissing();
    });
    QObject::connect(m_manager, &BuildManager::runFinished, &m_qt, [this]() {
        refreshAll();
        refreshMissing();
        refreshMemory();
        updateCache();
    });
    QObject::connect(m_manager, &BuildManager::output, &m_qt,
                     [this](const QString &id, const QString &text) {
        if (id == m_shownLog && !(m_showInstall && m_installIds.contains(id))) {
            appendLog(text);
        }
    });
    QObject::connect(m_manager, &BuildManager::guestReleaseChanged, &m_qt, [this]() {
        fill();
        refreshAll();
    });
    QObject::connect(GuestToolsBuilder::instance(), &GuestToolsBuilder::waitingChanged, &m_qt,
                     [this]() { refreshMemory(); });
    QObject::connect(HostKdeBuilder::instance(), &HostKdeBuilder::waitingChanged, &m_qt,
                     [this]() { refreshMemory(); });

    /* a host package installed or reverted: the output in its Log */
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    QObject::connect(monitor, &HostKdeMonitor::started, &m_qt, [this, monitor]() {
        m_showInstall = true;
        m_installIds = monitor->busyWith();
        m_shownLog.clear();
        if (m_selected == "system" || m_installIds.contains(m_selected)) {
            if (!m_installIds.contains(m_selected) && !m_installIds.isEmpty()) {
                select(m_installIds.first());
            }
            adw_view_stack_set_visible_child_name(m_views, "log");
        }
        refreshLog();
        refreshButtons();
    });
    QObject::connect(monitor, &HostKdeMonitor::output, &m_qt, [this](const QString &text) {
        if (m_showInstall && m_installIds.contains(m_shownLog)) {
            appendLog(text);
        }
    });
    QObject::connect(monitor, &HostKdeMonitor::finished, &m_qt, [this]() { refreshButtons(); });

    /* the run's task, in the main window's list: seen here once it is over */
    QObject::connect(TaskList::instance(), &TaskList::taskFinished, &m_qt, [this](Task *task) {
        if (task == m_manager->task()) {
            setSeen();
        }
    });

    fill();
    refreshAll();
    refreshMissing();
    refreshMemory();
}

BuildsWindow::~BuildsWindow()
{
    stopCallbacks();
    if (!m_destroyed) {
        saveSize();
        gtk_window_destroy(GTK_WINDOW(m_window));
    }
    g_object_unref(m_overviewPage);
    g_object_unref(m_componentPage);
    g_object_unref(m_componentTitle);
    g_object_unref(m_actions);
}

void BuildsWindow::saveSize()
{
    QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    int width = 0, height = 0;

    gtk_window_get_default_size(GTK_WINDOW(m_window), &width, &height);
    if (width > 0 && height > 0) {
        settings.setValue("gtk/builds-width", width);
        settings.setValue("gtk/builds-height", height);
    }
}

void BuildsWindow::createActions()
{
    const auto add = [this](const char *name, std::function<void()> activated) {
        GSimpleAction *action = g_simple_action_new(name, nullptr);
        on<void(GVariant *)>(action, "activate",
                             [activated = std::move(activated)](GVariant *) { activated(); });
        g_action_map_add_action(G_ACTION_MAP(m_actions), G_ACTION(action));
        g_object_unref(action);
    };

    add("build", [this]() { buildSelected(false); });
    /* build trees in doubt: from nothing (the host's; the packages build from nothing anyway) */
    add("build-clean", [this]() { buildSelected(true); });
    add("build-all", [this]() { buildAll(); });
    add("stop", [this]() { stop(); });
    add("install", [this]() { install(m_selected); });
    add("revert", [this]() { revert(); });
    add("open-output", [this]() { openOutput(); });
    add("copy-path", [this]() { copyOutputPath(); });
    add("clear-cache", [this]() { clearCache(); });
    gtk_widget_insert_action_group(m_window, "builds", G_ACTION_GROUP(m_actions));
}

void BuildsWindow::buildSidebar()
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GMenu *menu = g_menu_new();
    GMenu *build = g_menu_new();
    GMenu *host = g_menu_new();
    GMenu *output = g_menu_new();
    AdwSidebarItem *overview = adw_sidebar_item_new("Overview");

    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new("Builds", ""));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    /* the items' menu, as the Qt window's tree has it */
    g_menu_append(build, "_Build or Update", "builds.build");
    {
        GMenuItem *item = g_menu_item_new("Build from _Scratch", "builds.build-clean");
        g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
        g_menu_append_item(build, item);
        g_object_unref(item);
    }
    for (const auto &[label, action] : {std::pair("_Install…", "builds.install"),
                                        std::pair("Re_vert…", "builds.revert")}) {
        GMenuItem *item = g_menu_item_new(label, action);
        g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
        g_menu_append_item(host, item);
        g_object_unref(item);
    }
    g_menu_append(output, "_Open Output Folder", "builds.open-output");
    g_menu_append(output, "_Copy Its Path", "builds.copy-path");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(build));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(host));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(output));
    g_object_unref(build);
    g_object_unref(host);
    g_object_unref(output);

    adw_sidebar_item_set_icon_name(overview, "view-grid-symbolic");
    g_object_set_data_full(G_OBJECT(overview), "vitrine-id", g_strdup(""), g_free);
    adw_sidebar_section_append(m_overviewSection, overview);
    adw_sidebar_append(m_sidebar, m_overviewSection);
    m_items << Item{QString(), overview, -1};
    adw_sidebar_set_menu_model(m_sidebar, G_MENU_MODEL(menu));
    g_object_unref(menu);

    on<void(GParamSpec *)>(m_sidebar, "notify::selected-item", [this](GParamSpec *) {
        if (m_filling) {
            return;
        }
        AdwSidebarItem *item = adw_sidebar_get_selected_item(m_sidebar);
        const char *id = item ? static_cast<const char *>(g_object_get_data(G_OBJECT(item), "vitrine-id"))
                              : nullptr;
        m_selected = id ? QString::fromUtf8(id) : QString();
        showSelected();
    });
    /* narrow: the page of what was chosen */
    on<void(guint)>(m_sidebar, "activated", [this](guint) {
        adw_navigation_split_view_set_show_content(m_split, true);
    });
    /* the menu acts on the item it opens on */
    on<void(AdwSidebarItem *)>(m_sidebar, "setup-menu", [this](AdwSidebarItem *item) {
        if (const char *id = item ? static_cast<const char *>(g_object_get_data(G_OBJECT(item), "vitrine-id"))
                                  : nullptr) {
            select(QString::fromUtf8(id));
        }
    });
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_sidebar));
    adw_navigation_split_view_set_sidebar(
        m_split, ADW_NAVIGATION_PAGE(adw_navigation_page_new_with_tag(view, "Builds", "sidebar")));
}

GtkWidget *BuildsWindow::buildOverview()
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    AdwPreferencesGroup *run = Gtk4::group();

    m_overview = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    m_overviewMemory = ADW_BANNER(adw_banner_new(""));
    adw_banner_set_use_markup(m_overviewMemory, true);
    adw_banner_set_button_label(m_overviewMemory, "Build _Anyway");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_overviewMemory));
    on<void()>(m_overviewMemory, "button-clicked", [this]() { m_manager->proceed(); });

    /* the run, or what there is to build */
    m_runRow = ADW_ACTION_ROW(adw_action_row_new());
    gtk_widget_set_name(GTK_WIDGET(m_runRow), "run");
    adw_action_row_set_title_lines(m_runRow, 0);
    m_runSpinner = adw_spinner_new();
    adw_action_row_add_prefix(m_runRow, m_runSpinner);
    m_all = gtk_button_new_with_mnemonic("Update _All");
    gtk_widget_add_css_class(m_all, "suggested-action");
    gtk_widget_set_valign(m_all, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(m_all), "builds.build-all");
    m_stopAll = gtk_button_new_with_mnemonic("_Stop");
    gtk_widget_add_css_class(m_stopAll, "destructive-action");
    gtk_widget_set_valign(m_stopAll, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(m_stopAll), "builds.stop");
    adw_action_row_add_suffix(m_runRow, m_all);
    adw_action_row_add_suffix(m_runRow, m_stopAll);
    adw_preferences_group_add(run, GTK_WIDGET(m_runRow));
    m_runProgress = gtk_progress_bar_new();
    gtk_widget_set_margin_top(m_runProgress, 12);
    adw_preferences_group_add(run, m_runProgress);
    m_background = Gtk4::caption("Closing this window does not stop the build.");
    gtk_widget_set_margin_top(m_background, 6);
    adw_preferences_group_add(run, m_background);
    adw_preferences_page_add(m_overview, run);

    /* what the host lacks */
    m_missingGroup = Gtk4::group("Needed for the Builds");
    m_missing = Gtk4::label({}, true);
    gtk_widget_set_name(m_missing, "missing");
    adw_preferences_group_add(m_missingGroup, m_missing);
    adw_preferences_page_add(m_overview, m_missingGroup);

    /* the build cache: its size, and Clear */
    m_cacheGroup = Gtk4::group("Build Cache");
    m_cacheRow = ADW_ACTION_ROW(Gtk4::actionRow("Sources and Build Trees", "Counting…"));
    gtk_widget_set_name(GTK_WIDGET(m_cacheRow), "cache");
    m_clear = Gtk4::rowTextButton("C_lear…", "The sources, the build trees and the compiler cache "
                                             "go; what is built stays");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(m_clear), "builds.clear-cache");
    adw_action_row_add_suffix(m_cacheRow, m_clear);
    adw_preferences_group_add(m_cacheGroup, GTK_WIDGET(m_cacheRow));
    adw_preferences_page_add(m_overview, m_cacheGroup);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_overview));
    return view;
}

GtkWidget *BuildsWindow::buildComponentPage()
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *switcher = adw_view_switcher_new();
    GMenu *menu = g_menu_new();
    GMenu *clean = g_menu_new();
    GMenu *output = g_menu_new();

    m_views = ADW_VIEW_STACK(adw_view_stack_new());
    m_switcherBar = ADW_VIEW_SWITCHER_BAR(adw_view_switcher_bar_new());
    m_componentTitle = ADW_WINDOW_TITLE(adw_window_title_new("", ""));
    /* the header bar's title while narrow (the breakpoint's): kept meanwhile */
    g_object_ref_sink(m_componentTitle);
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), m_views);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_view_switcher_bar_set_stack(m_switcherBar, m_views);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);

    m_build = gtk_button_new_with_mnemonic("_Build");
    gtk_widget_set_name(m_build, "build");
    gtk_widget_add_css_class(m_build, "suggested-action");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(m_build), "builds.build");
    m_stop = gtk_button_new_with_mnemonic("_Stop");
    gtk_widget_add_css_class(m_stop, "destructive-action");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(m_stop), "builds.stop");
    g_menu_append(clean, "Build from _Scratch", "builds.build-clean");
    g_menu_append(output, "_Open Output Folder", "builds.open-output");
    g_menu_append(output, "_Copy Its Path", "builds.copy-path");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(clean));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(output));
    m_menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_menu), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(m_menu), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(m_menu, "More");
    g_object_unref(clean);
    g_object_unref(output);
    g_object_unref(menu);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_stop);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_build);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    m_componentMemory = ADW_BANNER(adw_banner_new(""));
    adw_banner_set_use_markup(m_componentMemory, true);
    adw_banner_set_button_label(m_componentMemory, "Build _Anyway");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_componentMemory));
    on<void()>(m_componentMemory, "button-clicked", [this]() { m_manager->proceed(); });

    /* what it is and how it stands, where it comes from */
    m_details = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    gtk_widget_set_name(GTK_WIDGET(m_details), "details");
    adw_view_stack_add_titled_with_icon(m_views, GTK_WIDGET(m_details), "overview", "Overview",
                                        "document-properties-symbolic");

    /* its patches, by origin */
    m_patchesStack = gtk_stack_new();
    m_patches = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    {
        GtkWidget *none = adw_status_page_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(none), "document-edit-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(none), "No Patches");
        gtk_stack_add_named(GTK_STACK(m_patchesStack), none, "none");
    }
    gtk_stack_add_named(GTK_STACK(m_patchesStack), GTK_WIDGET(m_patches), "patches");
    adw_view_stack_add_titled_with_icon(m_views, m_patchesStack, "patches", "Patches",
                                        "document-edit-symbolic");

    /* its log, as it comes */
    m_logStack = gtk_stack_new();
    {
        GtkWidget *none = adw_status_page_new();
        GtkWidget *scroll = gtk_scrolled_window_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(none), "text-x-generic-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(none), "No Build of It Yet");
        gtk_stack_add_named(GTK_STACK(m_logStack), none, "none");
        m_logView = gtk_text_view_new();
        m_log = gtk_text_view_get_buffer(GTK_TEXT_VIEW(m_logView));
        gtk_widget_set_name(m_logView, "log");
        gtk_text_view_set_editable(GTK_TEXT_VIEW(m_logView), false);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(m_logView), false);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(m_logView), true);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(m_logView), GTK_WRAP_NONE);
        for (const auto margin : {&gtk_text_view_set_left_margin, &gtk_text_view_set_right_margin,
                                  &gtk_text_view_set_top_margin, &gtk_text_view_set_bottom_margin}) {
            margin(GTK_TEXT_VIEW(m_logView), 12);
        }
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m_logView);
        gtk_widget_set_vexpand(scroll, true);
        gtk_stack_add_named(GTK_STACK(m_logStack), scroll, "log");
    }
    adw_view_stack_add_titled_with_icon(m_views, m_logStack, "log", "Log", "text-x-generic-symbolic");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_views));
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_switcherBar));
    /* the log of the view shown: its end as it comes */
    on<void(GParamSpec *)>(m_views, "notify::visible-child-name", [this](GParamSpec *) {
        refreshLog();
    });
    m_componentHeader = header;
    return view;
}

void BuildsWindow::fill()
{
    const QString was = m_selected;
    const QString release = m_manager->guestRelease();

    m_filling = true;
    for (AdwSidebarSection *section : std::as_const(m_sections)) {
        adw_sidebar_remove(m_sidebar, section);
    }
    m_sections.clear();
    m_items.erase(m_items.begin() + 1, m_items.end());
    for (const GroupBox &box : std::as_const(m_groupBoxes)) {
        adw_preferences_page_remove(m_overview, box.box);
    }
    m_groupBoxes.clear();
    m_rows.clear();

    int at = 2;     // after the run and what is missing
    for (const Group group : BuildsModel::groups(*m_manager)) {
        const QString groupId = BuildsModel::groupId(group);
        const QString groupTitle = BuildsModel::groupTitle(group, release);
        AdwSidebarSection *section = adw_sidebar_section_new();
        GroupBox box{group, Gtk4::group(groupTitle), nullptr, nullptr, nullptr};
        GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        adw_sidebar_section_set_title(section, utf8(groupTitle).constData());
        GtkWidget *state = gtk_label_new(nullptr);
        gtk_widget_add_css_class(state, "dim-label");
        gtk_widget_add_css_class(state, "caption");
        adw_sidebar_section_set_suffix(section, state);
        g_object_set_data(G_OBJECT(section), "vitrine-state", state);

        /* what the group makes, its Build, the host desktop's Install All… */
        adw_preferences_group_set_description(
            box.box, utf8(Markup::pango(BuildTexts::groupAbout(groupId, release))).constData());
        gtk_widget_set_name(GTK_WIDGET(box.box), utf8("group-" + groupId).constData());
        box.build = gtk_button_new_with_mnemonic("_Build");
        gtk_widget_set_valign(box.build, GTK_ALIGN_CENTER);
        on<void()>(box.build, "clicked", [this, groupId]() {
            const QStringList todo =
                BuildsModel::toBuild(*m_manager, BuildsModel::selection(*m_manager, groupId));
            if (!todo.isEmpty()) {
                m_manager->build(todo);
            }
        });
        if (group == Group::System) {
            box.install = gtk_button_new_with_mnemonic("_Install All…");
            gtk_widget_set_valign(box.install, GTK_ALIGN_CENTER);
            on<void()>(box.install, "clicked", [this]() { install("system"); });
            gtk_box_append(GTK_BOX(buttons), box.install);
        }
        gtk_box_append(GTK_BOX(buttons), box.build);
        adw_preferences_group_set_header_suffix(box.box, buttons);
        /* the guest tools of one Fedora release at a time: each has its medium */
        if (group == Group::Guest) {
            QStringList releases;
            for (const QString &r : GuestTools::releases()) {
                releases << QString("Fedora %1").arg(r);
            }
            box.release = Gtk4::comboRow("Fedora Release", releases);
            gtk_widget_set_name(box.release, "release");
            Gtk4::setSelected(box.release, int(GuestTools::releases().indexOf(release)));
            adw_preferences_group_add(box.box, box.release);
        }

        for (const QString &id : BuildsModel::componentsOf(*m_manager, group)) {
            const Component c = m_manager->component(id);
            AdwSidebarItem *item = adw_sidebar_item_new(utf8(c.title).constData());
            Row row;

            adw_sidebar_item_set_icon_name(item, kindIcon(c.kind));
            g_object_set_data_full(G_OBJECT(item), "vitrine-id", g_strdup(utf8(id).constData()), g_free);
            adw_sidebar_section_append(section, item);
            m_items << Item{id, item, -1};

            row.row = adw_action_row_new();
            gtk_widget_set_name(row.row, utf8("row-" + id).constData());
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row.row), true);
            row.icon = gtk_image_new();
            adw_action_row_add_prefix(ADW_ACTION_ROW(row.row), row.icon);
            row.progress = gtk_progress_bar_new();
            gtk_widget_set_valign(row.progress, GTK_ALIGN_CENTER);
            gtk_widget_set_size_request(row.progress, 80, -1);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row.row), row.progress);
            row.built = gtk_label_new(nullptr);
            gtk_widget_add_css_class(row.built, "dim-label");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row.row), row.built);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row.row), gtk_image_new_from_icon_name("go-next-symbolic"));
            on<void()>(row.row, "activated", [this, id]() {
                select(id);
                adw_navigation_split_view_set_show_content(m_split, true);
            });
            adw_preferences_group_add(box.box, row.row);
            m_rows.insert(id, row);
        }
        adw_sidebar_append(m_sidebar, section);
        m_sections << section;
        adw_preferences_page_insert(m_overview, box.box, at++);
        m_groupBoxes << box;
    }
    /* the release chosen: its components, its medium (refused during a run: back to its own) */
    for (const GroupBox &box : std::as_const(m_groupBoxes)) {
        if (box.release) {
            GtkWidget *combo = box.release;
            on<void(GParamSpec *)>(combo, "notify::selected", [this, combo](GParamSpec *) {
                if (m_filling) {
                    return;
                }
                const QString chosen = GuestTools::releases().value(Gtk4::selected(combo));
                if (chosen.isEmpty() || chosen == m_manager->guestRelease()) {
                    return;
                }
                /* not from within the combo's own signal: the window changes under it */
                QTimer::singleShot(0, &m_qt, [this, chosen]() {
                    if (!m_manager->setGuestRelease(chosen)) {
                        refreshButtons();
                    }
                });
            });
        }
    }
    m_filling = false;
    select(was);
}

void BuildsWindow::present(const QString &component)
{
    if (m_destroyed) {
        return;
    }
    const bool first = !gtk_widget_get_visible(m_window);

    if (!component.isEmpty()) {
        select(component);
    }
    gtk_window_present(GTK_WINDOW(m_window));
    /* shown: the sources may have changed meanwhile, and the system (Plasma, Qt) */
    if (first) {
        if (!m_manager->isRunning()) {
            m_manager->refresh();
        }
        HostKdeMonitor::instance()->refresh();
        updateCache();
    }
}

void BuildsWindow::select(const QString &component)
{
    QString id = component;

    /* a group: the Overview, which shows them */
    if (BuildsModel::isGroupId(id)) {
        id.clear();
    }
    for (const Item &item : std::as_const(m_items)) {
        if (item.id == id) {
            adw_sidebar_set_selected(m_sidebar, adw_sidebar_item_get_index(item.item));
            if (m_selected != id) {
                m_selected = id;
                showSelected();
            }
            return;
        }
    }
    adw_sidebar_set_selected(m_sidebar, 0);
}

QString BuildsWindow::selected() const
{
    return m_selected;
}

void BuildsWindow::showSelected()
{
    const Component c = m_manager->component(m_selected);

    if (m_selected.isEmpty() || !c.isValid()) {
        adw_navigation_split_view_set_content(m_split, m_overviewPage);
        refreshButtons();
        return;
    }
    adw_navigation_page_set_title(m_componentPage, utf8(c.title).constData());
    adw_window_title_set_title(m_componentTitle, utf8(c.title).constData());
    adw_navigation_split_view_set_content(m_split, m_componentPage);
    refreshDetails();
    refreshPatches();
    if (BuildsModel::patchGroups(*m_manager, m_selected).isEmpty() &&
        g_strcmp0(adw_view_stack_get_visible_child_name(m_views), "patches") == 0) {
        adw_view_stack_set_visible_child_name(m_views, "overview");
    }
    refreshLog();
    refreshButtons();
}

void BuildsWindow::lookAgain()
{
    /* a run says itself what it builds; the host packages: one reading at a time */
    if (!m_manager->isRunning()) {
        m_manager->refresh();
        HostKdeMonitor::instance()->lookAgain();
    }
}

void BuildsWindow::presentDialog(AdwDialog *dialog)
{
    if (m_destroyed) {
        g_object_ref_sink(dialog);
        g_object_unref(dialog);
        return;
    }
    adw_dialog_present(dialog, m_window);
}

void BuildsWindow::refreshAll()
{
    /* a component more or less (the sources changed): anew */
    QStringList ids;
    QStringList shown;
    for (const Component &c : m_manager->components()) {
        ids << c.id;
    }
    for (const Item &item : std::as_const(m_items)) {
        if (!item.id.isEmpty()) {
            shown << item.id;
        }
    }
    if (ids != shown && !m_filling) {
        fill();
    }
    for (const QString &id : std::as_const(ids)) {
        refreshItem(id);
    }
    refreshRun();
    refreshMemory();
    if (!m_selected.isEmpty()) {
        refreshDetails();
        m_shownPatches.clear();
        refreshPatches();
        refreshLog();
    }
    refreshButtons();
}

void BuildsWindow::refreshItem(const QString &id)
{
    const BuildsModel::Status status = BuildsModel::status(*m_manager, id);
    const QString title = BuildsModel::title(*m_manager, id);

    for (Item &item : m_items) {
        if (item.id != id) {
            continue;
        }
        /* a new one only when it changes: progress comes by the thousand, a spinner turns on */
        if (int(status.look) != item.look) {
            GtkWidget *suffix = nullptr;
            item.look = int(status.look);
            if (status.look == Look::Building) {
                suffix = adw_spinner_new();
            } else if (status.look != Look::None) {
                suffix = gtk_image_new();
                setLook(suffix, status.look);
            }
            adw_sidebar_item_set_suffix(item.item, suffix);
        }
        adw_sidebar_item_set_title(item.item, utf8(title).constData());
        adw_sidebar_item_set_subtitle(item.item, utf8(status.text).constData());
        adw_sidebar_item_set_tooltip(item.item, utf8(title + " — " + status.text).constData());
    }
    if (m_rows.contains(id)) {
        const Row &row = m_rows[id];
        Gtk4::setTitle(row.row, title);
        Gtk4::setSubtitle(row.row, status.text);
        setLook(row.icon, status.look);
        gtk_widget_set_visible(row.progress, status.fraction >= 0);
        if (status.fraction >= 0) {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(row.progress), status.fraction);
        }
        gtk_label_set_text(GTK_LABEL(row.built), utf8(BuildsModel::built(*m_manager, id)).constData());
        setTip(row.built, BuildsModel::built(*m_manager, id, true));
    }
    /* the group's state, beside its title */
    const Group group = BuildsModel::groupOf(m_manager->component(id).kind);
    for (qsizetype i = 0; i < m_groupBoxes.size() && i < m_sections.size(); i++) {
        if (m_groupBoxes[i].group == group) {
            auto *label = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(m_sections[i]),
                                                                     "vitrine-state"));
            gtk_label_set_text(GTK_LABEL(label),
                               utf8(BuildsModel::groupStatus(*m_manager, group)).constData());
        }
    }
}

void BuildsWindow::refreshRun()
{
    const bool running = m_manager->isRunning();
    const BuildsModel::Action all = BuildsModel::buildAll(*m_manager);
    const Task *task = m_manager->task();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(m_runRow),
                                  utf8(Markup::escaped(BuildsModel::runText(*m_manager))).constData());
    gtk_widget_set_visible(m_runSpinner, running);
    gtk_widget_set_visible(m_runProgress, running);
    gtk_widget_set_visible(m_background, running);
    if (running && task) {
        if (task->fraction() >= 0) {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m_runProgress), task->fraction());
        } else {
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(m_runProgress));
        }
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(m_runProgress), utf8(task->text()).constData());
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(m_runProgress), true);
    }
    gtk_widget_set_visible(m_all, !running);
    gtk_button_set_label(GTK_BUTTON(m_all), utf8(all.label).constData());
    setTip(m_all, all.tooltip);
    gtk_widget_set_visible(m_stopAll, running);
    g_simple_action_set_enabled(
        G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_actions), "build-all")), all.enabled);
    g_simple_action_set_enabled(
        G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_actions), "stop")), running);
}

void BuildsWindow::refreshMemory()
{
    const QString text = BuildsModel::memoryText(*m_manager);

    for (AdwBanner *banner : {m_overviewMemory, m_componentMemory}) {
        if (!text.isEmpty()) {
            adw_banner_set_title(banner, utf8(Markup::pango(text)).constData());
        }
        adw_banner_set_revealed(banner, !text.isEmpty());
    }
}

void BuildsWindow::refreshMissing()
{
    const QString text = BuildsModel::missingText(*m_manager);

    gtk_label_set_markup(GTK_LABEL(m_missing), utf8(Markup::pango(text)).constData());
    gtk_widget_set_visible(GTK_WIDGET(m_missingGroup), !text.isEmpty());
}

void BuildsWindow::refreshButtons()
{
    const bool running = m_manager->isRunning();
    const Component c = m_manager->component(m_selected);
    const BuildsModel::Action build = BuildsModel::build(*m_manager, m_selected);
    const auto action = [this](const char *name) {
        return G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_actions), name));
    };
    const bool host = c.isValid() && c.kind == Kind::System;
    const Builds::Status status = m_manager->status(m_selected);

    gtk_button_set_label(GTK_BUTTON(m_build), utf8(build.label).constData());
    setTip(m_build, build.tooltip);
    g_simple_action_set_enabled(action("build"), c.isValid() && build.enabled);
    g_simple_action_set_enabled(action("build-clean"),
                                c.isValid() && c.kind == Kind::Host && build.enabled);
    gtk_widget_set_visible(m_stop, running);
    if (host) {
        const BuildsModel::Action install = BuildsModel::install(*m_manager, m_selected);
        const BuildsModel::Action revert = BuildsModel::revert(*m_manager, m_selected);
        g_simple_action_set_enabled(action("install"), install.enabled);
        g_simple_action_set_enabled(action("revert"), revert.enabled);
        /* the rows of the Overview say what they do, or why not */
        if (m_installRow) {
            Gtk4::setSubtitle(m_installRow, install.tooltip);
            Gtk4::setSubtitle(m_revertRow, revert.tooltip);
        }
    } else {
        g_simple_action_set_enabled(action("install"), false);
        g_simple_action_set_enabled(action("revert"), false);
    }
    g_simple_action_set_enabled(action("open-output"), c.isValid() && !status.output.isEmpty());
    g_simple_action_set_enabled(action("copy-path"), c.isValid() && !status.output.isEmpty());
    g_simple_action_set_enabled(action("clear-cache"), !running);
    gtk_widget_set_sensitive(m_clear, !running);

    /* the groups' own */
    for (const GroupBox &box : std::as_const(m_groupBoxes)) {
        const QString id = BuildsModel::groupId(box.group);
        const BuildsModel::Action groupBuild = BuildsModel::build(*m_manager, id);
        gtk_button_set_label(GTK_BUTTON(box.build), utf8(groupBuild.label).constData());
        setTip(box.build, groupBuild.tooltip);
        gtk_widget_set_sensitive(box.build, groupBuild.enabled);
        if (box.install) {
            const BuildsModel::Action all = BuildsModel::install(*m_manager, "system");
            setTip(box.install, all.tooltip);
            gtk_widget_set_sensitive(box.install, all.enabled);
        }
        if (box.release) {
            gtk_widget_set_sensitive(box.release, !running);
            m_filling = true;
            Gtk4::setSelected(box.release,
                              int(GuestTools::releases().indexOf(m_manager->guestRelease())));
            m_filling = false;
        }
    }
}

void BuildsWindow::refreshDetails()
{
    m_detailsShown = g_get_monotonic_time();
    for (AdwPreferencesGroup *group : std::as_const(m_detailGroups)) {
        adw_preferences_page_remove(m_details, group);
    }
    m_detailGroups.clear();
    if (m_selected.isEmpty()) {
        return;
    }
    const BuildsModel::Details d =
        BuildsModel::details(*m_manager, m_selected, [this](const QString &path) { return size(path); });

    AdwPreferencesGroup *about = Gtk4::group();
    for (const QString &paragraph : d.paragraphs) {
        GtkWidget *label = richLabel(paragraph);
        gtk_widget_set_margin_bottom(label, 6);
        adw_preferences_group_add(about, label);
    }
    adw_preferences_page_add(m_details, about);
    m_detailGroups << about;
    m_installRow = m_revertRow = nullptr;
    if (m_manager->component(m_selected).kind == Kind::System) {
        AdwPreferencesGroup *computer = Gtk4::group("This Computer");
        const QString title = m_manager->component(m_selected).title;
        m_installRow = Gtk4::actionRow(QString("Install Vitrine's %1").arg(title));
        m_revertRow = Gtk4::actionRow(QString("Revert to the Distribution's %1").arg(title));
        for (const auto &[row, label, name] :
             {std::tuple(m_installRow, "_Install…", "builds.install"),
              std::tuple(m_revertRow, "Re_vert…", "builds.revert")}) {
            GtkWidget *button = Gtk4::rowTextButton(label);
            gtk_widget_set_name(button, name + 7);
            gtk_actionable_set_action_name(GTK_ACTIONABLE(button), name);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
            adw_preferences_group_add(computer, row);
        }
        adw_preferences_page_add(m_details, computer);
        m_detailGroups << computer;
        refreshButtons();
    }
    if (!d.changes.isEmpty()) {
        AdwPreferencesGroup *changes = Gtk4::group("Changed Since Its Build");
        for (const QString &change : d.changes) {
            GtkWidget *row = Gtk4::actionRow(change);
            adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 0);
            adw_preferences_group_add(changes, row);
        }
        adw_preferences_page_add(m_details, changes);
        m_detailGroups << changes;
    }
    if (!d.properties.isEmpty()) {
        AdwPreferencesGroup *source = Gtk4::group("Where It Comes From");
        for (const BuildsModel::Property &p : d.properties) {
            GtkWidget *row = adw_action_row_new();
            /* the value first, the key over it: GNOME's property rows */
            gtk_widget_add_css_class(row, "property");
            Gtk4::setTitle(row, p.label);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), utf8(Markup::pango(p.value)).constData());
            adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), true);
            if (!p.link.isEmpty()) {
                GtkWidget *open = Gtk4::rowButton("adw-external-link-symbolic", "Open in the Browser");
                const QString link = p.link;
                on<void()>(open, "clicked", [this, link]() {
                    GtkUriLauncher *launcher = gtk_uri_launcher_new(utf8(link).constData());
                    gtk_uri_launcher_launch(launcher, GTK_WINDOW(m_window), nullptr, nullptr, nullptr);
                    g_object_unref(launcher);
                });
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
            }
            if (!p.path.isEmpty()) {
                GtkWidget *open = Gtk4::rowButton("folder-open-symbolic", "Open Folder");
                const QString path = p.path;
                on<void()>(open, "clicked", [this, path]() {
                    const QFileInfo info(path);
                    Gtk4::openPath(m_window, info.isDir() ? path : info.absolutePath());
                });
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
            }
            adw_preferences_group_add(source, row);
        }
        adw_preferences_page_add(m_details, source);
        m_detailGroups << source;
    }
    if (!d.after.isEmpty()) {
        AdwPreferencesGroup *after = Gtk4::group();
        for (const QString &paragraph : d.after) {
            adw_preferences_group_add(after, richLabel(paragraph));
        }
        adw_preferences_page_add(m_details, after);
        m_detailGroups << after;
    }
}

void BuildsWindow::refreshPatches()
{
    const Component c = m_manager->component(m_selected);

    if (m_selected == m_shownPatches) {
        return;
    }
    m_shownPatches = m_selected;
    for (AdwPreferencesGroup *group : std::as_const(m_patchGroups)) {
        adw_preferences_page_remove(m_patches, group);
    }
    m_patchGroups.clear();
    const QList<BuildsModel::PatchGroup> groups = BuildsModel::patchGroups(*m_manager, m_selected);
    for (const BuildsModel::PatchGroup &g : groups) {
        AdwPreferencesGroup *group = Gtk4::group(g.title);
        for (qsizetype i = 0; i < g.patches.size(); i++) {
            const Builds::Patch patch = g.patches[i];
            const QString subject = g.subjects.value(i);
            GtkWidget *row = Gtk4::actionRow(subject, patch.name());
            gtk_widget_set_tooltip_text(row, utf8(patch.path).constData());
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
            /* its text on demand */
            on<void()>(row, "activated", [this, c, patch, subject]() {
                auto *text = new TextDialog(patch.name(), c.title, Builds::text(c, patch), {});
                adw_dialog_present(text->dialog(), m_window);
            });
            adw_preferences_group_add(group, row);
        }
        adw_preferences_page_add(m_patches, group);
        m_patchGroups << group;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(m_patchesStack), groups.isEmpty() ? "none" : "patches");
    adw_view_stack_page_set_visible(adw_view_stack_get_page(m_views, m_patchesStack), !groups.isEmpty());
}

void BuildsWindow::refreshLog()
{
    const QString id = m_manager->component(m_selected).isValid() ? m_selected : QString();

    if (id == m_shownLog && !id.isEmpty()) {
        return;
    }
    m_shownLog = id;
    QString text = id.isEmpty() ? QString() : m_manager->log(id);
    if (m_showInstall && m_installIds.contains(id)) {
        text = HostKdeMonitor::instance()->log();
    }
    /* the end of it: the lines the window keeps */
    qsizetype at = text.size();
    for (int i = 0; i <= kLogLines && at > 0; i++) {
        at = text.lastIndexOf('\n', at - 1);
        if (at < 0) {
            break;
        }
    }
    if (at > 0) {
        text = text.mid(at + 1);
    }
    gtk_text_buffer_set_text(m_log, utf8(text).constData(), -1);
    gtk_stack_set_visible_child_name(GTK_STACK(m_logStack), text.isEmpty() ? "none" : "log");
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(m_log, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(m_logView), &end, 0, false, 0, 1);
}

void BuildsWindow::appendLog(const QString &text)
{
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(m_log, &end);
    gtk_text_buffer_insert(m_log, &end, utf8(text).constData(), -1);
    /* the lines it keeps: the first ones go */
    if (const int lines = gtk_text_buffer_get_line_count(m_log); lines > kLogLines + 500) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(m_log, &start);
        gtk_text_buffer_get_iter_at_line(m_log, &cut, lines - kLogLines);
        gtk_text_buffer_delete(m_log, &start, &cut);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(m_logStack), "log");
    gtk_text_buffer_get_end_iter(m_log, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(m_logView), &end, 0, false, 0, 1);
}

qint64 BuildsWindow::size(const QString &path)
{
    if (m_sizes.contains(path)) {
        return m_sizes.value(path);
    }
    m_sizes.insert(path, -1);
    auto *watcher = new QFutureWatcher<qint64>(&m_qt);
    QObject::connect(watcher, &QFutureWatcher<qint64>::finished, &m_qt, [this, watcher, path]() {
        m_sizes.insert(path, watcher->result());
        watcher->deleteLater();
        if (m_manager->status(m_selected).output == path) {
            refreshDetails();
        }
    });
    watcher->setFuture(QtConcurrent::run([path]() { return BuildsModel::diskUsage(path); }));
    return -1;
}

void BuildsWindow::updateCache()
{
    const QStringList dirs = m_manager->cacheDirs();
    auto *watcher = new QFutureWatcher<qint64>(&m_qt);

    /* the folders' sizes again, and those of the outputs */
    m_sizes.clear();
    Gtk4::setSubtitle(GTK_WIDGET(m_cacheRow), "Counting…");
    gtk_widget_set_tooltip_text(
        GTK_WIDGET(m_cacheRow),
        utf8(QString("Sources and build trees, downloads, the compiler cache:\n%1").arg(dirs.join('\n')))
            .constData());
    QObject::connect(watcher, &QFutureWatcher<qint64>::finished, &m_qt, [this, watcher]() {
        m_cacheSize = watcher->result();
        Gtk4::setSubtitle(GTK_WIDGET(m_cacheRow), m_cacheSize > 0 ? BuildsModel::bytes(m_cacheSize)
                                                                  : QString("Empty"));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([dirs]() {
        qint64 total = 0;
        for (const QString &dir : dirs) {
            total += BuildsModel::diskUsage(dir);
        }
        return total;
    }));
}

void BuildsWindow::clearCache()
{
    AdwDialog *dialog = Gtk4::alert(
        "Clear the Build Cache?",
        QString("The sources, the build trees and the compiler cache go (%1): the next build of "
                "each component downloads its sources and compiles all of it again. What is "
                "built stays.")
            .arg(m_cacheSize >= 0 ? BuildsModel::bytes(m_cacheSize) : QString("all of it")),
        {{"cancel", "_Cancel"}, {"clear", "C_lear"}}, "clear", true);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    Gtk4::onResponse(dialog, guarded<void(const QString &)>([this](const QString &response) {
        QString error;
        if (response != "clear") {
            return;
        }
        if (!m_manager->clearCache(&error)) {
            presentDialog(Gtk4::alert("Clear the Build Cache", error));
        }
        updateCache();
    }));
    presentDialog(dialog);
}

void BuildsWindow::buildSelected(bool clean)
{
    const QStringList todo =
        BuildsModel::toBuild(*m_manager, BuildsModel::selection(*m_manager, m_selected));

    if (!todo.isEmpty()) {
        m_manager->build(todo, clean);
    }
}

void BuildsWindow::buildAll()
{
    m_manager->buildOutdated();
}

void BuildsWindow::stop()
{
    m_manager->cancel();
}

void BuildsWindow::install(const QString &id)
{
    const auto present = [this](AdwDialog *dialog) { presentDialog(dialog); };

    /* the group: every package built and ready, at once (one password) */
    if (id == "system") {
        HostKdeAlerts::installAll(present, BuildsModel::installable(*m_manager));
    } else {
        HostKdeAlerts::install(present, id);
    }
}

void BuildsWindow::revert()
{
    HostKdeAlerts::revert([this](AdwDialog *dialog) { presentDialog(dialog); }, {m_selected});
}

void BuildsWindow::openOutput()
{
    const QString output = m_manager->status(m_selected).output;
    const QFileInfo info(output);

    if (!output.isEmpty()) {
        Gtk4::openPath(m_window, info.isDir() ? output : info.absolutePath());
    }
}

void BuildsWindow::copyOutputPath()
{
    const QString output = m_manager->status(m_selected).output;

    if (!output.isEmpty()) {
        gdk_clipboard_set_text(gtk_widget_get_clipboard(m_window), utf8(output).constData());
    }
}

void BuildsWindow::setSeen()
{
    Task *task = m_manager->task();

    if (task && !task->isRunning() && !m_destroyed && gtk_widget_get_visible(m_window) &&
        gtk_window_is_active(GTK_WINDOW(m_window))) {
        task->seen();
    }
}

QString BuildsWindow::itemSubtitle(const QString &id) const
{
    for (const Item &item : m_items) {
        if (item.id == id) {
            return QString::fromUtf8(adw_sidebar_item_get_subtitle(item.item));
        }
    }
    return {};
}

QStringList BuildsWindow::items() const
{
    QStringList ids;
    for (const Item &item : m_items) {
        ids << item.id;
    }
    return ids;
}

QString BuildsWindow::runText() const
{
    return Markup::plain(QString::fromUtf8(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(m_runRow))));
}

QString BuildsWindow::buildLabel() const
{
    return QString::fromUtf8(gtk_button_get_label(GTK_BUTTON(m_build))).remove('_');
}

bool BuildsWindow::canBuild() const
{
    return g_action_get_enabled(g_action_map_lookup_action(G_ACTION_MAP(m_actions), "build"));
}

QString BuildsWindow::cacheText() const
{
    return QString::fromUtf8(adw_action_row_get_subtitle(m_cacheRow));
}

QString BuildsWindow::memoryText() const
{
    return adw_banner_get_revealed(m_overviewMemory)
               ? Markup::plain(QString::fromUtf8(adw_banner_get_title(m_overviewMemory)))
               : QString();
}

QString BuildsWindow::detailsText() const
{
    QStringList texts;
    std::function<void(GtkWidget *)> walk = [&](GtkWidget *widget) {
        if (GTK_IS_LABEL(widget) && gtk_widget_get_visible(widget)) {
            const QString text = QString::fromUtf8(gtk_label_get_text(GTK_LABEL(widget)));
            if (!text.isEmpty()) {
                texts << text;
            }
        }
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            walk(child);
        }
    };
    walk(GTK_WIDGET(m_details));
    return texts.join('\n');
}

QStringList BuildsWindow::patchRows() const
{
    QStringList rows;
    for (AdwPreferencesGroup *group : m_patchGroups) {
        for (guint i = 0; GtkWidget *row = adw_preferences_group_get_row(group, i); i++) {
            rows << Markup::plain(QString::fromUtf8(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row))));
        }
    }
    return rows;
}

QString BuildsWindow::logText() const
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(m_log, &start, &end);
    char *text = gtk_text_buffer_get_text(m_log, &start, &end, false);
    const QString result = QString::fromUtf8(text);
    g_free(text);
    return result;
}
