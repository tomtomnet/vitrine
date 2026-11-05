// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MainWindow's parts that the Qt app has in its status bar and its menus:
 * Preferences, and the notices of the running VMs (host tuning, udmabuf,
 * memory).  The Qt app's MainWindow sets up the same
 * through the same core calls.
 */
#include "gtk/mainwindow.h"

#include <utility>

#include "core/hostsettings.h"
#include "core/vmstore.h"
#include "gtk/gtkloop.h"
#include "gtk/notices.h"
#include "gtk/preferencesdialog.h"
#include "gtk/vmpage.h"
#include "gtk/widgets.h"

void MainWindow::createParts()
{
    auto *app = G_ACTION_MAP(m_loop->adw());

    /* Preferences, active now: in the place of the inactive one of createActions() */
    addAction(app, "preferences", [this]() { showPreferences(); }, "<Control>comma");

    /* what the running VMs want the user to know, at the start of the page's header bar */
    m_notices = new Notices(HostSettings::instance(), m_store, GTK_WINDOW(m_window), presenter());
    adw_header_bar_pack_start(m_page->headerBar(), m_notices->button());
}

void MainWindow::dropParts()
{
    delete std::exchange(m_notices, nullptr);
}

Notices *MainWindow::notices() const
{
    return m_notices;
}

PreferencesDialog *MainWindow::showPreferences()
{
    if (!m_window) {
        return nullptr;
    }
    /* one at a time: the one open stays */
    if (m_preferences) {
        return m_preferences;
    }
    auto *dialog = new PreferencesDialog(m_store, presenter(),
                                         [this](const QString &component, const QString &release) {
        showBuilds(component, release);
    });
    m_preferences = dialog;
    Gtk4::connect<void()>(dialog->dialog(), "closed", [this, dialog]() {
        if (m_preferences == dialog) {
            m_preferences = nullptr;
        }
    });
    present(dialog->dialog());
    return dialog;
}

/*
 * The Builds window on @component: gtk-builds' window, which takes this
 * place when its stream is merged; until then, the primary menu's Builds
 */
void MainWindow::showBuilds(const QString &component, const QString &guestRelease)
{
    Q_UNUSED(component);
    Q_UNUSED(guestRelease);
    if (m_loop) {
        g_action_group_activate_action(G_ACTION_GROUP(m_loop->adw()), "builds", nullptr);
    }
}
