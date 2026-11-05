// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/vmpage.h"

#include <QTimer>

#include <cstring>

#include "core/gpucontexts.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/vmconsole.h"
#include "gtk/widgets.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

/*
 * Keys the host takes for itself, pressed in the VM, as the Qt app's Send
 * Key menu has them (virt-manager's): the desktop's (Ctrl+Alt+Del), the
 * host's terminals (Ctrl+Alt+F1 to F12, which the host's compositor
 * switches before any window sees them, grab or not), X's old
 * Ctrl+Alt+Backspace and Print Screen.  QEMU's key names (QMP's send-key):
 * any running VM, whatever shows its screen.
 */
static GMenuModel *sendKeyMenu()
{
    GMenu *menu = g_menu_new();
    GMenu *desktop = g_menu_new();
    GMenu *terminals = g_menu_new();
    GMenu *other = g_menu_new();
    const auto add = [](GMenu *to, const QByteArray &label, const QByteArray &keys) {
        GMenuItem *item = g_menu_item_new(label.constData(), nullptr);
        g_menu_item_set_action_and_target_value(item, "win.send-keys",
                                                g_variant_new_string(keys.constData()));
        g_menu_append_item(to, item);
        g_object_unref(item);
    };

    add(desktop, "Ctrl+Alt+_Del", "ctrl alt delete");
    add(desktop, "Ctrl+Alt+_Backspace", "ctrl alt backspace");
    for (int n = 1; n <= 12; n++) {
        const QByteArray number = QByteArray::number(n);
        add(terminals,
            n <= 9 ? "Ctrl+Alt+F_" + number : n == 10 ? QByteArray("Ctrl+Alt+F1_0")
                                                      : "Ctrl+Alt+F" + number,
            "ctrl alt f" + number);
    }
    /* the PrintScreen key: QEMU's "print", the guest's KEY_SYSRQ (E0 37 on PS/2) */
    add(other, "_Print Screen", "print");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(desktop));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(terminals));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(other));
    g_object_unref(desktop);
    g_object_unref(terminals);
    g_object_unref(other);
    return G_MENU_MODEL(menu);
}

/*
 * The console's part of the machine menu: the nested desktop's views (only
 * for a VM that has them: hidden otherwise), full screen, the keys and
 * Release Input.  Ctrl+Alt+1/2 go by the key's place, not its character
 * (MainWindow): the menu shows them, as AZERTY gives & and é there.
 */
static void appendConsoleSections(GMenu *menu)
{
    GMenu *views = g_menu_new();
    GMenu *screen = g_menu_new();
    const struct {
        const char *label, *view, *accel;
    } entries[] = {{"Sc_reen", "screen", "<Control><Alt>1"}, {"D_esktop", "desktop", "<Control><Alt>2"}};

    for (const auto &entry : entries) {
        GMenuItem *item = g_menu_item_new(entry.label, nullptr);
        g_menu_item_set_action_and_target_value(item, "win.view", g_variant_new_string(entry.view));
        g_menu_item_set_attribute(item, "accel", "s", entry.accel);
        g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
        g_menu_append_item(views, item);
        g_object_unref(item);
    }
    g_menu_append(screen, "_Full Screen", "win.full-screen");
    GMenuModel *keys = sendKeyMenu();
    g_menu_append_submenu(screen, "Send _Key", keys);
    g_object_unref(keys);
    g_menu_append(screen, "Release _Input", "win.release-input");
    g_menu_append(screen, "_USB Devices…", "win.usb-devices");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(views));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(screen));
    g_object_unref(views);
    g_object_unref(screen);
}

