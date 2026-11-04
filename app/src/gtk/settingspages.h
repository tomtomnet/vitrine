// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/settingspage.h"

#include <QList>
#include <QPointer>
#include <QStringList>

#include "core/vmconfig.h"
#include "core/vmhardware.h"

class Vm;

/*
 * The pages of a VM's settings, each the Qt app's page of the same name,
 * reading and writing the arguments through the same core calls, on
 * libadwaita's rows.
 */

/* The name, the system and its desktop, the folder */
class GeneralPage : public SettingsPage
{
public:
    explicit GeneralPage(Vm *vm);

    QString title() const override { return "General"; }
    const char *icon() const override { return "document-properties-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    /* The guest the page shows */
    VmConfig::Guest shown() const;
    void updateDesktop();

    GtkWidget *m_name;
    GtkWidget *m_os;
    GtkWidget *m_desktop;
    /* what the lists' entries write, in their order */
    QStringList m_osKeys;
    QStringList m_desktopKeys;
    QString m_loaded;
    VmConfig::Guest m_loadedGuest;
};

/* Memory and processors */
class HardwarePage : public SettingsPage
{
public:
    HardwarePage();

    QString title() const override { return "Hardware"; }
    const char *icon() const override { return "preferences-system-details-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    GtkWidget *m_memory;
    GtkWidget *m_cpus;
    GtkWidget *m_topology;
    int m_loadedMemory = 0;
    int m_loadedCpus = 0;
};

/* NAT and the forward to the guest's SSH */
class NetworkPage : public SettingsPage
{
public:
    explicit NetworkPage(Vm *vm);

    QString title() const override { return "Network"; }
    const char *icon() const override { return "network-wired-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    /* What the page shows */
    VmConfig::Network shown() const;
    void update();

    QPointer<Vm> m_vm;
    AdwBanner *m_custom;
    GtkWidget *m_nat;
    GtkWidget *m_ssh;
    GtkWidget *m_port;
    GtkWidget *m_sshInfo;
    VmConfig::Network m_loaded;
    /* The host ports of the VM's forwards but the SSH one, which stay */
    QList<int> m_otherForwards;
    /* passt, if this computer and the VM's QEMU have it */
    QString m_backend;
};
