// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/machinepage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "core/firmware.h"
#include "core/hostdevices.h"
#include "core/optionvalue.h"
#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;
using VmConfig::FirmwareKind;

static void setCaption(GtkWidget *caption, const QString &text)
{
    gtk_label_set_text(GTK_LABEL(caption), utf8(text).constData());
    gtk_widget_set_visible(caption, !text.isEmpty());
}

static GtkWidget *captionUnder(AdwPreferencesGroup *group)
{
    GtkWidget *caption = Gtk4::caption({});
    gtk_widget_set_margin_top(caption, 6);
    adw_preferences_group_add(group, caption);
    return caption;
}

MachinePage::MachinePage(Vm *vm, ResetFirmware resetFirmware)
    : SettingsPage(adw_preferences_page_new()), m_vm(vm),
      m_resetFirmware(std::move(resetFirmware))
{
    auto *page = ADW_PREFERENCES_PAGE(widget());
    AdwPreferencesGroup *cpu = Gtk4::group("Processor");
    AdwPreferencesGroup *machine = Gtk4::group("Machine");
    AdwPreferencesGroup *qemu = Gtk4::group("QEMU");
    AdwPreferencesGroup *boot = Gtk4::group("Boot");

    /* the model, and its topology */
    m_model = Gtk4::entryRow("Processor Model");
    gtk_widget_set_name(m_model, "cpuModel");
    Gtk4::addSuggestions(m_model, "QEMU's Processor Models", [this]() { return m_models; });
    adw_preferences_group_add(cpu, m_model);
    m_topology = adw_expander_row_new();
    gtk_widget_set_name(m_topology, "topology");
    Gtk4::setTitle(m_topology, "Set the Topology");
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(m_topology), true);
    m_sockets = Gtk4::spinRow("Sockets", 1, 1024, 1);
    m_cores = Gtk4::spinRow("Cores", 1, 1024, 1);
    m_threads = Gtk4::spinRow("Threads", 1, 1024, 1);
    for (GtkWidget *spin : {m_sockets, m_cores, m_threads}) {
        adw_expander_row_add_row(ADW_EXPANDER_ROW(m_topology), spin);
        on<void(GParamSpec *)>(spin, "notify::value", [this](GParamSpec *) {
            if (!m_loading) {
                updateTopology();
                changed();
            }
        });
    }
    adw_preferences_group_add(cpu, m_topology);
    m_modelInfo = captionUnder(cpu);
    m_count = captionUnder(cpu);
    adw_preferences_page_add(page, cpu);

    m_machine = Gtk4::entryRow("Machine");
    gtk_widget_set_name(m_machine, "machine");
    Gtk4::addSuggestions(m_machine, "QEMU's Machines", [this]() { return m_machines; });
    adw_preferences_group_add(machine, m_machine);
    m_machineInfo = captionUnder(machine);
    adw_preferences_page_add(page, machine);

    /* the QEMU of this VM: #qemu; the default's path in a tool tip: a long one would widen the
       page */
    const QString preferred = Paths::qemuBinary();
    m_qemu = Gtk4::comboRow("QEMU", {preferred.isEmpty() ? QString("From the Preferences: None")
                                                         : QString("From the Preferences"),
                                     "Another Build"});
    gtk_widget_set_tooltip_text(m_qemu, utf8(preferred).constData());
    gtk_widget_set_name(m_qemu, "qemu");
    m_qemuPath = Gtk4::entryRow("QEMU Binary");
    gtk_widget_set_name(m_qemuPath, "qemuPath");
    GtkWidget *choose = Gtk4::rowButton("document-open-symbolic", "Choose the QEMU Binary");
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(m_qemuPath), choose);
    adw_preferences_group_add(qemu, m_qemu);
    adw_preferences_group_add(qemu, m_qemuPath);
    m_qemuInfo = captionUnder(qemu);
    adw_preferences_page_add(page, qemu);

    /* the boot: what was the Boot page */
    m_firmware = Gtk4::comboRow("Firmware", {});
    gtk_widget_set_name(m_firmware, "firmware");
    m_bootMenu = Gtk4::switchRow("Show the Boot Menu When the VM Starts");
    gtk_widget_set_name(m_bootMenu, "bootMenu");
    /* for a system that no longer starts, its variables damaged */
    m_resetVars = Gtk4::buttonRow("_Reset UEFI Variables…");
    gtk_widget_add_css_class(m_resetVars, "destructive-action");
    gtk_widget_set_name(m_resetVars, "resetVars");
    adw_preferences_group_add(boot, m_firmware);
    adw_preferences_group_add(boot, m_bootMenu);
    adw_preferences_group_add(boot, m_resetVars);
    m_firmwareInfo = captionUnder(boot);
    /* one place for the devices to start from: Storage */
    GtkWidget *from = Gtk4::caption("The VM starts from the devices checked in the boot order of "
                                    "the Storage page, in that order.");
    gtk_widget_set_margin_top(from, 6);
    adw_preferences_group_add(boot, from);
    adw_preferences_page_add(page, boot);

    on<void()>(m_model, "changed", [this]() {
        describe();
        changed();
    });
    on<void()>(m_machine, "changed", [this]() {
        describe();
        /* the firmware there is for the machine: ARM's virt boots with UEFI only */
        if (shownMachine().startsWith("virt") != m_firmwareVirt) {
            fillFirmware(shownFirmware());
        }
        describeFirmware();
        changed();
    });
    on<void(GParamSpec *)>(m_topology, "notify::enable-expansion", [this](GParamSpec *) {
        if (m_loading) {
            return;
        }
        if (topologyOn() &&
            Gtk4::value(m_sockets) * Gtk4::value(m_cores) * Gtk4::value(m_threads) !=
                m_loadedCpus.count) {
            /* one socket of cores */
            const Loading loading(m_loading);
            Gtk4::setValue(m_sockets, 1);
            Gtk4::setValue(m_cores, m_loadedCpus.count);
            Gtk4::setValue(m_threads, 1);
        }
        updateTopology();
        changed();
    });
    on<void(GParamSpec *)>(m_qemu, "notify::selected", [this](GParamSpec *) {
        updateQemu();
        changed();
    });
    on<void()>(m_qemuPath, "changed", [this]() {
        updateQemu();
        changed();
    });
    on<void()>(choose, "clicked", [this, choose]() {
        Gtk4::chooseFile(choose, "QEMU Binary", {}, Gtk4::text(m_qemuPath),
                         guarded<void(const QString &)>([this](const QString &path) {
                             Gtk4::setSelected(m_qemu, 1);
                             Gtk4::setText(m_qemuPath, path);
                         }));
    });
    on<void(GParamSpec *)>(m_firmware, "notify::selected", [this](GParamSpec *) {
        if (!m_loading) {
            describeFirmware();
            changed();
        }
    });
    on<void(GParamSpec *)>(m_bootMenu, "notify::active", [this](GParamSpec *) { changed(); });
    on<void()>(m_resetVars, "activated", [this]() { resetVars(); });
    if (vm) {
        QObject::connect(vm->runner(), &VmRunner::stateChanged, qt(), [this]() { updateResetVars(); });
    }
    updateQemu();
}