/* The menu of the VM's other actions, in the header bar: the window's actions */
static GMenuModel *machineMenu()
{
    GMenu *menu = g_menu_new();
    GMenu *power = g_menu_new();
    GMenu *files = g_menu_new();
    GMenu *vm = g_menu_new();

    appendConsoleSections(menu);

    g_menu_append(power, "_Reset…", "win.reset");
    g_menu_append(power, "_Force Off…", "win.force-off");
    g_menu_append(files, "_Settings", "win.settings");
    g_menu_append(files, "Show _Log", "win.show-log");
    g_menu_append(files, "_Open Folder", "win.open-folder");
    g_menu_append(files, "Show Comma_nd Line", "win.command-line");
    g_menu_append(vm, "Install _Guest Tools…", "win.install-guest-tools");
    g_menu_append(vm, "S_napshots", "win.snapshots");
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

/* Where the keys go, for the screen in sight */
static QString inputText(const VmConsole *console)
{
    if (!console->takesInput()) {
        /* QEMU drops input: the keys go to the window (Resume) */
        return "Paused";
    }
    if (console->desktopShown()) {
        return console->hasKeyboard() ? "Keys go to the guest's desktop"
                                      : "Click the desktop to type in the VM";
    }
    if (console->grabbed()) {
        return "The VM has the keyboard · Ctrl+Alt+G releases";
    }
    return console->hasKeyboard() ? "Keys go to the VM · Ctrl+Alt+G grabs"
                                  : "Click the screen to type in the VM";
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
    QObject::connect(m_noticeTimer, &QTimer::timeout, &m_qt, [this]() {
        m_noticeShown = false;
        placeNotice();
    });
    /* the notice over the screen, or in the banner, as the views change; the
       Console view's screen takes the keys */
    Gtk4::connect<void(GParamSpec *)>(m_views, "notify::visible-child-name", [this](GParamSpec *) {
        placeNotice();
        if (view() == View::Console && consoleShown) {
            consoleShown();
        }
    });
}

VmPage::~VmPage()
{
    gtk_widget_unparent(m_toast);
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
    /* while the screen shows, as GNOME Boxes has them: full screen and the keys */
    m_fullScreen = iconButton("view-fullscreen-symbolic",
                              "Full Screen: the VM's screen alone, on the whole monitor "
                              "(Ctrl+Alt+F)",
                              "win.full-screen");
    m_sendKey = gtk_menu_button_new();
    {
        /* the keys' sections, and Release Input after them (a section in a
           section would lose their separators) */
        GMenuModel *keys = sendKeyMenu();
        GMenu *release = g_menu_new();
        g_menu_append(release, "Release _Input", "win.release-input");
        g_menu_append_section(G_MENU(keys), nullptr, G_MENU_MODEL(release));
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(m_sendKey), keys);
        g_object_unref(release);
        g_object_unref(keys);
    }
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_sendKey), "input-keyboard-symbolic");
    gtk_widget_set_tooltip_text(m_sendKey, "Send Key");
    gtk_widget_set_visible(m_fullScreen, false);
    gtk_widget_set_visible(m_sendKey, false);

    adw_header_bar_pack_end(m_header, menu);
    adw_header_bar_pack_end(m_header, m_shutDown);
    adw_header_bar_pack_end(m_header, m_pause);
    adw_header_bar_pack_end(m_header, m_start);
    adw_header_bar_pack_end(m_header, m_fullScreen);
    adw_header_bar_pack_end(m_header, m_sendKey);
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
    /* Starting, connecting, no screen, its own window; Try Again for a screen that did not show */
    m_message = ADW_STATUS_PAGE(adw_status_page_new());
    m_retry = gtk_button_new_with_mnemonic("_Try Again");
    gtk_widget_add_css_class(m_retry, "pill");
    gtk_widget_set_halign(m_retry, GTK_ALIGN_CENTER);
    Gtk4::connect<void()>(m_retry, "clicked", [this]() {
        if (m_consoleShown) {
            m_consoleShown->retry();
        }
    });
    adw_status_page_set_child(m_message, m_retry);
    gtk_stack_add_named(m_console, GTK_WIDGET(m_message), "message");
    /*
     * The screen: the consoles' widgets (VmConsole), the selected VM's
     * shown, filling the area; nothing over it, which would resize the
     * guest, but the notices' popover, a surface of its own above it
     */
    {
        m_screen = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(m_screen, true);
        gtk_widget_set_vexpand(m_screen, true);
        gtk_widget_set_name(m_screen, "screen");
        m_consoles = GTK_STACK(gtk_stack_new());
        gtk_stack_set_transition_type(m_consoles, GTK_STACK_TRANSITION_TYPE_NONE);
        gtk_widget_set_hexpand(GTK_WIDGET(m_consoles), true);
        gtk_widget_set_vexpand(GTK_WIDGET(m_consoles), true);
        m_noConsole = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_stack_add_child(m_consoles, m_noConsole);
        gtk_box_append(GTK_BOX(m_screen), GTK_WIDGET(m_consoles));
        gtk_stack_add_named(m_console, m_screen, "screen");

        /* a notice over the screen: no keys taken, not closed by a click elsewhere */
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
        m_toast = gtk_popover_new();
        m_toastLabel = gtk_label_new(nullptr);
        gtk_label_set_wrap(GTK_LABEL(m_toastLabel), true);
        gtk_label_set_max_width_chars(GTK_LABEL(m_toastLabel), 60);
        gtk_label_set_xalign(GTK_LABEL(m_toastLabel), 0);
        gtk_widget_set_margin_start(m_toastLabel, 6);
        gtk_widget_add_css_class(close, "flat");
        gtk_widget_add_css_class(close, "circular");
        gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(close, "Close");
        Gtk4::connect<void()>(close, "clicked", [this]() {
            m_noticeTimer->stop();
            m_noticeShown = false;
            placeNotice();
        });
        gtk_box_append(GTK_BOX(row), m_toastLabel);
        gtk_box_append(GTK_BOX(row), close);
        gtk_popover_set_child(GTK_POPOVER(m_toast), row);
        gtk_popover_set_autohide(GTK_POPOVER(m_toast), false);
        gtk_popover_set_has_arrow(GTK_POPOVER(m_toast), false);
        gtk_popover_set_position(GTK_POPOVER(m_toast), GTK_POS_BOTTOM);
        /* the guest keeps the keys: nothing in it takes the focus */
        gtk_widget_set_can_focus(m_toast, false);
        gtk_widget_set_parent(m_toast, m_screen);
    }
    return GTK_WIDGET(m_console);
}

