// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MainWindow's part for the VMs' screens: the consoles (VmConsole), their
 * views and keys, full screen, Send Key and Release Input, the keys to the
 * screen as it comes, and the VM in front for the host's tuning.  The Qt
 * app's MainWindow does the same through the same core calls.
 */
#include "gtk/mainwindow.h"
#include "gtkview/viewkeys.h"

#include <QRegularExpression>

#include <algorithm>
#include <cstring>

#include "core/consoleviews.h"
#include "core/hostsettings.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/vmconsole.h"
#include "gtk/vmpage.h"
#include "gtk/widgets.h"

/* A click or a key this recent chose where the keys go: 0.5 s, as the Qt app has it */
static constexpr gint64 kRecentUs = 500000;

void MainWindow::createConsoleActions()
{
    auto *win = G_ACTION_MAP(m_window);

    /* as QEMU's SDL and GTK displays, and the screen itself */
    addAction(win, "full-screen", [this]() { toggleFullScreen(); }, "<Control><Alt>f");
    addAction(win, "release-input", [this]() { releaseInput(); });
    {
        GSimpleAction *send = g_simple_action_new("send-keys", G_VARIANT_TYPE_STRING);
        Gtk4::connect<void(GVariant *)>(send, "activate", [this](GVariant *keys) {
            sendKeys(QString::fromUtf8(g_variant_get_string(keys, nullptr)));
        });
        g_action_map_add_action(win, G_ACTION(send));
        g_object_unref(send);
        m_actions.insert("send-keys", send);
    }
    /* the nested desktop's views: radio items of the machine menu */
    {
        GSimpleAction *view = g_simple_action_new_stateful("view", G_VARIANT_TYPE_STRING,
                                                           g_variant_new_string("screen"));
        Gtk4::connect<void(GVariant *)>(view, "activate", [this](GVariant *target) {
            VmConsole *console = currentConsole();
            if (!console || !target) {
                return;
            }
            m_page->setView(VmPage::View::Console);
            console->showView(strcmp(g_variant_get_string(target, nullptr), "desktop") == 0
                                  ? ConsoleViews::View::Desktop
                                  : ConsoleViews::View::Screen);
        });
        g_action_map_add_action(win, G_ACTION(view));
        g_object_unref(view);
        m_actions.insert("view", view);
    }

    /*
     * Ctrl+Alt+1/2 before anything else of the window, the screen
     * included, which would give them to the guest.  A key controller: the
     * screens hold the window's shortcut controllers while they have the
     * keys, never this one.  In the full-screen windows, the views ask
     * (ViewKeys).
     */
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    Gtk4::connect<gboolean(guint, guint, GdkModifierType)>(
        keys, "key-pressed", [this](guint keyval, guint keycode, GdkModifierType state) -> gboolean {
            return switchViewByKey(keyval, keycode, state);
        });
    gtk_widget_add_controller(m_window, keys);
    ViewKeys::setFilter([this](guint keyval, guint keycode, GdkModifierType state) {
        return m_window && switchViewByKey(keyval, keycode, state);
    });

    /* a click in the window chose where the keys go: no screen takes them instead */
    GtkEventController *presses = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(presses, GTK_PHASE_CAPTURE);
    Gtk4::connect<gboolean(GdkEvent *)>(presses, "event", [this](GdkEvent *event) -> gboolean {
        if (gdk_event_get_event_type(event) == GDK_BUTTON_PRESS) {
            m_lastPress = g_get_monotonic_time();
        }
        return false;
    });
    gtk_widget_add_controller(m_window, presses);
    /* the arrows, Home, End... going through the list of VMs */
    GtkEventController *listKeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(listKeys, GTK_PHASE_CAPTURE);
    Gtk4::connect<gboolean(guint, guint, GdkModifierType)>(
        listKeys, "key-pressed", [this](guint, guint, GdkModifierType) -> gboolean {
            m_lastSidebarKey = g_get_monotonic_time();
            return false;
        });
    gtk_widget_add_controller(GTK_WIDGET(m_sidebar), listKeys);

    /* back in front: the keys to the screen, once GTK gave the focus back to its widget */
    Gtk4::connect<void(GParamSpec *)>(m_window, "notify::is-active", [this](GParamSpec *) {
        if (m_window && gtk_window_is_active(GTK_WINDOW(m_window))) {
            g_idle_add_once([](gpointer self) {
                static_cast<MainWindow *>(self)->focusScreen(FocusCause::Activated);
            }, this);
        }
    });
    /* a VM chosen in the list, the one shown too: its screen */
    Gtk4::connect<void(guint)>(m_sidebar, "activated",
                               [this](guint) { focusScreen(FocusCause::Switched); });
    /* the Console view chosen: its screen */
    m_page->consoleShown = [this]() { focusScreen(FocusCause::Shown); };
}

VmConsole *MainWindow::console(const QString &id)
{
    Vm *vm = m_store->find(id);

    return vm ? consoleOf(vm) : nullptr;
}

VmConsole *MainWindow::consoleOf(Vm *vm)
{
    const QString id = vm->id();

    if (VmConsole *console = m_consoles.value(id)) {
        return console;
    }
    if (!m_window) {
        return nullptr;
    }
    auto *console = new VmConsole(vm);
    m_consoles.insert(id, console);
    m_page->addConsole(console);
    /* the console shows only for the selected VM, the one the actions act on */
    console->changed = [this, console]() { consoleChanged(console); };
    console->screenReady = [this, console]() {
        if (console == currentConsole()) {
            focusScreen(FocusCause::Shown);
        }
    };
    if (vm == current()) {
        m_page->setConsole(console);
    }
    consoleChanged(console);
    return console;
}