MachinePage::~MachinePage() = default;

QString MachinePage::chosenQemu() const
{
    return Gtk4::selected(m_qemu) == 1 ? Gtk4::text(m_qemuPath).trimmed() : QString();
}

bool MachinePage::topologyOn() const
{
    return adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(m_topology));
}

void MachinePage::updateQemu()
{
    QemuDocs *docs = QemuDocs::of(chosenQemu());

    gtk_widget_set_sensitive(m_qemuPath, Gtk4::selected(m_qemu) == 1);
    if (docs != m_docs) {
        if (m_docs) {
            QObject::disconnect(m_docs, nullptr, qt(), nullptr);
        }
        m_docs = docs;
        QObject::connect(docs, &QemuDocs::changed, qt(), [this]() { fillLists(); });
    }
    fillLists();
}

void MachinePage::fillLists()
{
    const QemuInfo *info = m_docs->info();
    QStringList models = {"host", "max"};
    QStringList machines = {"q35", "pc"};

    m_models = {{"host", "The processor of this computer with all its features"},
                {"max", "Every feature QEMU can offer"}};
    m_machines = {{"q35", "A modern PC with PCI Express"}, {"pc", "An older PC (i440FX)"}};
    if (info) {
        for (const QemuNamedDoc &c : info->cpus) {
            if (!models.contains(c.name)) {
                models << c.name;
                m_models << std::pair(c.name, c.desc);
            }
        }
        for (const QemuNamedDoc &m : info->machines) {
            if (!machines.contains(m.name)) {
                machines << m.name;
                m_machines << std::pair(m.name, m.desc);
            }
        }
    }
    describe();

    if (Gtk4::selected(m_qemu) == 1 && chosenQemu().isEmpty()) {
        setCaption(m_qemuInfo, "Choose the QEMU binary of this VM.");
    } else if (info) {
        setCaption(m_qemuInfo, QString("QEMU %1").arg(info->version));
    } else {
        setCaption(m_qemuInfo, m_docs->status());
    }
}

