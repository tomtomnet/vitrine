// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/guesttoolsstatus.h"

#include "core/guesttools.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmstore.h"
#include "gtk/markup.h"
#include "gtk/widgets.h"

using Gtk4::utf8;
using GuestTools::Pending;
using GuestTools::State;

/*
 * The tools are offered to @vm: its guest is known to run a system they are
 * for (it said so, through an agent, when it last ran), with the 3D card
 * their driver is for.  Not while what it runs is not known.
 */
static bool offered(Vm *vm)
{
    return GuestToolsMonitor::of(vm)->supported() &&
           VmConfig::graphics(vm->args()).kind == VmConfig::Graphics::Accelerated;
}

GuestToolsStatus::GuestToolsStatus()
    : m_home(gtk_list_box_new()), m_details(ADW_PREFERENCES_GROUP(adw_preferences_group_new()))
{
    /* they live as long as this, wherever they are shown */
    g_object_ref_sink(m_home);
    g_object_ref_sink(m_details);

    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_home), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(m_home, "boxed-list");
    gtk_widget_set_name(m_home, "guestTools");
    m_homeRow = makeRow();
    gtk_list_box_append(GTK_LIST_BOX(m_home), m_homeRow.row);

    adw_preferences_group_set_title(m_details, "Guest Tools");
    m_detailsRow = makeRow();
    adw_preferences_group_add(m_details, m_detailsRow.row);

    gtk_widget_set_visible(m_home, false);
    gtk_widget_set_visible(GTK_WIDGET(m_details), false);
}

GuestToolsStatus::~GuestToolsStatus()
{
    /* nothing they emit as they go reaches this */
    for (const Row &row : {m_homeRow, m_detailsRow}) {
        g_signal_handlers_disconnect_by_data(row.button, this);
    }
    g_object_unref(m_details);
    g_object_unref(m_home);
}

GuestToolsStatus::Row GuestToolsStatus::makeRow()
{
    Row row;

    row.row = adw_action_row_new();
    adw_action_row_set_title_lines(ADW_ACTION_ROW(row.row), 0);
    row.icon = gtk_image_new_from_icon_name("dialog-information-symbolic");
    adw_action_row_add_prefix(ADW_ACTION_ROW(row.row), row.icon);
    row.button = gtk_button_new();
    gtk_button_set_use_underline(GTK_BUTTON(row.button), true);
    gtk_widget_set_valign(row.button, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row.row), row.button);
    g_signal_connect_swapped(row.button, "clicked",
                             G_CALLBACK(+[](GuestToolsStatus *self) { self->activate(); }), this);
    return row;
}

Vm *GuestToolsStatus::vm() const
{
    return m_vm;
}

void GuestToolsStatus::setVm(Vm *vm)
{
    if (m_vm != vm) {
        if (m_vm) {
            QObject::disconnect(GuestToolsMonitor::of(m_vm), nullptr, &m_qt, nullptr);
            QObject::disconnect(m_vm, nullptr, &m_qt, nullptr);
        }
        m_vm = vm;
        if (vm) {
            QObject::connect(GuestToolsMonitor::of(vm), &GuestToolsMonitor::changed, &m_qt,
                             [this]() { update(); });
            /* its settings: the 3D card the tools' driver is for (offered()) */
            QObject::connect(vm, &Vm::changed, &m_qt, [this]() { update(); });
        }
    }
    update();
}

void GuestToolsStatus::update()
{
    m_text.clear();
    m_button.clear();
    if (m_vm) {
        const GuestToolsMonitor *monitor = GuestToolsMonitor::of(m_vm);
        const State state = monitor->state();
        const QString text = monitor->text();
        const GuestTools::Inputs in = monitor->inputs();
        /* a guest known to run a system the tools are not for: nothing to install */
        const bool other = monitor->unsupported();
        /* the install asked for at this start did not happen: said, offered or not */
        const bool asked = state == State::NotInstalled && in.running && in.bootstrapRun;
        QString button;
        bool warning = false;

        switch (state) {
        case State::NotInstalled:
            button = other ? QString() : QString("_Install Guest Tools…");
            warning = asked && other;
            break;
        case State::Pending:
            button = "_Cancel";
            break;
        case State::Failed:
            button = other ? QString() : QString("_Try Again…");
            warning = true;
            break;
        case State::RebootNeeded:
            button = monitor->hasAgent() ? QString("_Restart Guest") : QString();
            break;
        case State::DriverNotActive:
        case State::MesaNotActive:
        case State::KwinMismatch:
            button = other ? QString() : QString("_Install Again…");
            warning = true;
            break;
        case State::UpdateAvailable:
            button = "_Update…";
            break;
        case State::Unsupported:
            warning = true;
            break;
        default:
            break;
        }
        /* the offers: to install them, to update them */
        if (!text.isEmpty() && !(state == State::NotInstalled && !asked && !offered(m_vm)) &&
            !(state == State::UpdateAvailable && other)) {
            m_text = text;
            m_button = button;
            for (const Row &row : {m_homeRow, m_detailsRow}) {
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row.row),
                                              utf8(Markup::pango(text)).constData());
                gtk_image_set_from_icon_name(GTK_IMAGE(row.icon),
                                             warning ? "dialog-warning-symbolic"
                                                     : "dialog-information-symbolic");
                if (warning) {
                    gtk_widget_add_css_class(row.icon, "warning");
                } else {
                    gtk_widget_remove_css_class(row.icon, "warning");
                }
                if (!button.isEmpty()) {
                    gtk_button_set_label(GTK_BUTTON(row.button), utf8(button).constData());
                }
                gtk_widget_set_visible(row.button, !button.isEmpty());
            }
        }
    }
    gtk_widget_set_visible(m_home, !m_text.isEmpty());
    gtk_widget_set_visible(GTK_WIDGET(m_details), !m_text.isEmpty());
}

QString GuestToolsStatus::text() const
{
    return Markup::plain(m_text);
}

QString GuestToolsStatus::button() const
{
    return QString(m_button).remove('_');
}

void GuestToolsStatus::activate()
{
    if (!m_vm) {
        return;
    }
    GuestToolsMonitor *monitor = GuestToolsMonitor::of(m_vm);
    switch (monitor->state()) {
    case State::Pending:
        monitor->setPending(Pending::None);
        /* nor the restart its install waits to make, out of sight */
        if (cancelPending) {
            cancelPending(m_vm);
        }
        break;
    case State::RebootNeeded:
        monitor->requestReboot();
        break;
    default:
        if (install) {
            install(m_vm);
        }
        break;
    }
}
