// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/dialog.h"

namespace Gtk4 {

Dialog::Dialog(AdwDialog *dialog, Presenter presenter)
    : m_dialog(dialog), m_present(std::move(presenter))
{
    /* its widgets live as long as this side: Qt's connections may still reach them */
    g_object_ref_sink(m_dialog);
    on<void()>(m_dialog, "closed", [this]() { finish(); });
}

void Dialog::finish()
{
    m_closed = true;
    closed();
    g_idle_add_once([](gpointer self) { delete static_cast<Dialog *>(self); }, this);
}

Dialog::~Dialog()
{
    /* what the dialog's widgets emit as they go reaches nothing */
    stopCallbacks();
    g_object_unref(m_dialog);
}

void Dialog::close()
{
    if (!m_closed) {
        adw_dialog_close(m_dialog);
    }
}

void Dialog::forceClose()
{
    if (m_closed) {
        return;
    }
    /* never presented (a test's): closed all the same */
    if (!gtk_widget_get_parent(GTK_WIDGET(m_dialog))) {
        finish();
        return;
    }
    adw_dialog_force_close(m_dialog);
}

void Dialog::present(AdwDialog *dialog) const
{
    if (m_present) {
        m_present(dialog);
    } else {
        adw_dialog_present(dialog, GTK_WIDGET(m_dialog));
    }
}

}