void MachinePage::describe()
{
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;
    /* host,topoext=on: the model and its flags */
    const QString model = Gtk4::text(m_model).section(',', 0, 0).trimmed();
    const QString machine = Gtk4::text(m_machine).trimmed();
    QString modelText, machineText;

    if (model.isEmpty()) {
        modelText = "QEMU's basic processor, for compatibility.";
    } else if (model == "host") {
        modelText = "The processor of this computer with all its features: the fastest. Needs KVM.";
    } else if (model == "max") {
        modelText = "Every feature QEMU can offer.";
    } else if (info) {
        for (const QemuNamedDoc &c : info->cpus) {
            if (c.name == model) {
                modelText = c.desc;
            }
        }
    }
    if (machine == "q35") {
        machineText = "A modern PC with PCI Express. The best choice for most systems.";
    } else if (machine == "pc") {
        machineText = "An older PC (i440FX), for old systems.";
    } else if (info) {
        for (const QemuNamedDoc &m : info->machines) {
            if (m.name == machine) {
                machineText = m.desc;
            }
        }
    }
    setCaption(m_modelInfo, modelText);
    setCaption(m_machineInfo, machineText);
}

void MachinePage::updateTopology()
{
    const int count = topologyOn() && !m_loadedCpus.custom
                          ? Gtk4::value(m_sockets) * Gtk4::value(m_cores) * Gtk4::value(m_threads)
                          : m_loadedCpus.count;
    const QString processors = count == 1 ? QString("1 processor") : QString("%1 processors").arg(count);

    setCaption(m_count, m_loadedCpus.custom
                            ? QString("%1 in all. The -smp line has keys this page does not follow, "
                                      "such as maxcpus or dies: change it on the Arguments page.")
                                  .arg(processors)
                            : QString("%1 in all.").arg(processors));
}

void MachinePage::load(const ArgsFile &args)
{
    const Loading loading(m_loading);

    m_loadedCpus = VmConfig::cpus(args);
    m_loadedMachine = VmConfig::machineType(args);
    /* first: the machine shown below changes the firmware list */
    m_loadedFirmware = VmConfig::firmwareKind(args);
    {
        /* what -smp leaves out, as QEMU works it out, e.g. 2 sockets for "8,cores=4" */
        const VmConfig::Cpus shown = VmConfig::derivedTopology(m_loadedCpus);

        m_loadedTopology = m_loadedCpus.sockets > 0 || m_loadedCpus.cores > 0 ||
                           m_loadedCpus.threads > 0;
        Gtk4::setValue(m_sockets, shown.sockets);
        Gtk4::setValue(m_cores, shown.cores);
        Gtk4::setValue(m_threads, shown.threads);
        adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(m_topology), m_loadedTopology);
        adw_expander_row_set_expanded(ADW_EXPANDER_ROW(m_topology), false);
        /* maxcpus=, dies=...: numbers written without them would not start */
        gtk_widget_set_sensitive(m_topology, !m_loadedCpus.custom);
        /* compare with what the page shows */
        m_shownCpus.sockets = Gtk4::value(m_sockets);
        m_shownCpus.cores = Gtk4::value(m_cores);
        m_shownCpus.threads = Gtk4::value(m_threads);
    }
    updateTopology();
    Gtk4::setText(m_model, m_loadedCpus.model);
    Gtk4::setText(m_machine, m_loadedMachine);

    m_loadedQemu = VmConfig::qemuBinary(args);
    Gtk4::setText(m_qemuPath, m_loadedQemu);
    Gtk4::setSelected(m_qemu, m_loadedQemu.isEmpty() ? 0 : 1);
    updateQemu();
    describe();

    m_loadedBootMenu = VmConfig::bootMenu(args);
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_bootMenu), m_loadedBootMenu);
    fillFirmware(m_loadedFirmware);
    describeFirmware();
    updateResetVars();
}

