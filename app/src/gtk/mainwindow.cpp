// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/mainwindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QTimer>

#include <algorithm>

#include "core/hostsettings.h"
#include "core/nesteddesktop.h"
#include "core/paths.h"
#include "core/tasks.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmstore.h"
#include "gtk/gtkloop.h"
#include "gtk/vmpage.h"
#include "gtk/vmsummary.h"
#include "gtk/widgets.h"
#include "ui/usbaccess.h"

using FirmwareFiles::File;
using Gtk4::utf8;

static constexpr char kAppId[] = "org.vitrine.Vitrine";

/* The primary menu: the app's, in the sidebar's header bar and the empty state's */
static GMenuModel *primaryMenu()
{
    GMenu *menu = g_menu_new();
    GMenu *vms = g_menu_new();
    GMenu *app = g_menu_new();
    GMenu *about = g_menu_new();

    g_menu_append(vms, "_New VM…", "win.new-vm");
    g_menu_append(vms, "_Import VM…", "win.import-vm");
    g_menu_append(app, "_Builds", "app.builds");
    g_menu_append(app, "_Preferences", "app.preferences");
    g_menu_append(about, "_About Vitrine", "app.about");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(vms));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(app));
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(about));
    g_object_unref(vms);
    g_object_unref(app);
    g_object_unref(about);
    return G_MENU_MODEL(menu);
}

/* A VM's menu in the sidebar, as the Qt app's list has it */
static GMenuModel *itemMenu()
{
    GMenu *menu = g_menu_new();
    GMenu *power = g_menu_new();
    GMenu *files = g_menu_new();
    GMenu *vm = g_menu_new();

    g_menu_append(power, "S_tart", "win.start");
    g_menu_append(power, "_Pause or Resume", "win.pause");
    g_menu_append(power, "Shut _Down", "win.shut-down");
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

static GtkWidget *primaryMenuButton()
{
    GtkWidget *button = gtk_menu_button_new();
    GMenuModel *menu = primaryMenu();

    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), menu);
    /* F10 opens it */
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(button), true);
    gtk_widget_set_tooltip_text(button, "Main Menu");
    g_object_unref(menu);
    return button;
}

/* The firmware copies of @vm whose damage the error of a failed start names */
static QList<File> firmwareNamed(const Vm *vm, const QString &error)
{
    QList<File> list;

    /* a lock names the file too, but a file in use is not damaged */
    if (FirmwareFiles::inUse(error)) {
        return list;
    }
    for (const File &f : FirmwareFiles::list(vm->args(), vm->dir())) {
        if (!f.templatePath.isEmpty() && error.contains(f.name())) {
            list << f;
        }
    }
    return list;
}

/* What putting new copies in the place of @files does */
static QString firmwareConsequences(const QList<File> &files)
{
    bool vars = false, snapshots = false;
    QStringList text;

    for (const File &f : files) {
        vars |= f.role == File::Role::Vars;
        snapshots |= f.role == File::Role::Vars && FirmwareFiles::snapshotCount(f.path) > 0;
    }
    text << (vars ? QString("The UEFI variables become those of a new VM: the boot entries, the "
                            "boot order and the Secure Boot keys enrolled since are gone. Most "
                            "systems still start, from the fallback boot loader they install.")
                  : QString("Nothing of the VM is lost: the firmware code holds no settings."));
    text << "The old files stay in the VM folder, renamed to end in .bak.";
    if (snapshots) {
        text << "The snapshots keep the variables they were taken with, unless the old file can "
                "no longer be read.";
    }
    return text.join("\n\n");
}

MainWindow::MainWindow(GtkLoop *loop, VmStore *store)
    : m_loop(loop), m_store(store), m_qt(std::make_unique<QObject>()),
      m_page(std::make_unique<VmPage>())
{
    buildWindow();
    createActions();

    /* the host's settings while VMs run (vitrine-helper), what it could not apply said */
    auto *host = new HostSettings(store, m_qt.get());
    QObject::connect(host, &HostSettings::notice, m_qt.get(),
                     [this](const QString &text) { notify(text, 15000); });

    QObject::connect(store, &VmStore::added, m_qt.get(), [this](Vm *vm) { addVm(vm); });
    QObject::connect(store, &VmStore::removed, m_qt.get(),
                     [this](const QString &id) { removeItem(id); });
    for (Vm *vm : store->vms()) {
        addVm(vm);
    }

    const QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    select(settings.value("gtk/current").toString());
    if (!current() && !m_items.isEmpty()) {
        select(m_items.first().id);
    }
    m_page->setView(settings.value("gtk/view").toString() == "details" ? VmPage::View::Details
                                                                         : VmPage::View::Console);
    currentChanged();
}

MainWindow::~MainWindow()
{
    if (GtkWidget *window = m_window) {
        g_signal_handlers_disconnect_by_data(window, this);
        /* nothing follows the window's signals any more */
        closed();
        gtk_window_destroy(GTK_WINDOW(window));
    }
}