VmConsole *MainWindow::currentConsole() const
{
    const Vm *vm = current();

    return vm ? m_consoles.value(vm->id()) : nullptr;
}

void MainWindow::dropConsole(const QString &id)
{
    if (VmConsole *console = m_consoles.take(id)) {
        m_page->removeConsole(console);
        delete console;
    }
}

void MainWindow::consoleChanged(VmConsole *console)
{
    /*
     * Focus priority: the VM whose screen has the keyboard - its console
     * with the focus in the active window, or its full-screen window active
     * - is in front, any console, selected or not.  Losing the keyboard to
     * a host window changes nothing (HostSettings::setFront).  GNOME says
     * nothing of the windows of QEMU's own, which KWin's script tells the
     * Qt app: the core then does without (HostSettings::windowActivated).
     */
    HostSettings *host = HostSettings::instance();
    if (host && console->hasKeyboard() && console->vm()) {
        host->setFront(console->vm()->id());
    }
    if (m_window && console == currentConsole()) {
        m_page->refreshConsole();
        updateConsoleActions();
    }
}

void MainWindow::updateConsoleActions()
{
    const Vm *vm = current();
    const VmConsole *console = currentConsole();
    const bool views = console && console->hasViews();

    setEnabled("full-screen", console && console->hasScreen());
    setEnabled("send-keys", vm && vm->runner()->state() == VmRunner::State::Running);
    setEnabled("release-input", console && (console->hasKeyboard() || console->grabbed()));
    setEnabled("view", views);
    if (GSimpleAction *view = m_actions.value("view")) {
        g_simple_action_set_state(view, g_variant_new_string(
                                            views && console->currentView() == ConsoleViews::View::Desktop
                                                ? "desktop"
                                                : "screen"));
    }
}

/*
 * As soon as the window is in front, the keys go to the screen of the VM it
 * shows, as after a click on it: when the window comes to the front, when
 * the screen appears or its VM runs again, when the Console view is
 * chosen, and when a VM is chosen in the list, the one shown too.  Not
 * when a click in the window brought it to the front (the click chose
 * where the keys go), nor while the keys go through the list of VMs, nor
 * from a text field or a menu that has them, nor under a dialog.  Release
 * Input gives them back to the window.
 */
void MainWindow::focusScreen(FocusCause cause)
{
    VmConsole *console = currentConsole();

    if (!m_window || !console || m_covering > 0 || !gtk_window_is_active(GTK_WINDOW(m_window)) ||
        !console->hasScreen() || !m_page->screenShown() || !console->takesInput() ||
        console->isFullScreen() || console->hasKeyboard()) {
        return;
    }
    const gint64 now = g_get_monotonic_time();
    if (cause == FocusCause::Activated && m_lastPress && now - m_lastPress < kRecentUs) {
        return;
    }
    if (cause == FocusCause::Switched && m_lastSidebarKey && now - m_lastSidebarKey < kRecentUs) {
        return;
    }
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(m_window));
    if (focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus) ||
                  gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))) {
        return;
    }
    console->focusScreen();
}

void MainWindow::leaveScreens()
{
    for (VmConsole *console : std::as_const(m_consoles)) {
        console->leaveScreen();
    }
}

void MainWindow::toggleFullScreen()
{
    VmConsole *console = currentConsole();

    if (!console || !console->hasScreen()) {
        return;
    }
    /* back from full screen, the screen is where the console shows */
    m_page->setView(VmPage::View::Console);
    console->setFullScreen(!console->isFullScreen());
}

bool MainWindow::switchViewByKey(guint keyval, guint keycode, GdkModifierType state)
{
    VmConsole *console = currentConsole();

    if (!console || !console->hasViews()) {
        return false;
    }
    const std::optional<ConsoleViews::View> view = VmConsole::viewForKey(keyval, keycode, state);
    if (!view) {
        return false;
    }
    /* not for a console out of sight, on the Details view: full screen it is in sight */
    if (!m_page->screenShown() && !console->isFullScreen()) {
        return false;
    }
    console->showView(*view);
    return true;
}

void MainWindow::sendKeys(const QString &keys)
{
    Vm *vm = current();

    if (!vm) {
        return;
    }
    const QStringList names = keys.split(' ', Qt::SkipEmptyParts);
    const QPointer<Vm> sent(vm);
    vm->runner()->sendKeys(names, [this, sent](const QString &error) {
        if (sent && m_window && !error.isEmpty()) {
            notify(QString("The keys did not reach %1: %2").arg(sent->name(), error));
        }
    });
    /* the guest's terminals show on the VM's own screen: in vitrine's window with the
       nested desktop, the Screen view, while the desktop's session goes on in the Desktop
       view */
    static const QRegularExpression fkey("^f[0-9]+$");
    VmConsole *console = currentConsole();
    const bool terminal = std::any_of(names.cbegin(), names.cend(),
                                      [](const QString &key) { return fkey.match(key).hasMatch(); });
    if (terminal && console && console->hasViews()) {
        m_page->setView(VmPage::View::Console);
        console->showView(ConsoleViews::View::Screen);
    }
}

void MainWindow::releaseInput()
{
    VmConsole *console = currentConsole();

    if (!console || !m_window) {
        return;
    }
    console->setGrab(false);
    /* the keys to the window again, its shortcuts with them */
    gtk_window_set_focus(GTK_WINDOW(m_window), nullptr);
}