bool MachinePage::topologyChanged() const
{
    if (m_loadedCpus.custom) {
        return false;
    }
    if (topologyOn() != m_loadedTopology) {
        return true;
    }
    return topologyOn() && (Gtk4::value(m_sockets) != m_shownCpus.sockets ||
                            Gtk4::value(m_cores) != m_shownCpus.cores ||
                            Gtk4::value(m_threads) != m_shownCpus.threads);
}

void MachinePage::save(ArgsFile &args)
{
    const QString machine = Gtk4::text(m_machine).trimmed();
    /* "host,topoext=on": the model, then flags for the -cpu line */
    const QString model = Gtk4::text(m_model).trimmed();
    const OptionValue flags(model.section(',', 1));
    const QString modelName = model.section(',', 0, 0).trimmed();
    const bool topology = topologyChanged();

    /*
     * -smp only when the topology was edited: the count is the Hardware
     * page's, and numbers the page made up must not change it
     */
    if (topology) {
        VmConfig::Cpus cpus = m_loadedCpus;

        cpus.sockets = cpus.cores = cpus.threads = 0;
        if (topologyOn()) {
            cpus.sockets = Gtk4::value(m_sockets);
            cpus.cores = Gtk4::value(m_cores);
            cpus.threads = Gtk4::value(m_threads);
            cpus.count = cpus.sockets * cpus.cores * cpus.threads;
        }
        cpus.model = modelName;
        VmConfig::setCpus(args, cpus);
    } else if (modelName != m_loadedCpus.model) {
        VmConfig::setCpuModel(args, modelName);
    }
    if (topology || modelName != m_loadedCpus.model) {
        /* AMD: the guest sees the threads of its cores only with topoext */
        const int threads = VmConfig::derivedTopology(VmConfig::cpus(args)).threads;
        if (threads > 1 && (modelName == "host" || modelName == "max") &&
            HostDevices::cpuHasFlag("topoext")) {
            VmConfig::enableCpuFeature(args, "topoext");
        }
    }
    m_loadedCpus = VmConfig::cpus(args);
    m_loadedTopology = topologyOn();
    m_shownCpus.sockets = Gtk4::value(m_sockets);
    m_shownCpus.cores = Gtk4::value(m_cores);
    m_shownCpus.threads = Gtk4::value(m_threads);
    if (!modelName.isEmpty() && !flags.isEmpty()) {
        const int cpu = args.indexOf("cpu");
        OptionValue v = args.valueAt(cpu);
        QString extra;

        for (const OptionValue::Item &item : flags.items()) {
            if (item.key.isEmpty() || item.bare) {
                /* +avx, or a flag written without a value */
                const QString word = item.key.isEmpty() ? item.value : item.key;
                if (!v.has(word) && !word.isEmpty()) {
                    extra += ',' + OptionValue::escape(word);
                }
            } else {
                v.set(item.key, item.value);
            }
        }
        args.setValueAt(cpu, v.toString() + extra);
        const Loading loading(m_loading);
        Gtk4::setText(m_model, modelName);
    }
    if (!machine.isEmpty() && machine != m_loadedMachine) {
        VmConfig::setMachineType(args, machine);
        m_loadedMachine = machine;
    }
    if (chosenQemu() != m_loadedQemu) {
        VmConfig::setQemuBinary(args, chosenQemu());
        m_loadedQemu = chosenQemu();
    }

    /* the boot, for the machine saved above */
    const FirmwareKind kind = shownFirmware();
    if (kind != m_loadedFirmware && kind != FirmwareKind::Custom) {
        if (kind == FirmwareKind::Bios) {
            VmConfig::useBios(args);
        } else if (const std::optional<Firmware> fw = FirmwareDb::find(
                       kind == FirmwareKind::UefiSecureBoot, VmConfig::machineType(args))) {
            /* copied into the VM folder when the settings are applied */
            FirmwareDb::apply(args, *fw, m_staging.path());
        }
        m_loadedFirmware = kind;
    }
    if (adw_switch_row_get_active(ADW_SWITCH_ROW(m_bootMenu)) != m_loadedBootMenu) {
        VmConfig::setBootMenu(args, adw_switch_row_get_active(ADW_SWITCH_ROW(m_bootMenu)));
        m_loadedBootMenu = adw_switch_row_get_active(ADW_SWITCH_ROW(m_bootMenu));
    }
}