void VmPage::addConsole(VmConsole *console)
{
    gtk_stack_add_child(m_consoles, console->widget());
}

void VmPage::removeConsole(VmConsole *console)
{
    if (console == m_consoleShown) {
        setConsole(nullptr);
    }
    if (gtk_widget_get_parent(console->widget()) == GTK_WIDGET(m_consoles)) {
        gtk_stack_remove(m_consoles, console->widget());
    }
}

void VmPage::setConsole(VmConsole *console)
{
    if (console != m_consoleShown) {
        /* what covers the page covers the console in sight */
        if (m_consoleShown && m_covered) {
            m_consoleShown->setCovered(false);
        }
        m_consoleShown = console;
        if (console && m_covered) {
            console->setCovered(true);
        }
        gtk_stack_set_visible_child(m_consoles, console ? console->widget() : m_noConsole);
    }
    updateConsole();
}

bool VmPage::screenShown() const
{
    const char *view = adw_view_stack_get_visible_child_name(m_views);
    const char *page = gtk_stack_get_visible_child_name(m_console);

    return view && strcmp(view, "console") == 0 && page && strcmp(page, "screen") == 0 &&
           gtk_widget_get_mapped(m_screen);
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
    m_noticeText = text;
    m_noticeShown = true;
    adw_banner_set_title(m_notice, utf8(text.toHtmlEscaped()).constData());
    gtk_label_set_text(GTK_LABEL(m_toastLabel), utf8(text).constData());
    m_noticeTimer->start(timeoutMs);
    placeNotice();
}

void VmPage::clearNotice(const QString &text)
{
    if (m_noticeShown && m_noticeText == text) {
        m_noticeTimer->stop();
        m_noticeShown = false;
        placeNotice();
    }
}

/*
 * A banner over the views would resize the guest's screen as it comes and
 * as it goes: over the screen, the notice is a popover instead, a window
 * of the compositor's above the screen's surface, which takes no keys
 */
