// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/settingspage.h"

#include <memory>

SettingsPage::SettingsPage(GtkWidget *widget) : m_widget(widget)
{
    g_object_ref_sink(m_widget);
}

SettingsPage::~SettingsPage()
{
    stopCallbacks();
    g_object_unref(m_widget);
}

void SettingsPage::changed()
{
    if (!m_loading && m_changed) {
        m_changed();
    }
}

void SettingsPage::present(AdwDialog *dialog) const
{
    if (m_present) {
        m_present(dialog);
    } else {
        adw_dialog_present(dialog, m_widget);
    }
}

void SettingsPage::later(std::function<void()> function)
{
    auto *call = new std::function<void()>(guarded<void()>(std::move(function)));
    g_idle_add_once(
        [](gpointer data) {
            std::unique_ptr<std::function<void()>> call(static_cast<std::function<void()> *>(data));
            (*call)();
        },
        call);
}