bool MachinePage::isModified() const
{
    const QString machine = Gtk4::text(m_machine).trimmed();

    return Gtk4::text(m_model).trimmed() != m_loadedCpus.model ||
           (!machine.isEmpty() && machine != m_loadedMachine) || chosenQemu() != m_loadedQemu ||
           topologyChanged() || shownFirmware() != m_loadedFirmware ||
           adw_switch_row_get_active(ADW_SWITCH_ROW(m_bootMenu)) != m_loadedBootMenu;
}

QString MachinePage::shownMachine() const
{
    const QString machine = Gtk4::text(m_machine).trimmed();
    return machine.isEmpty() ? m_loadedMachine : machine;
}

FirmwareKind MachinePage::shownFirmware() const
{
    return m_firmwareKinds.value(Gtk4::selected(m_firmware), m_loadedFirmware);
}

void MachinePage::fillFirmware(FirmwareKind choose)
{
    const QString machine = shownMachine();
    const Loading loading(m_loading);
    QStringList names;

    m_firmwareVirt = machine.startsWith("virt");
    m_firmwareKinds.clear();
    /* ARM's virt boots with UEFI only */
    if (!m_firmwareVirt) {
        m_firmwareKinds << FirmwareKind::Bios;
        names << "BIOS (SeaBIOS), for old systems";
    } else if (m_loadedFirmware == FirmwareKind::Bios) {
        m_firmwareKinds << FirmwareKind::Bios;
        names << "None";
    }
    m_firmwareKinds << FirmwareKind::Uefi;
    names << "UEFI";
    if (!m_firmwareVirt || m_loadedFirmware == FirmwareKind::UefiSecureBoot ||
        FirmwareDb::find(true, machine)) {
        m_firmwareKinds << FirmwareKind::UefiSecureBoot;
        names << "UEFI with Secure Boot";
    }
    if (m_loadedFirmware == FirmwareKind::Custom) {
        m_firmwareKinds << FirmwareKind::Custom;
        names << "Set by hand";
    }
    Gtk4::setItems(m_firmware, names);
    const int chosen = int(m_firmwareKinds.indexOf(choose));
    Gtk4::setSelected(m_firmware, chosen >= 0 ? chosen : int(m_firmwareKinds.indexOf(m_loadedFirmware)));
    gtk_widget_set_sensitive(m_firmware, m_loadedFirmware != FirmwareKind::Custom);
}

/* The variable store in the VM folder, by the saved arguments, if it can be made again */
static FirmwareFiles::File resettableVars(const Vm *vm)
{
    for (const FirmwareFiles::File &f : vm ? FirmwareFiles::list(vm->args(), vm->dir())
                                           : QList<FirmwareFiles::File>()) {
        if (f.role == FirmwareFiles::File::Role::Vars && !f.templatePath.isEmpty()) {
            return f;
        }
    }
    return {};
}