void MainWindow::buildWindow()
{
    const QSettings settings(Paths::settingsPath(), QSettings::IniFormat);

    m_window = adw_application_window_new(GTK_APPLICATION(m_loop->adw()));
    gtk_window_set_title(GTK_WINDOW(m_window), "Vitrine");
    gtk_window_set_icon_name(GTK_WINDOW(m_window), kAppId);
    /* the narrowest GNOME asks of an adaptive window */
    gtk_widget_set_size_request(m_window, 360, 294);
    gtk_window_set_default_size(GTK_WINDOW(m_window), settings.value("gtk/width", 1060).toInt(),
                                settings.value("gtk/height", 680).toInt());
    if (settings.value("gtk/maximized", false).toBool()) {
        gtk_window_maximize(GTK_WINDOW(m_window));
    }

    /* The sidebar: the VMs, under the primary menu and New VM */
    GtkWidget *sidebarView = adw_toolbar_view_new();
    {
        GtkWidget *header = adw_header_bar_new();
        GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
        GMenuModel *menu = itemMenu();

        gtk_widget_set_tooltip_text(add, "New VM (Ctrl+N)");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(add), "win.new-vm");
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), add);
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), primaryMenuButton());
        adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new("Vitrine", ""));
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebarView), header);

        m_sidebar = ADW_SIDEBAR(adw_sidebar_new());
        m_section = adw_sidebar_section_new();
        adw_sidebar_append(m_sidebar, m_section);
        adw_sidebar_set_menu_model(m_sidebar, menu);
        g_object_unref(menu);
        Gtk4::connect<void(GParamSpec *)>(m_sidebar, "notify::selected", [this](GParamSpec *) {
            if (m_window && !m_syncing) {
                currentChanged();
            }
        });
        /* the VM's page in front: over a narrow window's sidebar */
        Gtk4::connect<void(guint)>(m_sidebar, "activated", [this](guint) {
            if (m_split && adw_overlay_split_view_get_collapsed(m_split)) {
                adw_overlay_split_view_set_show_sidebar(m_split, false);
            }
        });
        /* the VM a menu opens on is the one its actions act on */
        Gtk4::connect<void(AdwSidebarItem *)>(m_sidebar, "setup-menu", [this](AdwSidebarItem *item) {
            for (const Item &each : std::as_const(m_items)) {
                if (each.item == item) {
                    select(each.id);
                }
            }
        });
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sidebarView), GTK_WIDGET(m_sidebar));
    }

    /* The VM's page, and the button that shows and hides the sidebar */
    {
        GtkWidget *toggle = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(toggle), "sidebar-show-symbolic");
        gtk_widget_set_tooltip_text(toggle, "Show the List of VMs (F9)");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(toggle), "win.show-sidebar");
        adw_header_bar_pack_start(m_page->headerBar(), toggle);
    }
    m_split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar(m_split, sidebarView);
    adw_overlay_split_view_set_content(m_split, m_page->widget());
    adw_overlay_split_view_set_min_sidebar_width(m_split, 220);
    adw_overlay_split_view_set_max_sidebar_width(m_split, 300);
    adw_overlay_split_view_set_show_sidebar(m_split, settings.value("gtk/sidebar", true).toBool());

    /* No VMs yet */
    GtkWidget *emptyView = adw_toolbar_view_new();
    {
        GtkWidget *header = adw_header_bar_new();
        GtkWidget *page = adw_status_page_new();
        GtkWidget *create = gtk_button_new_with_mnemonic("_New VM…");

        adw_header_bar_pack_end(ADW_HEADER_BAR(header), primaryMenuButton());
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(emptyView), header);
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), kAppId);
        adw_status_page_set_title(ADW_STATUS_PAGE(page), "No Virtual Machines");
        adw_status_page_set_description(ADW_STATUS_PAGE(page), "Create one to get started.");
        gtk_widget_add_css_class(create, "pill");
        gtk_widget_add_css_class(create, "suggested-action");
        gtk_widget_set_halign(create, GTK_ALIGN_CENTER);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(create), "win.new-vm");
        adw_status_page_set_child(ADW_STATUS_PAGE(page), create);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(emptyView), page);
    }

    m_main = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(m_main, emptyView, "empty");
    gtk_stack_add_named(m_main, GTK_WIDGET(m_split), "vms");
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(m_window), GTK_WIDGET(m_main));

    /* narrow: the sidebar over the page, which takes the whole window */
    AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 500sp"));
    adw_breakpoint_add_setters(narrow, G_OBJECT(m_split), "collapsed", true, nullptr);
    m_page->addNarrowSetters(narrow);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(m_window), narrow);

    Gtk4::connect<gboolean()>(m_window, "close-request", [this]() -> gboolean {
        return closeRequested();
    });
    g_signal_connect_swapped(m_window, "destroy",
                             G_CALLBACK(+[](MainWindow *self) { self->closed(); }), this);
}

GSimpleAction *MainWindow::addAction(GActionMap *map, const char *name,
                                     std::function<void()> activated, const char *accel)
{
    GSimpleAction *action = g_simple_action_new(name, nullptr);

    Gtk4::connect<void(GVariant *)>(action, "activate",
                                    [activated = std::move(activated)](GVariant *) { activated(); });
    g_action_map_add_action(map, G_ACTION(action));
    g_object_unref(action);
    if (accel) {
        const char *accels[] = {accel, nullptr};
        const QByteArray detailed =
            QByteArray(G_IS_APPLICATION(map) ? "app." : "win.") + name;
        gtk_application_set_accels_for_action(GTK_APPLICATION(m_loop->adw()),
                                              detailed.constData(), accels);
    }
    m_actions.insert(name, action);
    return action;
}

void MainWindow::setEnabled(const char *name, bool enabled)
{
    if (GSimpleAction *action = m_actions.value(name)) {
        g_simple_action_set_enabled(action, enabled);
    }
}