void VmPage::placeNotice()
{
    const bool shown = screenShown();
    const bool over = m_noticeShown && shown;

    /* at once, before the screen is laid out: what hides over it goes first */
    if (shown != m_screenWasShown) {
        m_screenWasShown = shown;
        if (screenShownChanged) {
            screenShownChanged(shown);
        }
    }

    adw_banner_set_revealed(m_notice, m_noticeShown && !over);
    if (!over) {
        gtk_popover_popdown(GTK_POPOVER(m_toast));
        return;
    }
    /* at the top of the screen, in the middle */
    const GdkRectangle at = {gtk_widget_get_width(m_screen) / 2, 12, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(m_toast), &at);
    gtk_popover_popup(GTK_POPOVER(m_toast));
}

void VmPage::setClosing(const QString &text)
{
    if (!text.isEmpty()) {
        adw_banner_set_title(m_closing, utf8(text.toHtmlEscaped()).constData());
    }
    adw_banner_set_revealed(m_closing, !text.isEmpty());
}

void VmPage::addHomeWidget(GtkWidget *widget)
{
    gtk_box_insert_child_after(GTK_BOX(gtk_widget_get_parent(m_homeError)), widget, m_homeError);
}

void VmPage::addDetailsGroup(AdwPreferencesGroup *group)
{
    /* after the last run's error; the sections come after it, and stay there as they change */
    adw_preferences_page_add(m_details, group);
}

void VmPage::setCovered(bool covered)
{
    m_covered = covered;
    /* the console in sight: a still of the screen under the dialog */
    if (m_consoleShown) {
        m_consoleShown->setCovered(covered);
    }
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
        gtk_widget_set_visible(m_fullScreen, false);
        gtk_widget_set_visible(m_sendKey, false);
        showConsolePage("home");
        placeNotice();
        return;
    }
    const VmRunner *runner = m_vm->runner();
    const VmRunner::State state = runner->state();
    /* the console's page, which its VM's run decides (VmConsole); none, the VM is off */
    const VmConsole *console =
        m_consoleShown && m_consoleShown->vm() == m_vm ? m_consoleShown : nullptr;
    const VmConsole::Page page = console ? console->page() : VmConsole::Page::Home;

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
    gtk_widget_set_visible(m_fullScreen, page == VmConsole::Page::Screen);
    gtk_widget_set_visible(m_sendKey, page == VmConsole::Page::Screen &&
                                          state != VmRunner::State::Stopped);
    /* where the keys go, as the Qt app's status bar says it */
    gtk_widget_set_tooltip_text(
        m_sendKey, utf8(page == VmConsole::Page::Screen ? "Send Key\n" + inputText(console)
                                                         : QString("Send Key"))
                       .constData());

    switch (page) {
    case VmConsole::Page::Home:
        /* off, or on its way: its screen with it */
        showConsolePage("home");
        break;
    case VmConsole::Page::Screen:
        showConsolePage("screen");
        break;
    case VmConsole::Page::Message:
        if (console->isWaiting()) {
            /* the page's own frame clock turns it */
            AdwSpinnerPaintable *spinner = adw_spinner_paintable_new(GTK_WIDGET(m_message));
            adw_status_page_set_paintable(m_message, GDK_PAINTABLE(spinner));
            g_object_unref(spinner);
            adw_status_page_set_icon_name(m_message, nullptr);
        } else {
            adw_status_page_set_paintable(m_message, nullptr);
            adw_status_page_set_icon_name(m_message, console->canRetry() ? "dialog-warning-symbolic"
                                                                         : "computer-symbolic");
        }
        adw_status_page_set_title(m_message, utf8(console->messageTitle()).constData());
        adw_status_page_set_description(
            m_message, console->messageText().isEmpty()
                           ? nullptr
                           : utf8(console->messageText().toHtmlEscaped()).constData());
        gtk_widget_set_visible(m_retry, console->canRetry());
        showConsolePage("message");
        break;
    case VmConsole::Page::OwnWindow:
        adw_status_page_set_paintable(m_message, nullptr);
        adw_status_page_set_icon_name(m_message, "window-new-symbolic");
        adw_status_page_set_title(m_message, "Shown in Its Own Window");
        adw_status_page_set_description(
            m_message, "QEMU shows this VM in a window of its own, which stays open without "
                       "Vitrine.");
        gtk_widget_set_visible(m_retry, false);
        showConsolePage("message");
        break;
    }
    placeNotice();
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
