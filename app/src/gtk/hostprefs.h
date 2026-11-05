// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"
#include "gtk/guarded.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/gpuinputawake.h"
#include "core/hostsettings.h"

class QTimer;

/*
 * The Preferences' Host page: host tuning while VMs run (HostSettings,
 * vitrine-helper), as the Qt app's HostTuningPrefs has it.  On or off;
 * on, a row says whether tuning is active, and if not why and what to do,
 * with Set Up… where the vitrine group is what it takes; off, nothing to
 * warn about.  The GPU clock floor, and on a host with a dedicated GPU,
 * which a floor does nothing for, gpu-input-awake (GpuInputAwake): whether
 * it is installed, Install… and Remove…, which act at once with the
 * administrator's password.  The CPU kept ready to wake up.
 *
 * Each change is saved at once, and reaches the running VMs a moment
 * later (HostSettings::preferencesChanged()), as the Qt dialog's OK does:
 * a clock floor typed digit by digit asks the helper once.
 */
class HostPrefs : public Gtk4::Guarded
{
public:
    /* @present: the alerts it asks over the Preferences */
    explicit HostPrefs(Gtk4::Dialog::Presenter present);
    ~HostPrefs();
    HostPrefs(const HostPrefs &) = delete;
    HostPrefs &operator=(const HostPrefs &) = delete;

    AdwPreferencesPage *page() const { return m_page; }
    /* The changes made reach the running VMs now, not in a moment */
    void flush();

private:
    void update();
    /* The state, asked again (polkit, the user database) */
    void refresh();
    void showState(const HostSettings::Status &status);
    /* Saved: the running VMs get it a moment later */
    void changed();
    /* gpu-input-awake's state, read again (its files, systemctl) */
    void refreshAwake();
    void showAwake(const QString &active);
    /* Install (true) or Remove, once confirmed */
    void runAwake(bool install);
    void saveFloor();

    Gtk4::Dialog::Presenter m_present;
    QObject m_qt;
    QTimer *m_retune;
    bool m_checked = false;
    bool m_needsGroup = false;
    /* what the running VMs were last tuned with, by these preferences */
    bool m_wasEnabled;
    QString m_wasFloor;
    bool m_wasCpuLatency;
    /* gpu-input-awake: none of its widgets without a dedicated GPU */
    QStringList m_dedicated;
    GpuInputAwake::Installed m_installed = GpuInputAwake::Installed::No;

    AdwPreferencesPage *m_page;
    GtkWidget *m_tune;
    GtkWidget *m_state;
    GtkWidget *m_setUp;
    GtkWidget *m_floor;
    GtkWidget *m_mhz;
    GtkWidget *m_cpuLatency;
    GtkWidget *m_awake = nullptr;
    GtkWidget *m_awakeInstall = nullptr;
    GtkWidget *m_awakeRemove = nullptr;
};