void MainWindow::createActions()
{
    auto *win = G_ACTION_MAP(m_window);
    auto *app = G_ACTION_MAP(m_loop->adw());

    addAction(app, "about", [this]() { about(); });
    /* Close, with its questions: the app ends with its window */
    addAction(app, "quit", [this]() {
        if (m_window) {
            gtk_window_close(GTK_WINDOW(m_window));
        }
    }, "<Control>q");
    /* their windows are not in the GTK app yet */
    addAction(app, "builds", [] {}, "<Control>b");
    addAction(app, "preferences", [] {}, "<Control>comma");
    addAction(win, "new-vm", [] {}, "<Control>n");
    addAction(win, "import-vm", [] {}, "<Control>i");
    addAction(win, "settings", [] {}, "<Control>s");
    addAction(win, "clone", [] {});
    addAction(win, "command-line", [] {});
    for (const char *name :
         {"builds", "preferences", "new-vm", "import-vm", "settings", "clone", "command-line"}) {
        setEnabled(name, false);
    }

    /* the sidebar shown or hidden: the split view's own state */
    GPropertyAction *sidebar = g_property_action_new("show-sidebar", m_split, "show-sidebar");
    g_action_map_add_action(win, G_ACTION(sidebar));
    g_object_unref(sidebar);
    {
        const char *accels[] = {"F9", nullptr};
        gtk_application_set_accels_for_action(GTK_APPLICATION(m_loop->adw()), "win.show-sidebar",
                                              accels);
    }

    addAction(win, "start", [this]() { start(); }, "<Control>Return");
    addAction(win, "pause", [this]() { togglePause(); }, "<Control>p");
    addAction(win, "shut-down", [this]() { shutDown(); }, "<Control>h");
    addAction(win, "reset", [this]() { reset(); });
    addAction(win, "force-off", [this]() { forceOff(); });
    addAction(win, "remove", [this]() { remove(); });
    addAction(win, "show-log", [this]() { showLog(); }, "<Control>l");
    addAction(win, "open-folder", [this]() { openFolder(); });
}

GtkWindow *MainWindow::window() const
{
    return m_window ? GTK_WINDOW(m_window) : nullptr;
}

void MainWindow::present(const QByteArray &token)
{
    if (!m_window) {
        return;
    }
    if (!token.isEmpty()) {
        gtk_window_set_startup_id(GTK_WINDOW(m_window), token.constData());
    }
    gtk_window_present(GTK_WINDOW(m_window));
}

void MainWindow::present(AdwDialog *dialog)
{
    if (!m_window) {
        g_object_ref_sink(dialog);
        g_object_unref(dialog);
        return;
    }
    if (m_covering++ == 0) {
        m_page->setCovered(true);
    }
    Gtk4::connect<void()>(dialog, "closed", [this]() {
        if (--m_covering == 0) {
            m_page->setCovered(false);
        }
    });
    adw_dialog_present(dialog, m_window);
}

void MainWindow::notify(const QString &text, int timeoutMs)
{
    if (m_window) {
        m_page->notify(text, timeoutMs);
    }
}

Vm *MainWindow::current() const
{
    if (!m_sidebar) {
        return nullptr;
    }
    const guint index = adw_sidebar_get_selected(m_sidebar);
    return index < guint(m_items.size()) ? m_store->find(m_items[index].id) : nullptr;
}

int MainWindow::indexOf(const QString &id) const
{
    for (int i = 0; i < m_items.size(); i++) {
        if (m_items[i].id == id) {
            return i;
        }
    }
    return -1;
}

void MainWindow::select(const QString &id)
{
    if (const int index = indexOf(id); index >= 0 && m_sidebar) {
        adw_sidebar_set_selected(m_sidebar, guint(index));
    }
}

void MainWindow::insertItem(Vm *vm)
{
    const Vm *selected = current();
    const QString selectedId = selected ? selected->id() : QString();
    AdwSidebarItem *item = adw_sidebar_item_new(utf8(vm->name()).constData());
    int at = 0;

    adw_sidebar_item_set_icon_name(item, "computer-symbolic");
    /* by name, as the store lists them */
    while (at < m_items.size()) {
        const Vm *other = m_store->find(m_items[at].id);
        if (other && QString::compare(vm->name(), other->name(), Qt::CaseInsensitive) < 0) {
            break;
        }
        at++;
    }
    m_syncing = true;
    m_items.insert(at, Item{vm->id(), item});
    /* the section takes the item */
    adw_sidebar_section_insert(m_section, item, at);
    m_syncing = false;
    select(selectedId);
}

