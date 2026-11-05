// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/settingspage.h"

#include <QList>
#include <QPointer>
#include <QTemporaryDir>

#include "core/firmwarefiles.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

class QemuDocs;
class Vm;

/*
 * The processor model and topology, the machine, the QEMU of the VM, and
 * under Boot its firmware and boot menu: the Qt app's MachinePage, through
 * the same core calls.  The model and the machine are text, with QEMU's
 * own lists to pick from.  The firmware copies a change of firmware needs
 * are staged, and copied into the VM folder on Apply (commit()); Reset
 * UEFI Variables asks the window's question first.
 */
class MachinePage : public SettingsPage
{
public:
    using ResetFirmware = std::function<void(Vm *vm, const QList<FirmwareFiles::File> &files,
                                             const QString &accept, std::function<void(bool)> done)>;

    MachinePage(Vm *vm, ResetFirmware resetFirmware);
    ~MachinePage() override;

    QString title() const override { return "Machine"; }
    const char *icon() const override { return "computer-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;
    bool commit(const ArgsFile &args, const QString &vmDir, QString *error) override;

    /* For tests */
    void setModel(const QString &model);
    void setMachine(const QString &machine);
    void setTopology(bool on, int sockets = 1, int cores = 1, int threads = 1);
    void setFirmware(VmConfig::FirmwareKind kind);

private:
    /* The QEMU chosen, empty for the default one */
    QString chosenQemu() const;
    void updateQemu();
    void fillLists();
    void updateTopology();
    /* The topology box or its numbers differ from what load() showed */
    bool topologyChanged() const;
    bool topologyOn() const;
    void describe();
    /* The machine shown, which the firmware to choose from depends on */
    QString shownMachine() const;
    VmConfig::FirmwareKind shownFirmware() const;
    /* The firmware for the machine shown, @choose chosen if it has it */
    void fillFirmware(VmConfig::FirmwareKind choose);
    void describeFirmware();
    /* Reset UEFI Variables: for the variable store the saved arguments name */
    void updateResetVars();
    void resetVars();

    QPointer<Vm> m_vm;
    ResetFirmware m_resetFirmware;
    QemuDocs *m_docs = nullptr;

    GtkWidget *m_model;
    GtkWidget *m_modelInfo;
    GtkWidget *m_topology;
    GtkWidget *m_sockets;
    GtkWidget *m_cores;
    GtkWidget *m_threads;
    GtkWidget *m_count;
    GtkWidget *m_machine;
    GtkWidget *m_machineInfo;
    GtkWidget *m_qemu;
    GtkWidget *m_qemuPath;
    GtkWidget *m_qemuInfo;
    GtkWidget *m_firmware;
    QList<VmConfig::FirmwareKind> m_firmwareKinds;
    GtkWidget *m_firmwareInfo;
    GtkWidget *m_resetVars;
    GtkWidget *m_bootMenu;
    /* QEMU's lists, for the rows' suggestions */
    QList<std::pair<QString, QString>> m_models;
    QList<std::pair<QString, QString>> m_machines;
    /* The firmware files save() copies, until the settings are applied */
    QTemporaryDir m_staging;

    /* As -smp and -cpu say */
    VmConfig::Cpus m_loadedCpus;
    /* The topology load() showed: the box, and QEMU's numbers */
    bool m_loadedTopology = false;
    VmConfig::Cpus m_shownCpus;
    QString m_loadedMachine;
    QString m_loadedQemu;
    VmConfig::FirmwareKind m_loadedFirmware = VmConfig::FirmwareKind::Bios;
    bool m_loadedBootMenu = false;
    /* The firmware list is for ARM's virt, which boots with UEFI only */
    bool m_firmwareVirt = false;
};
