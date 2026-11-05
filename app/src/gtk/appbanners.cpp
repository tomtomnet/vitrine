// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/appbanners.h"

#include "core/hostkde.h"
#include "core/stackbuilder.h"
#include "gtk/markup.h"
#include "gtk/widgets.h"
#include "ui/buildtexts.h"
#include "ui/hostkdetext.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

AppBanners::AppBanners()
{
    StackBuilder *builder = StackBuilder::instance();
    HostKdeMonitor *monitor = HostKdeMonitor::instance();

    QObject::connect(builder, &StackBuilder::started, &m_qt, [this]() {
        m_error.clear();
        m_percent = -1;
        refreshStack();
    });
    QObject::connect(builder, &StackBuilder::stepStarted, &m_qt, [this]() {
        m_percent = -1;
        refreshStack();
    });
    /* by whole percents: ninja's lines come by the thousand */
    QObject::connect(builder, &StackBuilder::progress, &m_qt, [this](int done, int total) {
        const int percent = total > 0 ? done * 100 / total : -1;
        if (percent != m_percent) {
            m_percent = percent;
            refreshStack();
        }
    });
    QObject::connect(builder, &StackBuilder::finished, &m_qt,
                     [this, builder](const QString &error) {
        /* stopped: as before it started */
        m_error = builder->wasStopped() ? QString() : error;
        refreshStack();
    });
    /* the preferences chose another QEMU, or the default again: reloadPreferred() tells */
    QObject::connect(QemuDocs::preferred(), &QemuDocs::changed, &m_qt,
                     [this]() { refreshStack(); });
    QObject::connect(monitor, &HostKdeMonitor::changed, &m_qt, [this]() { refreshHost(); });
    /* the system as it is now: it may have moved since the last run */
    if (!monitor->isRead()) {
        monitor->refresh();
    }
}

AppBanners::~AppBanners()
{
    for (const Place &place : std::as_const(m_places)) {
        g_signal_handlers_disconnect_by_data(place.stack, this);
        g_signal_handlers_disconnect_by_data(place.host, this);
        g_object_unref(place.box);
    }
}

void AppBanners::addTo(AdwToolbarView *view)
{
    Place place{view, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), ADW_BANNER(adw_banner_new("")),
                ADW_BANNER(adw_banner_new(""))};

    gtk_widget_set_name(GTK_WIDGET(place.stack), "stackBanner");
    gtk_widget_set_name(GTK_WIDGET(place.host), "hostKdeBanner");
    /* the texts are rich (bold, code): not always so by default */
    adw_banner_set_use_markup(place.stack, true);
    adw_banner_set_use_markup(place.host, true);
    gtk_box_append(GTK_BOX(place.box), GTK_WIDGET(place.stack));
    gtk_box_append(GTK_BOX(place.box), GTK_WIDGET(place.host));
    /* as long as this, in its view or out of it */
    g_object_ref_sink(place.box);
    adw_toolbar_view_add_top_bar(view, place.box);
    g_signal_connect_swapped(place.stack, "button-clicked",
                             G_CALLBACK(+[](AppBanners *self) { self->activateStack(); }), this);
    g_signal_connect_swapped(place.host, "button-clicked",
                             G_CALLBACK(+[](AppBanners *self) { self->activateHost(); }), this);
    m_places << place;
    show(place.stack, m_stackText, m_stackButton);
    show(place.host, m_hostText, m_hostButton);
    if (m_places.size() == 1) {
        refresh();
    }
}

void AppBanners::setScreenShown(AdwToolbarView *view, bool shown)
{
    for (const Place &place : std::as_const(m_places)) {
        if (place.view == view) {
            gtk_widget_set_visible(place.box, !shown);
        }
    }
}

void AppBanners::refresh()
{
    refreshStack();
    refreshHost();
}

void AppBanners::show(AdwBanner *banner, const QString &text, const QString &button)
{
    if (!text.isEmpty()) {
        adw_banner_set_title(banner, utf8(Markup::pango(text)).constData());
    }
    adw_banner_set_button_label(banner, button.isEmpty() ? nullptr
                                                         : utf8('_' + button).constData());
    adw_banner_set_revealed(banner, !text.isEmpty());
}

void AppBanners::refreshStack()
{
    m_stackText = BuildTexts::stackNote(StackBuilder::instance(), m_percent, m_error,
                                        &m_stackButton);
    for (const Place &place : std::as_const(m_places)) {
        show(place.stack, m_stackText, m_stackButton);
    }
}

void AppBanners::refreshHost()
{
    const QList<HostKde::Status> statuses = HostKdeMonitor::instance()->statuses();

    m_hostText = HostKdeNotice::text(statuses);
    m_outdated = HostKdeNotice::outdated(statuses);
    m_rebuild = HostKdeNotice::rebuildable(statuses);
    m_hostButton = m_hostText.isEmpty()   ? QString()
                   : !m_rebuild.isEmpty() ? QString("Rebuild…")
                                          : QString("Revert…");
    for (const Place &place : std::as_const(m_places)) {
        show(place.host, m_hostText, m_hostButton);
    }
}

void AppBanners::activateStack()
{
    if (showBuilds) {
        showBuilds("qemu");
    }
}

void AppBanners::activateHost()
{
    if (!m_rebuild.isEmpty()) {
        if (showBuilds) {
            showBuilds(m_rebuild);
        }
    } else if (!m_outdated.isEmpty() && revert) {
        revert(m_outdated);
    }
}

QString AppBanners::stackText() const
{
    return Markup::plain(m_stackText);
}

QString AppBanners::stackButton() const
{
    return m_stackButton;
}

QString AppBanners::hostText() const
{
    return Markup::plain(m_hostText);
}

QString AppBanners::hostButton() const
{
    return m_hostButton;
}