void MainWindow::addVm(Vm *vm)
{
    const QString id = vm->id();

    insertItem(vm);

    QObject::connect(vm, &Vm::changed, m_qt.get(), [this, vm]() {
        updateItem(vm);
        if (vm == current()) {
            m_page->refresh();
        }
    });
    QObject::connect(vm->runner(), &VmRunner::stateChanged, m_qt.get(),
                     [this, vm](VmRunner::State state) { stateChanged(vm, state); });
    QObject::connect(vm->runner(), &VmRunner::sharesMounted, m_qt.get(),
                     [this, vm](const QStringList &mounted, const QStringList &problems) {
        if (!mounted.isEmpty()) {
            notify(QString("%1 mounted its shared folders: %2").arg(vm->name(), mounted.join(", ")),
                   10000);
        }
        if (!problems.isEmpty()) {
            present(Gtk4::alert("Shared Folders",
                                QString("%1 could not mount some of its shared folders:\n\n%2")
                                    .arg(vm->name(), problems.join('\n'))));
        }
    });
    QObject::connect(vm->runner(), &VmRunner::failed, m_qt.get(),
                     [this, vm](const QString &error) { failed(vm, error); });
    /* QEMU that does not end after Force Off: killed only if the user says so */
    QObject::connect(vm->runner(), &VmRunner::notResponding, m_qt.get(),
                     [this, vm]() { askKill(vm); });
    /* how Shut Down reaches the guest, and when the guest may ask on its screen */
    QObject::connect(vm->runner(), &VmRunner::shutdownWayChanged, m_qt.get(),
                     [this, vm](GuestShutdown::Way way) {
        if (way != GuestShutdown::Way::None) {
            notify(VmSummary::shutdownText(vm->name(), way), VmSummary::shutdownTimeout(way));
            return;
        }
        /* over: what was said of this request goes, not what came after it */
        for (const auto said : {GuestShutdown::Way::ToolsAgent, GuestShutdown::Way::GuestAgent,
                                GuestShutdown::Way::PowerButton}) {
            m_page->clearNotice(VmSummary::shutdownText(vm->name(), said));
        }
    });
    /*
     * No screen in this build of the GTK app (gtk-vmview's VmDisplay is
     * to come): a VM shown in vitrine's window runs at once instead of
     * waiting a few seconds for a screen, as the Qt app's console has it
     * when it cannot show the screen
     */
    QObject::connect(vm->runner(), &VmRunner::waitsForDisplayChanged, m_qt.get(),
                     [vm](bool waiting) {
        if (waiting) {
            vm->runner()->displayReady();
        }
    });
    /* nor the nested desktop in its window: such a VM says so, and does not start */
    vm->runner()->setNestedHost([](const QString &, QString *error) {
        if (error) {
            *error = "The GTK app does not show nested desktops yet: start this VM from "
                     "Vitrine's Qt app.";
        }
        return false;
    });
    m_states[id] = vm->runner()->state();
    updateItem(vm);
    if (m_items.size() == 1) {
        select(id);
    }
    currentChanged();
}

void MainWindow::removeItem(const QString &id)
{
    const int index = indexOf(id);
    const Vm *selected = current();
    const QString selectedId = selected ? selected->id() : QString();

    m_errors.remove(id);
    m_states.remove(id);
    m_endedFrom.remove(id);
    m_starting.remove(id);
    m_askingUsb.remove(id);
    if (index < 0) {
        return;
    }
    if (m_page->vm() && m_page->vm()->id() == id) {
        m_page->setVm(nullptr);
    }
    if (AdwDialog *prompt = m_killPrompts.take(id)) {
        adw_dialog_force_close(prompt);
    }
    m_syncing = true;
    adw_sidebar_section_remove(m_section, m_items[index].item);
    m_items.removeAt(index);
    m_syncing = false;
    if (selectedId != id) {
        select(selectedId);
    } else if (!m_items.isEmpty()) {
        select(m_items[std::min<int>(index, int(m_items.size()) - 1)].id);
    }
    currentChanged();
}

void MainWindow::updateItem(Vm *vm)
{
    const int index = indexOf(vm->id());
    const VmRunner::State state = vm->runner()->state();
    GtkWidget *suffix = nullptr;
    QString text;

    if (index < 0) {
        return;
    }
    AdwSidebarItem *item = m_items[index].item;
    if (m_errors.contains(vm->id()) && state == VmRunner::State::Stopped) {
        text = "Stopped with an error";
        suffix = gtk_image_new_from_icon_name("dialog-warning-symbolic");
        gtk_widget_add_css_class(suffix, "error");
    } else {
        text = VmSummary::stateText(vm);
        switch (state) {
        case VmRunner::State::Running:
            suffix = gtk_image_new_from_icon_name("media-playback-start-symbolic");
            gtk_widget_add_css_class(suffix, "success");
            break;
        case VmRunner::State::Paused:
            suffix = gtk_image_new_from_icon_name("media-playback-pause-symbolic");
            gtk_widget_add_css_class(suffix, "warning");
            break;
        case VmRunner::State::Starting:
        case VmRunner::State::Stopping:
            suffix = adw_spinner_new();
            break;
        case VmRunner::State::Stopped:
            break;
        }
    }
    if (suffix) {
        gtk_widget_set_tooltip_text(suffix, utf8(text).constData());
    }
    adw_sidebar_item_set_subtitle(item, utf8(text).constData());
    adw_sidebar_item_set_suffix(item, suffix);
    adw_sidebar_item_set_tooltip(item, utf8(vm->name() + " — " + text).constData());
    if (QString::fromUtf8(adw_sidebar_item_get_title(item)) != vm->name()) {
        /* renamed: in its place by name again, selected if it was */
        const bool selected = current() == vm;
        m_syncing = true;
        adw_sidebar_section_remove(m_section, item);
        m_items.removeAt(index);
        m_syncing = false;
        insertItem(vm);
        if (selected) {
            select(vm->id());
        }
        updateItem(vm);
    }
}

void MainWindow::stateChanged(Vm *vm, VmRunner::State state)
{
    const QString id = vm->id();

    if (state == VmRunner::State::Stopped) {
        /* QEMU ended by itself: nothing to decide */
        if (AdwDialog *prompt = m_killPrompts.take(id)) {
            adw_dialog_force_close(prompt);
        }
        /* for failed(), which follows */
        m_endedFrom[id] = m_states.value(id);
        m_shutDownOnceUp.remove(id);
        /* the last of those the closing window waited for */
        if (m_closeAfter.remove(id) && m_closeAfter.isEmpty()) {
            QTimer::singleShot(0, m_qt.get(), [this]() {
                if (m_window) {
                    gtk_window_close(GTK_WINDOW(m_window));
                }
            });
        }
    } else {
        /* a new run, started here or found running (attach) */
        m_errors.remove(id);
        if (state != VmRunner::State::Starting) {
            m_starting.remove(id);
        }
        /* the closing window waits for it to shut down, but it was starting
           when asked, without QMP to ask it through: now */
        if (m_shutDownOnceUp.contains(id) && state == VmRunner::State::Paused) {
            vm->runner()->resume();
        } else if (state == VmRunner::State::Running && m_shutDownOnceUp.remove(id)) {
            vm->runner()->powerdown();
        }
    }
    m_states[id] = state;
    updateItem(vm);
    updateWaiting();
    if (vm == current()) {
        m_page->setError(m_errors.value(id));
        m_page->refresh();
        updateActions();
    }
}