void MachinePage::updateResetVars()
{
    const bool running = m_vm && m_vm->runner()->isActive();

    gtk_widget_set_visible(m_resetVars, !resettableVars(m_vm).path.isEmpty() && m_resetFirmware);
    gtk_widget_set_sensitive(m_resetVars, !running);
    gtk_widget_set_tooltip_text(m_resetVars, running ? "Shut the VM down first."
                                                     : "For a system that no longer starts: the "
                                                       "boot entries and keys go back to those of "
                                                       "a new VM.");
}

void MachinePage::resetVars()
{
    const FirmwareFiles::File vars = resettableVars(m_vm);

    if (vars.path.isEmpty() || !m_resetFirmware || !m_vm) {
        return;
    }
    m_resetFirmware(m_vm, {vars}, "_Reset", guarded<void(bool)>([this](bool done) {
        if (done) {
            setCaption(m_firmwareInfo, "The UEFI variables are those of a new VM again. The old "
                                       "ones are kept in the VM folder, in a .bak file.");
        }
    }));
}

void MachinePage::describeFirmware()
{
    const FirmwareKind kind = shownFirmware();
    const QString machine = shownMachine();

    if (kind == FirmwareKind::Custom) {
        setCaption(m_firmwareInfo, "The firmware is set by hand: change it on the Arguments page.");
    } else if ((kind == FirmwareKind::Uefi || kind == FirmwareKind::UefiSecureBoot) &&
               !FirmwareDb::find(kind == FirmwareKind::UefiSecureBoot, machine)) {
        setCaption(m_firmwareInfo, "No such UEFI firmware was found: install edk2-ovmf "
                                   "(edk2-aarch64 on ARM).");
    } else if (kind != m_loadedFirmware) {
        setCaption(m_firmwareInfo, "A system installed with UEFI needs UEFI, one installed with "
                                   "BIOS needs BIOS: changing it may keep the installed system from "
                                   "starting.");
    } else if (kind == FirmwareKind::UefiSecureBoot) {
        setCaption(m_firmwareInfo, "Secure Boot starts only signed kernels and modules: the guest "
                                   "tools' graphics driver is not signed.");
    } else if (kind == FirmwareKind::Bios) {
        setCaption(m_firmwareInfo, {});
    } else {
        setCaption(m_firmwareInfo, "The firmware and its variables are kept in the VM folder.");
    }
}

bool MachinePage::commit(const ArgsFile &args, const QString &vmDir, QString *error)
{
    const QDir staging(m_staging.path());

    /* the firmware copies the arguments use; the VM's own variables stay */
    for (const VmConfig::FileRef &ref : VmConfig::files(args)) {
        const QString source = staging.filePath(ref.path);
        const QString target = QDir(vmDir).filePath(ref.path);

        if (QDir::isAbsolutePath(ref.path) || !QFileInfo::exists(source) ||
            QFileInfo::exists(target)) {
            continue;
        }
        if (!QFile::copy(source, target)) {
            *error = QString("Cannot copy %1 into %2").arg(ref.path, vmDir);
            return false;
        }
        QFile::setPermissions(target, QFile::permissions(target) | QFile::ReadOwner |
                                          QFile::WriteOwner);
    }
    return true;
}

void MachinePage::setModel(const QString &model)
{
    Gtk4::setText(m_model, model);
}

void MachinePage::setMachine(const QString &machine)
{
    Gtk4::setText(m_machine, machine);
}

void MachinePage::setTopology(bool on, int sockets, int cores, int threads)
{
    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(m_topology), on);
    if (on) {
        Gtk4::setValue(m_sockets, sockets);
        Gtk4::setValue(m_cores, cores);
        Gtk4::setValue(m_threads, threads);
    }
}

void MachinePage::setFirmware(FirmwareKind kind)
{
    Gtk4::setSelected(m_firmware, int(m_firmwareKinds.indexOf(kind)));
}