void MainWindow::failed(Vm *vm, const QString &error)
{
    const QString id = vm->id();
    const VmRunner::State state = vm->runner()->state();
    QList<File> firmware;
    /* start() refused it before QEMU ran */
    bool refused = false;
    QString title, text;

    if (state != VmRunner::State::Stopped) {
        /* QEMU refused a command: the VM runs on */
        title = vm->name();
        text = "QEMU refused the command.";
    } else {
        const VmRunner::State from = m_endedFrom.value(id);

        m_errors[id] = error;
        updateItem(vm);
        if (vm == current()) {
            m_page->setError(error);
            updateActions();
        }
        if (m_starting.remove(id)) {
            title = QString("Cannot Start %1").arg(vm->name());
            text = QString("%1 could not start.").arg(vm->name());
            firmware = firmwareNamed(vm, error);
            /* start() refused it, to wait for Vitrine's QEMU, or for its GPU */
            refused = VmRunner::needsQemuBuild(vm->runner()->runArgs()) ||
                      !VmRunner::renderNodeProblem(vm->runner()->runArgs()).isEmpty();
        } else if (from != VmRunner::State::Stopped) {
            title = QString("%1 Stopped").arg(vm->name());
            text = QString("%1 stopped unexpectedly.").arg(vm->name());
        } else {
            title = vm->name();
            text = QString("Cannot connect to %1.").arg(vm->name());
        }
    }

    QList<std::pair<QString, QString>> responses = {{"close", "Close"}};
    /* not for a start refused before QEMU ran: that log is of another run */
    if (!refused && QFileInfo::exists(vm->runner()->logPath())) {
        responses << std::pair(QString("log"), QString("Show _Log"));
    }
    /* QEMU refused a firmware copy */
    bool vars = !firmware.isEmpty();
    for (const File &f : firmware) {
        vars &= f.role == File::Role::Vars;
    }
    if (!firmware.isEmpty()) {
        responses << std::pair(QString("firmware"), vars ? QString("Reset UEFI _Variables…")
                                                         : QString("_Replace Firmware Files…"));
    }
    AdwDialog *dialog = Gtk4::alert(title, text, responses);
    Gtk4::setDetails(dialog, error);
    Gtk4::onResponse(dialog, [this, id, firmware](const QString &response) {
        Vm *vm = m_store->find(id);
        if (!vm) {
            return;
        }
        if (response == "log") {
            Gtk4::openPath(m_window, vm->runner()->logPath());
        } else if (response == "firmware") {
            bool vars = true;
            for (const File &f : firmware) {
                vars &= f.role == File::Role::Vars;
            }
            resetFirmware(vm, firmware, vars ? "_Reset and Start" : "_Replace and Start",
                          [this, id](bool done) {
                if (done) {
                    select(id);
                    start();
                }
            });
        }
    });
    present(dialog);
}

void MainWindow::currentChanged()
{
    Vm *vm = current();

    if (!m_window) {
        return;
    }
    gtk_stack_set_visible_child_name(m_main, m_items.isEmpty() ? "empty" : "vms");
    m_page->setVm(vm, vm ? m_errors.value(vm->id()) : QString());
    updateActions();
}

void MainWindow::updateActions()
{
    const Vm *vm = current();
    const VmRunner::State state = vm ? vm->runner()->state() : VmRunner::State::Stopped;
    const bool paused = state == VmRunner::State::Paused;

    setEnabled("start", vm && state == VmRunner::State::Stopped);
    setEnabled("pause", vm && (state == VmRunner::State::Running || paused));
    setEnabled("shut-down", vm && state == VmRunner::State::Running);
    setEnabled("reset", vm && (state == VmRunner::State::Running || paused));
    setEnabled("force-off", vm && state != VmRunner::State::Stopped);
    setEnabled("remove", vm && state == VmRunner::State::Stopped);
    setEnabled("show-log", vm);
    setEnabled("open-folder", vm);
}

void MainWindow::updateWaiting()
{
    QStringList names;

    if (m_closeAfter.isEmpty()) {
        return;
    }
    for (const QString &id : std::as_const(m_closeAfter)) {
        if (const Vm *vm = m_store->find(id)) {
            names << vm->name();
        }
    }
    names.sort();
    notify(QString("Waiting for %1 to shut down, then closing").arg(names.join(", ")), 600000);
}

void MainWindow::start()
{
    Vm *vm = current();

    if (!vm || m_askingUsb.contains(vm->id())) {
        return;
    }
    /* its disks being copied for a clone: said, before USB access is
       asked and the firmware checked; no error of the VM's */
    if (const QString busy = vm->runner()->busy(); !busy.isEmpty()) {
        present(Gtk4::alert(QString("Cannot Start %1").arg(vm->name()), busy));
        return;
    }
    /* the nested desktop's two cards of before: its one card from this start on */
    if (ArgsFile moved = vm->args(); NestedDesktop::moveToOneCard(moved)) {
        QString error;
        if (!vm->save(moved, &error)) {
            notify(QString("%1 keeps the nested desktop's two cards: %2").arg(vm->name(), error));
        }
    }
    /* it waits for Vitrine's QEMU: start() refuses it, which failed() reports */
    if (VmRunner::needsQemuBuild(vm->args())) {
        m_starting.insert(vm->id());
        vm->runner()->start(vm->args());
        return;
    }
    /* firmware copies a power cut damaged, say: new ones, if the user wants */
    const QString id = vm->id();
    checkFirmware(vm, [this, id]() { startChecked(id); });
}

void MainWindow::startChecked(const QString &id)
{
    Vm *vm = m_store->find(id);

    if (!vm || vm->runner()->isActive() || m_askingUsb.contains(id)) {
        return;
    }
    m_errors.remove(id);
    if (vm == current()) {
        m_page->setError({});
    }
    /* the VM gets its USB devices only if it can open them from the start */
    const QString asking = QString("Checking the access to the USB devices of %1…").arg(vm->name());
    m_askingUsb.insert(id);
    UsbAccess::request(vm, m_qt.get(), [this, id, asking](const QString &warning) {
        Vm *vm = m_store->find(id);

        m_askingUsb.remove(id);
        m_page->clearNotice(asking);
        if (!vm || vm->runner()->isActive()) {
            return;
        }
        m_starting.insert(id);
        vm->runner()->start(vm->args());
        /* its screen comes up on the Console */
        if (vm == current() && VmConfig::screen(vm->args()) == VmConfig::Screen::Embedded) {
            m_page->setView(VmPage::View::Console);
        }
        if (!warning.isEmpty()) {
            present(Gtk4::alert("USB Passthrough", warning));
        }
    });
    if (m_askingUsb.contains(id)) {
        notify(asking, 120000);
    }
}

void MainWindow::togglePause()
{
    if (Vm *vm = current()) {
        if (vm->runner()->state() == VmRunner::State::Paused) {
            vm->runner()->resume();
        } else {
            vm->runner()->pause();
        }
    }
}

void MainWindow::shutDown()
{
    /* the notices say how (shutdownWayChanged) */
    if (Vm *vm = current()) {
        vm->runner()->powerdown();
    }
}

void MainWindow::reset()
{
    Vm *vm = current();

    if (!vm) {
        return;
    }
    const QString id = vm->id();
    AdwDialog *dialog = Gtk4::alert(QString("Reset %1?").arg(vm->name()),
                                    "The VM restarts at once: the guest loses its unsaved work.",
                                    {{"cancel", "_Cancel"}, {"reset", "_Reset"}}, "reset", true);
    Gtk4::onResponse(dialog, [this, id](const QString &response) {
        if (Vm *vm = m_store->find(id); vm && response == "reset") {
            vm->runner()->reset();
        }
    });
    present(dialog);
}

void MainWindow::forceOff()
{
    Vm *vm = current();

    if (!vm) {
        return;
    }
    if (vm->runner()->state() == VmRunner::State::Stopping) {
        /* the guest is off already, or on its way */
        vm->runner()->forceOff();
        return;
    }
    const QString id = vm->id();
    AdwDialog *dialog = Gtk4::alert(
        QString("Force Off %1?").arg(vm->name()),
        "The VM stops at once, as when pulling the plug: the guest loses its unsaved work.",
        {{"cancel", "_Cancel"}, {"force-off", "_Force Off"}}, "force-off", true);
    Gtk4::onResponse(dialog, [this, id](const QString &response) {
        if (Vm *vm = m_store->find(id); vm && response == "force-off") {
            vm->runner()->forceOff();
        }
    });
    present(dialog);
}

void MainWindow::remove()
{
    Vm *vm = current();

    if (!vm || vm->runner()->isActive()) {
        return;
    }
    const QString id = vm->id();
    AdwDialog *dialog = Gtk4::alert(
        QString("Remove %1?").arg(vm->name()),
        QString("The folder of %1, disks included, goes to the trash, where you can still "
                "restore it from.")
            .arg(vm->name()),
        {{"cancel", "_Cancel"}, {"remove", "_Move to Trash"}}, "remove", true);
    Gtk4::onResponse(dialog, [this, id](const QString &response) {
        Vm *vm = m_store->find(id);
        QString error;
        if (vm && response == "remove" && !m_store->remove(vm, &error)) {
            present(Gtk4::alert("Cannot Remove the VM", error));
        }
    });
    present(dialog);
}

void MainWindow::showLog()
{
    /* in the desktop's text editor, until the GTK app has the Logs view */
    if (Vm *vm = current()) {
        if (QFileInfo::exists(vm->runner()->logPath())) {
            Gtk4::openPath(m_window, vm->runner()->logPath());
        } else {
            notify(QString("%1 has no log yet: QEMU writes it as the VM runs.").arg(vm->name()));
        }
    }
}

void MainWindow::openFolder()
{
    if (Vm *vm = current()) {
        Gtk4::openPath(m_window, vm->dir());
    }
}

void MainWindow::about()
{
    AdwDialog *dialog = adw_about_dialog_new();
    auto *about = ADW_ABOUT_DIALOG(dialog);

    adw_about_dialog_set_application_name(about, "Vitrine");
    adw_about_dialog_set_application_icon(about, kAppId);
    adw_about_dialog_set_version(about, VITRINE_VERSION);
    adw_about_dialog_set_developer_name(about, "Written by AI: Anthropic's Claude, through Claude Code");
    adw_about_dialog_set_comments(
        about, "Creates and runs QEMU virtual machines. Each VM is its QEMU command line, which "
               "the settings edit and you can edit as text.");
    adw_about_dialog_set_website(about, "https://github.com/tomtomnet/vitrine");
    adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_2_0);
    /* CC BY 3.0 asks for its credit where the authors are named */
    adw_about_dialog_add_legal_section(
        about, "Logo",
        "QEMU's emu, from \"Kew the Angry Emu\" by Benoît Canet (QEMU Community)",
        GTK_LICENSE_CUSTOM,
        "Licensed under <a href=\"https://creativecommons.org/licenses/by/3.0/\">CC BY 3.0</a>.");
    present(dialog);
}

void MainWindow::checkFirmware(Vm *vm, std::function<void()> go)
{
    QList<File> damaged;
    QStringList lines;

    for (const File &f : FirmwareFiles::list(vm->args(), vm->dir())) {
        /* the start makes the missing ones again; without a template QEMU tells */
        if (!QFileInfo::exists(f.path) || f.templatePath.isEmpty()) {
            continue;
        }
        const QString problem = FirmwareFiles::problem(f);
        if (!problem.isEmpty()) {
            damaged << f;
            lines << QString("%1: %2").arg(f.name(), problem);
        }
    }
    if (damaged.isEmpty()) {
        go();
        return;
    }
    const QString id = vm->id();
    AdwDialog *dialog = Gtk4::alert(
        "Damaged Firmware",
        QString("Firmware files of %1 look damaged:").arg(vm->name()) + "\n\n" + lines.join('\n') +
            "\n\nNew copies can replace them.\n\n" + firmwareConsequences(damaged),
        {{"cancel", "_Cancel"}, {"anyway", "Start _Anyway"}, {"renew", "_Replace and Start"}},
        "renew");
    Gtk4::onResponse(dialog, [this, id, damaged, go](const QString &response) {
        Vm *vm = m_store->find(id);
        if (!vm) {
            return;
        }
        if (response == "anyway") {
            go();
            return;
        }
        if (response != "renew") {
            return;
        }
        const QString qemuImg = FirmwareFiles::qemuImg(vm->args());
        QStringList errors;
        for (const File &f : damaged) {
            QString error;
            if (!FirmwareFiles::reset(f, qemuImg, nullptr, nullptr, &error)) {
                errors << error;
            }
        }
        if (!errors.isEmpty()) {
            present(Gtk4::alert("Firmware", errors.join('\n')));
            return;
        }
        go();
    });
    present(dialog);
}

void MainWindow::resetFirmware(Vm *vm, const QList<File> &files, const QString &accept,
                               std::function<void(bool)> done)
{
    bool vars = false;

    if (files.isEmpty()) {
        done(false);
        return;
    }
    if (vm->runner()->isActive()) {
        present(Gtk4::alert("Firmware", QString("Shut %1 down first.").arg(vm->name())));
        done(false);
        return;
    }
    for (const File &f : files) {
        vars |= f.role == File::Role::Vars;
    }
    const QString id = vm->id();
    AdwDialog *dialog = Gtk4::alert(
        vars ? "Reset UEFI Variables" : "Replace Firmware",
        (vars ? QString("Reset the UEFI variables of %1?").arg(vm->name())
              : QString("Replace the firmware of %1 with a new copy?").arg(vm->name())) +
            "\n\n" + firmwareConsequences(files),
        {{"cancel", "_Cancel"}, {"renew", accept}}, "renew", true);
    Gtk4::onResponse(dialog, [this, id, files, done](const QString &response) {
        Vm *vm = m_store->find(id);
        if (!vm || response != "renew") {
            done(false);
            return;
        }
        const QString qemuImg = FirmwareFiles::qemuImg(vm->args());
        QStringList errors;
        for (const File &f : files) {
            QString error;
            if (!FirmwareFiles::reset(f, qemuImg, nullptr, nullptr, &error)) {
                errors << error;
            }
        }
        if (!errors.isEmpty()) {
            present(Gtk4::alert("Firmware", errors.join('\n')));
        }
        done(errors.isEmpty());
    });
    present(dialog);
}

void MainWindow::askKill(Vm *vm)
{
    const QString id = vm->id();

    if (m_killPrompts.contains(id) || vm->runner()->state() == VmRunner::State::Stopped) {
        return;
    }
    AdwDialog *dialog = Gtk4::alert(
        QString("%1 Does Not Respond").arg(vm->name()),
        QString("The QEMU of %1 does not end: it was asked to quit and is still running. It may "
                "still be writing to the VM's disks.\n\nKill it to end it now: its disks may lose "
                "their last writes, which can damage a qcow2 disk image. Or wait, and use Force "
                "Off again later.")
            .arg(vm->name()),
        {{"wait", "_Wait"}, {"kill", "_Kill QEMU"}});
    /* it comes up by itself: Enter or Escape must not kill anything */
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "kill",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "wait");
    Gtk4::onResponse(dialog, [this, id](const QString &response) {
        if (Vm *vm = m_store->find(id); vm && response == "kill") {
            vm->runner()->killQemu();
        }
    });
    /* answered, or gone as QEMU ended by itself (stateChanged()) */
    Gtk4::connect<void()>(dialog, "closed", [this, id, dialog]() {
        if (m_killPrompts.value(id) == dialog) {
            m_killPrompts.remove(id);
        }
    });
    m_killPrompts.insert(id, dialog);
    present(dialog);
}

bool MainWindow::closeRequested()
{
    if (m_closing) {
        return false;
    }
    /* asked again: what the user answers now goes */
    m_closeAfter.clear();
    m_shutDownOnceUp.clear();

    /*
     * VMs outlive the window, but those shown in it go on without a
     * screen until vitrine shows them again
     */
    QList<QPointer<Vm>> shown;
    QStringList names;
    for (Vm *each : m_store->vms()) {
        if (!each->runner()->displaySocket().isEmpty()) {
            shown << each;
            names << each->name();
        }
    }
    if (shown.isEmpty()) {
        return askTasks();
    }
    AdwDialog *dialog = Gtk4::alert(
        "Close Vitrine",
        (shown.size() == 1 ? QString("%1 is running.").arg(names.first())
                           : QString("%1 VMs are running: %2.")
                                 .arg(shown.size())
                                 .arg(names.join(", "))) +
            "\n\n" +
            (shown.size() == 1
                 ? QString("In the background, it has no screen until vitrine starts again. Shut "
                           "Down asks the guest to shut down, through its agent or the power "
                           "button, and closes the window once it is off.")
                 : QString("In the background, they have no screen until vitrine starts again. "
                           "Shut Down asks the guests to shut down, through their agents or the "
                           "power button, and closes the window once they are off.")),
        {{"cancel", "_Cancel"}, {"shut-down", "_Shut Down"}, {"keep", "_Keep Running in the Background"}},
        "keep");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "shut-down",
                                             ADW_RESPONSE_DESTRUCTIVE);
    Gtk4::onResponse(dialog, [this, shown](const QString &response) {
        if (response == "keep") {
            closeUnlessAsked();
            return;
        }
        if (response != "shut-down") {
            return;
        }
        /* as they are now: one may have stopped while the question was open */
        QString first;
        for (const QPointer<Vm> &each : shown) {
            VmRunner *runner = each ? each->runner() : nullptr;
            if (!runner || !runner->isActive()) {
                continue;
            }
            switch (runner->state()) {
            case VmRunner::State::Paused:
                /* a paused guest would not see the button */
                runner->resume();
                runner->powerdown();
                break;
            case VmRunner::State::Running:
                runner->powerdown();
                break;
            case VmRunner::State::Starting:
                /* no QMP yet: once it runs (stateChanged()) */
                m_shutDownOnceUp.insert(each->id());
                break;
            default:
                /* stopping already */
                break;
            }
            m_closeAfter.insert(each->id());
            if (first.isEmpty()) {
                first = each->id();
            }
        }
        if (!first.isEmpty()) {
            /* the guest may ask first, on its screen: the window stays until they are off */
            select(first);
            m_page->setView(VmPage::View::Console);
            updateWaiting();
            return;
        }
        /* all of them off meanwhile: the window closes now */
        closeUnlessAsked();
    });
    present(dialog);
    return true;
}

bool MainWindow::askTasks()
{
    /* the tasks under way end with the app: a build stops, a clone's copy is removed */
    QStringList tasks;
    for (const Task *task : TaskList::instance()->tasks()) {
        if (task->isRunning()) {
            tasks << task->title();
        }
    }
    if (tasks.isEmpty()) {
        finishClosing();
        m_closing = true;
        return false;
    }
    AdwDialog *dialog = Gtk4::alert(
        "Close Vitrine",
        QString("Closing Vitrine stops what is under way: %1. A clone stopped before its end is "
                "removed.")
            .arg(tasks.join(", ")),
        {{"cancel", "_Cancel"}, {"close", "C_lose Anyway"}}, "close", true);
    Gtk4::onResponse(dialog, [this](const QString &response) {
        if (response == "close" && m_window) {
            finishClosing();
            m_closing = true;
            gtk_window_close(GTK_WINDOW(m_window));
        }
    });
    present(dialog);
    return true;
}

void MainWindow::closeUnlessAsked()
{
    /* out of the close request: a close from within it does nothing */
    if (!askTasks() && m_window) {
        gtk_window_close(GTK_WINDOW(m_window));
    }
}

void MainWindow::finishClosing()
{
    QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    const Vm *vm = current();
    int width = 0, height = 0;

    if (!m_window) {
        return;
    }
    gtk_window_get_default_size(GTK_WINDOW(m_window), &width, &height);
    settings.setValue("gtk/width", width);
    settings.setValue("gtk/height", height);
    settings.setValue("gtk/maximized", bool(gtk_window_is_maximized(GTK_WINDOW(m_window))));
    /* narrow, the sidebar hides as a VM is chosen: what the wide window had stays */
    if (!adw_overlay_split_view_get_collapsed(m_split)) {
        settings.setValue("gtk/sidebar", bool(adw_overlay_split_view_get_show_sidebar(m_split)));
    }
    settings.setValue("gtk/current", vm ? vm->id() : QString());
    settings.setValue("gtk/view", m_page->view() == VmPage::View::Details ? "details" : "console");
}

void MainWindow::quitWithoutQuestions()
{
    /* as Keep Running in the Background: the VMs run on; those asked to
       shut down as the window closed go on with it, not waited for */
    m_closeAfter.clear();
    m_shutDownOnceUp.clear();
    finishClosing();
    m_closing = true;
    if (m_window) {
        gtk_window_destroy(GTK_WINDOW(m_window));
    }
    m_loop->quit(0);
}

void MainWindow::closed()
{
    /* nothing to show any more: the core's signals go nowhere */
    m_qt.reset();
    m_window = nullptr;
    m_sidebar = nullptr;
    m_section = nullptr;
    m_split = nullptr;
    m_main = nullptr;
    m_items.clear();
    m_actions.clear();
}
