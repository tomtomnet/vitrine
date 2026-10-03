// SPDX-License-Identifier: GPL-2.0-or-later
#include "settingspages.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include "core/firmware.h"
#include "core/firmwarefiles.h"
#include "core/hostdevices.h"
#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/argseditor.h"
#include "ui/banner.h"
#include "ui/firmwarerepair.h"
#include "ui/icons.h"
#include "ui/qemudocs.h"
#include "ui/referencepanel.h"
#include "ui/uiconfig.h"
#include "ui/widgets.h"

/* General */

GeneralPage::GeneralPage(Vm *vm, QWidget *parent) : SettingsPage(parent), m_name(new QLineEdit)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *folder = Widgets::note(QString("<a href=\"%1\">%2</a>")
                            .arg(QUrl::fromLocalFile(vm->dir()).toString(),
                                 QDir::toNativeSeparators(vm->dir()).toHtmlEscaped()));

    m_name->setObjectName("name");
    m_name->setMaximumWidth(Widgets::em(this) * 20);
    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("Folder:"), folder);
    form->addRow(QString(), Widgets::hint(tr("The folder holds the arguments (vm.args), the disks the "
                                    "VM creates and its log.")));
    layout->addLayout(form);
    layout->addStretch();
}

QIcon GeneralPage::icon() const
{
    return Icons::themed({"preferences-system", "configure"}, QStyle::SP_ComputerIcon);
}

void GeneralPage::load(const ArgsFile &args)
{
    m_loaded = VmConfig::name(args);
    m_name->setText(m_loaded);
}

void GeneralPage::save(ArgsFile &args)
{
    const QString name = m_name->text().trimmed();

    if (!name.isEmpty() && name != m_loaded) {
        VmConfig::setName(args, name);
        m_loaded = name;
    }
}

bool GeneralPage::isModified() const
{
    const QString name = m_name->text().trimmed();
    return !name.isEmpty() && name != m_loaded;
}

/* System */

SystemPage::SystemPage(Vm *vm, QWidget *parent)
    : SettingsPage(parent), m_memorySlider(new QSlider(Qt::Horizontal)), m_memory(new QSpinBox),
      m_cpuSlider(new QSlider(Qt::Horizontal)), m_cpus(new QSpinBox),
      m_topology(new QCheckBox(tr("Set the &topology"))), m_sockets(new QSpinBox),
      m_cores(new QSpinBox), m_threads(new QSpinBox), m_model(new QComboBox),
      m_modelInfo(Widgets::hint()), m_machine(new QComboBox), m_machineInfo(Widgets::hint()),
      m_accel(new QComboBox), m_defaultQemu(new QRadioButton),
      m_ownQemu(new QRadioButton(tr("This &build:"))), m_qemuPath(new QLineEdit),
      m_qemuInfo(Widgets::hint()), m_vm(vm), m_firmware(new QComboBox),
      m_firmwareInfo(Widgets::hint()), m_resetVars(new QPushButton(tr("&Reset UEFI Variables…"))),
      m_bootMenu(new QCheckBox(tr("Show the boot men&u when the VM starts"))),
      m_bootDevice(new QComboBox)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *memoryRow = new QHBoxLayout;
    auto *cpuRow = new QHBoxLayout;
    const qint64 hostMiB = Widgets::hostMemoryMiB();
    const int hostCpus = QThread::idealThreadCount();

    m_memory->setObjectName("memory");
    m_memory->setRange(64, int(qMax<qint64>(hostMiB, 1024)));
    m_memory->setSingleStep(256);
    m_memory->setSuffix(tr(" MiB"));
    m_memory->setMinimumWidth(m_memory->fontMetrics().horizontalAdvance("0000000 MiB") + 40);
    m_memorySlider->setRange(0, m_memory->maximum() / 256);
    m_memorySlider->setPageStep(4);
    Widgets::link(m_memorySlider, m_memory, 256);
    /* sliders of a size to aim with, not the width of the window */
    m_memorySlider->setMaximumWidth(Widgets::em(this) * 16);
    memoryRow->addWidget(m_memorySlider, 1);
    memoryRow->addWidget(m_memory);
    memoryRow->addStretch();

    m_cpus->setObjectName("cpus");
    m_cpus->setRange(1, qMax(hostCpus, 1));
    m_cpuSlider->setRange(1, m_cpus->maximum());
    m_cpuSlider->setPageStep(2);
    Widgets::link(m_cpuSlider, m_cpus, 1);
    m_cpuSlider->setMaximumWidth(Widgets::em(this) * 16);
    cpuRow->addWidget(m_cpuSlider, 1);
    cpuRow->addWidget(m_cpus);
    cpuRow->addStretch();

    for (QSpinBox *spin : {m_sockets, m_cores, m_threads}) {
        spin->setRange(1, 1024);
        spin->setEnabled(false);
        connect(spin, &QSpinBox::valueChanged, this, &SystemPage::updateTopology);
    }
    connect(m_topology, &QCheckBox::toggled, this, [this](bool on) {
        for (QSpinBox *spin : {m_sockets, m_cores, m_threads}) {
            spin->setEnabled(on);
        }
        if (on && m_sockets->value() * m_cores->value() * m_threads->value() !=
                      m_cpus->value()) {
            /* one socket of cores */
            const QSignalBlocker a(m_sockets), b(m_cores), c(m_threads);
            m_sockets->setValue(1);
            m_cores->setValue(m_cpus->value());
            m_threads->setValue(1);
        }
        updateTopology();
    });

    m_model->setObjectName("cpuModel");
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    m_model->lineEdit()->setPlaceholderText(tr("QEMU default"));
    m_machine->setObjectName("machine");
    m_machine->setEditable(true);
    m_machine->setInsertPolicy(QComboBox::NoInsert);
    m_accel->setObjectName("accel");
    connect(m_model, &QComboBox::currentTextChanged, this, &SystemPage::describe);
    connect(m_machine, &QComboBox::currentTextChanged, this, &SystemPage::describe);

    form->addRow(Widgets::label(tr("M&emory:"), m_memory), memoryRow);
    form->addRow(QString(), Widgets::hint(tr("This computer has %1 GiB.")
                                     .arg(QString::number(hostMiB / 1024.0, 'f', 1))));
    form->addRow(Widgets::label(tr("&Processors:"), m_cpus), cpuRow);
    form->addRow(QString(), m_topology);
    form->addRow(tr("Sockets:"), m_sockets);
    form->addRow(tr("Cores:"), m_cores);
    form->addRow(tr("Threads:"), m_threads);
    form->addRow(tr("Processor mode&l:"), m_model);
    form->addRow(QString(), m_modelInfo);
    form->addRow(tr("Ma&chine:"), m_machine);
    form->addRow(QString(), m_machineInfo);
    form->addRow(tr("&Acceleration:"), m_accel);

    /* the QEMU of this VM: #qemu */
    auto *qemu = new QVBoxLayout;
    auto *ownRow = new QHBoxLayout;
    auto *qemuGroup = new QButtonGroup(this);
    m_defaultQemu->setObjectName("defaultQemu");
    m_ownQemu->setObjectName("ownQemu");
    m_qemuPath->setObjectName("qemuPath");
    m_qemuPath->setPlaceholderText(tr("A qemu-system-x86_64, e.g. of a build with a patch"));
    qemuGroup->addButton(m_defaultQemu);
    qemuGroup->addButton(m_ownQemu);
    ownRow->addWidget(m_ownQemu);
    ownRow->addWidget(Widgets::browseRow(m_qemuPath, tr("QEMU Binary")), 1);
    m_qemuPath->parentWidget()->setMaximumWidth(Widgets::em(this) * 30);
    qemu->addWidget(m_defaultQemu);
    qemu->addLayout(ownRow);
    form->addRow(Widgets::label(tr("&QEMU:"), m_defaultQemu), qemu);
    form->addRow(QString(), m_qemuInfo);

    /* boot */
    m_firmware->setObjectName("firmware");
    m_bootDevice->setObjectName("bootDevice");
    m_bootMenu->setObjectName("bootMenu");
    m_bootDevice->addItem(tr("The first bootable device, the firmware's default"),
                          int(VmConfig::BootDevice::Default));
    m_bootDevice->addItem(tr("The hard disk"), int(VmConfig::BootDevice::Disk));
    m_bootDevice->addItem(tr("The CD/DVD drive"), int(VmConfig::BootDevice::Cdrom));
    m_bootDevice->addItem(tr("The network (PXE)"), int(VmConfig::BootDevice::Network));
    m_bootDevice->setToolTip(tr("Sets bootindex=1 on its device, which both SeaBIOS and "
                                "UEFI follow"));
    /* for a system that no longer starts, its variables damaged */
    auto *resetRow = new QHBoxLayout;
    m_resetVars->setObjectName("resetVars");
    resetRow->addWidget(m_resetVars);
    resetRow->addStretch();
    connect(m_resetVars, &QPushButton::clicked, this, &SystemPage::resetVars);
    if (m_vm) {
        connect(m_vm->runner(), &VmRunner::stateChanged, this, &SystemPage::updateResetVars);
    }

    form->addSection(tr("Boot"));
    form->addRow(tr("F&irmware:"), m_firmware);
    form->addRow(QString(), m_firmwareInfo);
    form->addRow(QString(), resetRow);
    form->addRow(tr("&Start from:"), m_bootDevice);
    form->addRow(QString(), m_bootMenu);
    layout->addLayout(form);
    layout->addStretch();
    connect(m_firmware, &QComboBox::currentIndexChanged, this, &SystemPage::describeFirmware);

    connect(qemuGroup, &QButtonGroup::buttonToggled, this, &SystemPage::updateQemu);
    connect(m_qemuPath, &QLineEdit::textChanged, this, &SystemPage::updateQemu);
    updateQemu();
}

QString SystemPage::chosenQemu() const
{
    return m_ownQemu->isChecked() ? m_qemuPath->text().trimmed() : QString();
}

void SystemPage::updateQemu()
{
    const QString preferred = Paths::qemuBinary();
    QemuDocs *docs = QemuDocs::of(chosenQemu());

    /* the path in a tool tip: a long one would widen the page */
    m_defaultQemu->setText(preferred.isEmpty()
                               ? tr("The &default QEMU, from the preferences: not found")
                               : tr("The &default QEMU, from the preferences"));
    m_defaultQemu->setToolTip(preferred);
    m_qemuPath->parentWidget()->setEnabled(m_ownQemu->isChecked());
    if (docs != m_docs) {
        if (m_docs) {
            m_docs->disconnect(this);
        }
        m_docs = docs;
        connect(docs, &QemuDocs::changed, this, &SystemPage::fillLists);
    }
    fillLists();
}

QIcon SystemPage::icon() const
{
    return Icons::themed({"cpu", "computer"}, QStyle::SP_ComputerIcon);
}

void SystemPage::fillLists()
{
    const QemuInfo *info = m_docs->info();
    const QString model = m_model->currentText();
    const QString machine = m_machine->currentText();
    const QSignalBlocker a(m_model), b(m_machine);
    QStringList models = {"host", "max"};
    QStringList machines = {"q35", "pc"};

    if (info) {
        for (const QemuNamedDoc &c : info->cpus) {
            if (!models.contains(c.name)) {
                models << c.name;
            }
        }
        for (const QemuNamedDoc &m : info->machines) {
            if (!machines.contains(m.name)) {
                machines << m.name;
            }
        }
    }
    m_model->clear();
    m_model->addItems(models);
    m_model->setCurrentText(model);
    m_machine->clear();
    m_machine->addItems(machines);
    m_machine->setCurrentText(machine);
    describe();

    if (m_ownQemu->isChecked() && chosenQemu().isEmpty()) {
        m_qemuInfo->setText(tr("Choose the QEMU binary of this VM."));
    } else if (info) {
        m_qemuInfo->setText(tr("QEMU %1").arg(info->version));
    } else {
        m_qemuInfo->setText(m_docs->status());
    }
}

void SystemPage::describe()
{
    const QemuInfo *info = m_docs->info();
    /* host,topoext=on: the model and its flags */
    const QString model = m_model->currentText().section(',', 0, 0).trimmed();
    const QString machine = m_machine->currentText().trimmed();
    QString modelText, machineText;

    if (model.isEmpty()) {
        modelText = tr("QEMU's basic processor, for compatibility.");
    } else if (model == "host") {
        modelText = tr("The processor of this computer with all its features: the fastest. "
                       "Needs KVM.");
    } else if (model == "max") {
        modelText = tr("Every feature QEMU can offer.");
    } else if (info) {
        for (const QemuNamedDoc &c : info->cpus) {
            if (c.name == model) {
                modelText = c.desc;
            }
        }
    }
    if (machine == "q35") {
        machineText = tr("A modern PC with PCI Express. The best choice for most systems.");
    } else if (machine == "pc") {
        machineText = tr("An older PC (i440FX), for old systems.");
    } else if (info) {
        for (const QemuNamedDoc &m : info->machines) {
            if (m.name == machine) {
                machineText = m.desc;
            }
        }
    }
    m_modelInfo->setText(modelText);
    m_machineInfo->setText(machineText);
}

void SystemPage::updateTopology()
{
    if (m_topology->isChecked()) {
        m_cpus->setValue(m_sockets->value() * m_cores->value() * m_threads->value());
    }
    m_cpus->setEnabled(!m_topology->isChecked());
    m_cpuSlider->setEnabled(!m_topology->isChecked());
}

void SystemPage::load(const ArgsFile &args)
{
    const QString accel = VmConfig::accel(args);

    m_loadedMemory = VmConfig::memoryMiB(args);
    m_loadedCpus = VmConfig::cpus(args);
    m_loadedMachine = VmConfig::machineType(args);
    m_loadedAccel = accel;

    m_memory->setMaximum(int(qMax<qint64>(m_memory->maximum(), m_loadedMemory)));
    m_memorySlider->setMaximum(m_memory->maximum() / 256);
    m_memory->setValue(int(m_loadedMemory > 0 ? m_loadedMemory : 128));
    m_cpus->setMaximum(qMax(m_cpus->maximum(), m_loadedCpus.count));
    m_cpuSlider->setMaximum(m_cpus->maximum());
    m_cpus->setValue(m_loadedCpus.count);
    {
        const QSignalBlocker a(m_sockets), b(m_cores), c(m_threads), d(m_topology);
        const bool given = m_loadedCpus.sockets > 0 || m_loadedCpus.cores > 0 ||
                           m_loadedCpus.threads > 0;
        m_sockets->setValue(qMax(m_loadedCpus.sockets, 1));
        m_cores->setValue(qMax(m_loadedCpus.cores, 1));
        m_threads->setValue(qMax(m_loadedCpus.threads, 1));
        m_topology->setChecked(given);
        for (QSpinBox *spin : {m_sockets, m_cores, m_threads}) {
            spin->setEnabled(given);
        }
    }
    m_cpus->setEnabled(!m_topology->isChecked());
    m_cpuSlider->setEnabled(!m_topology->isChecked());
    /* compare with what the page shows, e.g. 128 MiB for no -m, so that
       an untouched page writes nothing */
    m_loadedMemory = m_memory->value();
    m_loadedCpus.count = m_cpus->value();
    if (m_topology->isChecked()) {
        m_loadedCpus.sockets = m_sockets->value();
        m_loadedCpus.cores = m_cores->value();
        m_loadedCpus.threads = m_threads->value();
    }
    m_model->setCurrentText(m_loadedCpus.model);
    m_machine->setCurrentText(m_loadedMachine);

    m_loadedQemu = VmConfig::qemuBinary(args);
    {
        const QSignalBlocker a(m_defaultQemu), b(m_ownQemu), c(m_qemuPath);
        m_qemuPath->setText(m_loadedQemu);
        (m_loadedQemu.isEmpty() ? m_defaultQemu : m_ownQemu)->setChecked(true);
    }
    updateQemu();

    m_accel->clear();
    m_accel->addItem(tr("KVM: hardware virtualization, fast"), "kvm");
    m_accel->addItem(tr("TCG: software emulation, slow"), "tcg");
    if (accel.isEmpty()) {
        m_accel->addItem(tr("Not set: QEMU's default, TCG"), QString());
    } else if (accel != "kvm" && accel != "tcg") {
        m_accel->addItem(accel, accel);
    }
    m_accel->setCurrentIndex(m_accel->findData(accel));
    describe();
    loadBoot(args);
}

void SystemPage::save(ArgsFile &args)
{
    VmConfig::Cpus cpus;
    const QString machine = m_machine->currentText().trimmed();
    const QString accel = m_accel->currentData().toString();

    if (m_memory->value() != m_loadedMemory) {
        VmConfig::setMemoryMiB(args, m_memory->value());
        m_loadedMemory = m_memory->value();
    }

    cpus.count = m_cpus->value();
    if (m_topology->isChecked()) {
        cpus.sockets = m_sockets->value();
        cpus.cores = m_cores->value();
        cpus.threads = m_threads->value();
    }
    /* "host,topoext=on": the model, then flags for the -cpu line */
    const QString model = m_model->currentText().trimmed();
    const OptionValue flags(model.section(',', 1));
    cpus.model = model.section(',', 0, 0).trimmed();
    if (cpus.count != m_loadedCpus.count || cpus.sockets != m_loadedCpus.sockets ||
        cpus.cores != m_loadedCpus.cores || cpus.threads != m_loadedCpus.threads ||
        cpus.model != m_loadedCpus.model) {
        VmConfig::setCpus(args, cpus);
        m_loadedCpus = cpus;
        /* AMD: the guest sees the threads of its cores only with topoext */
        if (cpus.threads > 1 && (cpus.model == "host" || cpus.model == "max") &&
            HostDevices::cpuHasFlag("topoext")) {
            VmConfig::enableCpuFeature(args, "topoext");
        }
    }
    if (!cpus.model.isEmpty() && !flags.isEmpty()) {
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
        m_model->setCurrentText(cpus.model);
    }
    if (!machine.isEmpty() && machine != m_loadedMachine) {
        VmConfig::setMachineType(args, machine);
        m_loadedMachine = machine;
    }
    if (accel != m_loadedAccel) {
        VmConfig::setAccel(args, accel);
        m_loadedAccel = accel;
    }
    if (chosenQemu() != m_loadedQemu) {
        VmConfig::setQemuBinary(args, chosenQemu());
        m_loadedQemu = chosenQemu();
    }
    saveBoot(args);
}

bool SystemPage::isModified() const
{
    const QString machine = m_machine->currentText().trimmed();

    if (bootModified()) {
        return true;
    }
    if (m_memory->value() != m_loadedMemory || m_cpus->value() != m_loadedCpus.count ||
        m_model->currentText().trimmed() != m_loadedCpus.model ||
        (!machine.isEmpty() && machine != m_loadedMachine) ||
        m_accel->currentData().toString() != m_loadedAccel || chosenQemu() != m_loadedQemu) {
        return true;
    }
    if (!m_topology->isChecked()) {
        return m_loadedCpus.sockets || m_loadedCpus.cores || m_loadedCpus.threads;
    }
    return m_sockets->value() != m_loadedCpus.sockets ||
           m_cores->value() != m_loadedCpus.cores || m_threads->value() != m_loadedCpus.threads;
}

void SystemPage::loadBoot(const ArgsFile &args)
{
    using VmConfig::FirmwareKind;
    const bool virt = m_loadedMachine.startsWith("virt");
    const QList<VmConfig::Disk> disks = VmConfig::disks(args);
    bool disk = false, cdrom = false, nic = false;

    m_loadedFirmware = VmConfig::firmwareKind(args);
    {
        const QSignalBlocker block(m_firmware);
        m_firmware->clear();
        /* ARM's virt boots with UEFI only */
        if (!virt) {
            m_firmware->addItem(tr("BIOS (SeaBIOS), for old systems"), int(FirmwareKind::Bios));
        } else if (m_loadedFirmware == FirmwareKind::Bios) {
            m_firmware->addItem(tr("None"), int(FirmwareKind::Bios));
        }
        m_firmware->addItem(tr("UEFI"), int(FirmwareKind::Uefi));
        if (!virt || FirmwareDb::find(true, m_loadedMachine)) {
            m_firmware->addItem(tr("UEFI with Secure Boot"), int(FirmwareKind::UefiSecureBoot));
        }
        if (m_loadedFirmware == FirmwareKind::Custom) {
            m_firmware->addItem(tr("Set by hand"), int(FirmwareKind::Custom));
        }
        m_firmware->setCurrentIndex(m_firmware->findData(int(m_loadedFirmware)));
        m_firmware->setEnabled(m_loadedFirmware != FirmwareKind::Custom);
    }

    m_loadedBootMenu = VmConfig::bootMenu(args);
    m_bootMenu->setChecked(m_loadedBootMenu);

    for (const VmConfig::Disk &d : disks) {
        (d.cdrom ? cdrom : disk) |= d.editable;
    }
    for (int i : args.indexesOf("device")) {
        nic |= args.valueAt(i).has("netdev");
    }
    m_loadedBootDevice = VmConfig::firstBootDevice(args);
    if (auto *model = qobject_cast<QStandardItemModel *>(m_bootDevice->model())) {
        const bool present[] = {true, disk, cdrom, nic};
        for (int i = 0; i < model->rowCount(); i++) {
            model->item(i)->setEnabled(present[i] || i == int(m_loadedBootDevice));
        }
    }
    m_bootDevice->setCurrentIndex(m_bootDevice->findData(int(m_loadedBootDevice)));
    describeFirmware();
    updateResetVars();
}

/* The variable store in the VM folder, by the saved arguments, if it can be made again */
static FirmwareFiles::File resettableVars(const Vm *vm)
{
    for (const FirmwareFiles::File &f : vm ? FirmwareRepair::files(vm)
                                           : QList<FirmwareFiles::File>()) {
        if (f.role == FirmwareFiles::File::Role::Vars && !f.templatePath.isEmpty()) {
            return f;
        }
    }
    return {};
}

void SystemPage::updateResetVars()
{
    const bool running = m_vm && m_vm->runner()->isActive();

    m_resetVars->setVisible(!resettableVars(m_vm).path.isEmpty());
    m_resetVars->setEnabled(!running);
    m_resetVars->setToolTip(running ? tr("Shut the VM down first.")
                                    : tr("For a system that no longer starts: the boot "
                                         "entries and keys go back to those of a new VM."));
}

void SystemPage::resetVars()
{
    const FirmwareFiles::File vars = resettableVars(m_vm);

    if (!vars.path.isEmpty() && FirmwareRepair::reset(this, m_vm, {vars}, tr("&Reset"))) {
        m_firmwareInfo->setText(tr("The UEFI variables are those of a new VM again. The old "
                                   "ones are kept in the VM folder, in a .bak file."));
    }
}

void SystemPage::saveBoot(ArgsFile &args)
{
    using VmConfig::FirmwareKind;
    const auto kind = FirmwareKind(m_firmware->currentData().toInt());
    const auto device = VmConfig::BootDevice(m_bootDevice->currentData().toInt());

    if (kind != m_loadedFirmware && kind != FirmwareKind::Custom) {
        if (kind == FirmwareKind::Bios) {
            VmConfig::useBios(args);
        } else if (const std::optional<Firmware> fw = FirmwareDb::find(
                       kind == FirmwareKind::UefiSecureBoot, m_machine->currentText().trimmed())) {
            /* copied into the VM folder when the dialog applies */
            FirmwareDb::apply(args, *fw, m_staging.path());
        }
        m_loadedFirmware = kind;
    }
    if (m_bootMenu->isChecked() != m_loadedBootMenu) {
        VmConfig::setBootMenu(args, m_bootMenu->isChecked());
        m_loadedBootMenu = m_bootMenu->isChecked();
    }
    if (device != m_loadedBootDevice) {
        VmConfig::setFirstBootDevice(args, device);
        m_loadedBootDevice = device;
    }
}

bool SystemPage::bootModified() const
{
    return m_firmware->currentData().toInt() != int(m_loadedFirmware) ||
           m_bootMenu->isChecked() != m_loadedBootMenu ||
           m_bootDevice->currentData().toInt() != int(m_loadedBootDevice);
}

void SystemPage::describeFirmware()
{
    using VmConfig::FirmwareKind;
    const auto kind = FirmwareKind(m_firmware->currentData().toInt());

    if (kind == FirmwareKind::Custom) {
        m_firmwareInfo->setText(tr("The firmware is set by hand: change it on the Arguments "
                                   "page."));
    } else if ((kind == FirmwareKind::Uefi || kind == FirmwareKind::UefiSecureBoot) &&
               !FirmwareDb::find(kind == FirmwareKind::UefiSecureBoot,
                                 m_machine->currentText().trimmed())) {
        m_firmwareInfo->setText(tr("No such UEFI firmware was found: install edk2-ovmf "
                                   "(edk2-aarch64 on ARM)."));
    } else if (kind != m_loadedFirmware) {
        m_firmwareInfo->setText(tr("A system installed with UEFI needs UEFI, one installed "
                                   "with BIOS needs BIOS: changing it may keep the installed "
                                   "system from starting."));
    } else if (kind == FirmwareKind::Bios) {
        m_firmwareInfo->setText(QString());
    } else {
        m_firmwareInfo->setText(tr("The firmware and its variables are kept in the VM "
                                   "folder."));
    }
}

bool SystemPage::commit(const ArgsFile &args, const QString &vmDir, QString *error)
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
            *error = tr("Cannot copy %1 into %2").arg(ref.path, vmDir);
            return false;
        }
        QFile::setPermissions(target, QFile::permissions(target) | QFile::ReadOwner |
                                          QFile::WriteOwner);
    }
    return true;
}

/* Display */

DisplayPage::DisplayPage(QWidget *parent)
    : SettingsPage(parent), m_custom(new Banner(Banner::Information)), m_kind(new QComboBox),
      m_device(new QComboBox),
      m_nativeContext(new QCheckBox(tr("DRM &native context"))),
      m_venus(new QCheckBox(tr("&Vulkan through Venus"))),
      m_venusUnused(new Banner(Banner::Warning)), m_hostmem(new QSpinBox),
      m_window(new QComboBox)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();

    m_kind->setObjectName("graphics");
    m_device->setObjectName("gpuDevice");
    /* its cards change with the kind: it gets as wide as they need, then */
    m_device->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_nativeContext->setObjectName("nativeContext");
    m_venus->setObjectName("venus");
    m_hostmem->setObjectName("hostmem");
    m_window->setObjectName("window");
    m_hostmem->setRange(1, 256);
    m_hostmem->setSuffix(tr(" GiB"));

    form->addRow(tr("&Graphics:"), m_kind);
    form->addRow(tr("&Card:"), m_device);
    form->addRow(QString(), Widgets::hint(tr("A card with VGA shows the firmware and the boot "
                                             "screens; a PCI-only card shows the system once "
                                             "its driver starts.")));
    form->addRow(tr("&Window:"), m_window);

    form->addSection(tr("3D acceleration"));
    form->addRow(QString(), m_nativeContext);
    form->addRow(QString(), Widgets::hint(
        tr("The guest uses the GPU through its own driver: much faster than virgl. It needs a "
           "virglrenderer built with native context for the GPU of this computer, whose renderer depends on the GPU: Intel (Xe or i915), "
           "AMD, Qualcomm, Apple (Asahi) or Arm Mali. File > Build QEMU builds one. The guest "
           "needs native context support in Mesa too. With KVM, the accelerator gets "
           "honor-guest-pat=on, which Intel GPUs need.")));
    form->addRow(QString(), m_venus);
    form->addRow(QString(), Widgets::hint(
        tr("Vulkan in the guest through the Vulkan driver of this computer; it needs a "
           "virglrenderer built with Venus.")));
    /* both on: native context does Vulkan too */
    m_venusUnused->setObjectName("venusUnused");
    m_venusUnused->setText(tr("Venus goes unused with DRM native context: the guest's own GPU "
                              "driver does Vulkan as well."));
    m_venusUnused->button()->setText(tr("Turn Venus &Off"));
    m_venusUnused->button()->show();
    m_venusUnused->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(m_venusUnused->button(), &QPushButton::clicked, this,
            [this]() { m_venus->setChecked(false); });
    form->addRow(QString(), m_venusUnused);
    form->addRow(tr("GPU m&emory window:"), m_hostmem);
    form->addRow(QString(), Widgets::hint(
        tr("The host memory the guest maps its GPU buffers into (hostmem, with blob=on).")));

    layout->addWidget(m_custom);
    layout->addLayout(form);
    layout->addStretch();

    connect(m_kind, &QComboBox::currentIndexChanged, this, [this]() {
        fillDevices();
        update();
    });
    connect(m_nativeContext, &QCheckBox::toggled, this, &DisplayPage::update);
    connect(m_venus, &QCheckBox::toggled, this, &DisplayPage::update);
}

QIcon DisplayPage::icon() const
{
    return Icons::themed({"video-display", "preferences-desktop-display"},
                         QStyle::SP_DesktopIcon);
}

/* The cards of @kind, VGA first but on ARM's virt */
static QStringList cardsOf(VmConfig::Graphics::Kind kind, bool virt)
{
    if (kind == VmConfig::Graphics::Accelerated) {
        return virt ? QStringList{"virtio-gpu-gl-pci"}
                    : QStringList{"virtio-vga-gl", "virtio-gpu-gl-pci"};
    }
    if (kind == VmConfig::Graphics::Virtio) {
        return virt ? QStringList{"virtio-gpu-pci"} : QStringList{"virtio-vga", "virtio-gpu-pci"};
    }
    return {};
}

void DisplayPage::fillDevices()
{
    const auto kind = VmConfig::Graphics::Kind(m_kind->currentData().toInt());
    QStringList cards = cardsOf(kind, m_virt);
    const QSignalBlocker block(m_device);
    /* the card as written, or its twin with or without OpenGL */
    const QString preferred = cards.isEmpty()           ? QString()
                              : kind == m_loaded.kind ? m_loaded.device
                                                      : VmConfig::glCounterpart(m_loaded.device);

    /* e.g. virtio-gpu-gl, as written */
    if (!preferred.isEmpty() && !cards.contains(preferred)) {
        cards.prepend(preferred);
    }
    m_device->clear();
    for (const QString &card : std::as_const(cards)) {
        m_device->addItem(VmConfig::isVgaDevice(card) ? tr("%1, with VGA").arg(card)
                                                      : tr("%1, PCI only").arg(card),
                          card);
    }
    if (!preferred.isEmpty()) {
        m_device->setCurrentIndex(m_device->findData(preferred));
    }
}

void DisplayPage::update()
{
    const auto kind = VmConfig::Graphics::Kind(m_kind->currentData().toInt());
    const bool custom = m_loaded.kind == VmConfig::Graphics::Custom;
    const bool accelerated = kind == VmConfig::Graphics::Accelerated;

    m_kind->setEnabled(!custom);
    m_device->setEnabled(!custom && m_device->count() > 1);
    m_nativeContext->setEnabled(accelerated);
    m_venus->setEnabled(accelerated);
    m_hostmem->setEnabled(accelerated && (m_nativeContext->isChecked() || m_venus->isChecked()));
    m_venusUnused->setVisible(accelerated && m_nativeContext->isChecked() && m_venus->isChecked());
}

void DisplayPage::load(const ArgsFile &args)
{
    using Kind = VmConfig::Graphics::Kind;
    const QSignalBlocker a(m_kind), b(m_nativeContext), c(m_venus), d(m_window);

    m_loaded = VmConfig::graphics(args);
    m_virt = VmConfig::machineType(args).startsWith("virt");

    m_kind->clear();
    m_kind->addItem(tr("3D accelerated: virtio-gpu with OpenGL"), int(Kind::Accelerated));
    m_kind->addItem(tr("2D: virtio-gpu"), int(Kind::Virtio));
    if (!m_virt) {
        m_kind->addItem(tr("Standard VGA, for compatibility"), int(Kind::Standard));
    }
    m_kind->addItem(tr("None"), int(Kind::None));
    if (m_loaded.kind == Kind::Custom) {
        m_kind->addItem(tr("Set by hand: %1").arg(m_loaded.custom), int(Kind::Custom));
    }
    m_kind->setCurrentIndex(m_kind->findData(int(m_loaded.kind)));
    fillDevices();

    m_nativeContext->setChecked(m_loaded.nativeContext);
    m_venus->setChecked(m_loaded.venus);
    m_loadedHostmemGiB = int(qMax<qint64>((m_loaded.hostmemMiB + 1023) / 1024, 4));
    m_hostmem->setValue(m_loadedHostmemGiB);

    m_window->clear();
    m_window->addItem(tr("SDL"), "sdl");
    m_window->addItem(tr("GTK"), "gtk");
    m_window->addItem(tr("None: no window"), "none");
    if (m_loaded.display.isEmpty()) {
        m_window->addItem(tr("QEMU's default"), QString());
    } else if (m_window->findData(m_loaded.display) < 0) {
        m_window->addItem(m_loaded.display, m_loaded.display);
    }
    m_window->setCurrentIndex(m_window->findData(m_loaded.display));

    m_custom->setText(tr("The graphics of this VM are set by hand (%1): change them on the "
                         "Arguments page. The window can change here.")
                          .arg(m_loaded.custom.toHtmlEscaped()));
    m_custom->setVisible(m_loaded.kind == Kind::Custom);
    update();
}

VmConfig::Graphics DisplayPage::shown() const
{
    VmConfig::Graphics g = m_loaded;

    g.kind = VmConfig::Graphics::Kind(m_kind->currentData().toInt());
    g.device = m_device->count() > 0 ? m_device->currentData().toString() : QString();
    g.nativeContext = g.kind == VmConfig::Graphics::Accelerated && m_nativeContext->isChecked();
    g.venus = g.kind == VmConfig::Graphics::Accelerated && m_venus->isChecked();
    /* as written, unless changed */
    if (m_hostmem->value() != m_loadedHostmemGiB) {
        g.hostmemMiB = qint64(m_hostmem->value()) * 1024;
    }
    g.display = m_window->currentData().toString();
    return g;
}

void DisplayPage::save(ArgsFile &args)
{
    VmConfig::setGraphics(args, shown());
    load(args);
}

bool DisplayPage::isModified() const
{
    const VmConfig::Graphics g = shown();

    return g.kind != m_loaded.kind || (!g.device.isEmpty() && g.device != m_loaded.device) ||
           g.nativeContext != m_loaded.nativeContext || g.venus != m_loaded.venus ||
           ((g.nativeContext || g.venus) && m_hostmem->value() != m_loadedHostmemGiB) ||
           g.display != m_loaded.display;
}

/* Storage */

StoragePage::StoragePage(const QString &vmDir, QWidget *parent)
    : SettingsPage(parent), m_vmDir(vmDir), m_table(new QTableWidget(0, 4)),
      m_disc(new QPushButton(tr("Choose &Disc…"))), m_eject(new QPushButton(tr("&Eject"))),
      m_resize(new QPushButton(tr("Si&ze…"))), m_remove(new QPushButton(tr("&Remove")))
{
    auto *layout = new QVBoxLayout(this);
    auto *tableRow = new QHBoxLayout;
    auto *buttons = new QVBoxLayout;
    auto *addDisk = new QPushButton(tr("Add Hard Dis&k…"));
    auto *addCdrom = new QPushButton(tr("Add &CD/DVD Drive"));

    m_table->setObjectName("disks");
    m_table->setHorizontalHeaderLabels({tr("Type"), tr("File"), tr("Bus"), tr("Note")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);

    addDisk->setIcon(Icons::themed({"drive-harddisk", "list-add"}, QStyle::SP_DriveHDIcon));
    addCdrom->setIcon(Icons::themed({"drive-optical", "list-add"}, QStyle::SP_DriveCDIcon));
    m_remove->setIcon(Icons::themed({"list-remove", "edit-delete"}, QStyle::SP_TrashIcon));
    /* beside the table, as a row under it would not fit a narrow window */
    buttons->addWidget(addDisk);
    buttons->addWidget(addCdrom);
    buttons->addSpacing(buttons->spacing() * 2);
    buttons->addWidget(m_disc);
    buttons->addWidget(m_eject);
    buttons->addWidget(m_resize);
    buttons->addWidget(m_remove);
    buttons->addStretch();
    tableRow->addWidget(m_table, 1);
    tableRow->addLayout(buttons);

    layout->addWidget(Widgets::note(tr("The disks and CD/DVD drives of the VM. New disks are "
                                       "created in the VM folder when you apply; removing a "
                                       "disk takes it out of the VM, but its file stays.")));
    layout->addLayout(tableRow, 1);

    connect(addDisk, &QPushButton::clicked, this, &StoragePage::addDisk);
    connect(addCdrom, &QPushButton::clicked, this, &StoragePage::addCdrom);
    connect(m_disc, &QPushButton::clicked, this, &StoragePage::chooseDisc);
    connect(m_resize, &QPushButton::clicked, this, &StoragePage::resize);
    connect(m_eject, &QPushButton::clicked, this, [this]() {
        if (const int i = current(); i >= 0) {
            m_entries[i].file.clear();
            fill();
        }
    });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        const int i = current();
        if (i < 0) {
            return;
        }
        if (m_entries[i].disk.line < 0) {
            m_entries.removeAt(i);
        } else {
            m_entries[i].removed = true;
        }
        fill();
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, &StoragePage::updateButtons);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this]() {
        if (m_disc->isEnabled()) {
            chooseDisc();
        } else if (m_resize->isEnabled()) {
            resize();
        }
    });
}

QIcon StoragePage::icon() const
{
    return Icons::themed({"drive-harddisk"}, QStyle::SP_DriveHDIcon);
}

void StoragePage::load(const ArgsFile &args)
{
    m_virt = VmConfig::machineType(args).startsWith("virt");
    m_entries.clear();
    for (const VmConfig::Disk &d : VmConfig::disks(args)) {
        m_entries << Entry{d, d.file, m_pending.value(d.file), false};
    }
    fill();
}

void StoragePage::save(ArgsFile &args)
{
    QList<VmConfig::Disk> removed;

    /* discs, which move no line */
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.disk.line >= 0 && !e.removed && e.disk.cdrom && e.file != e.disk.file) {
            VmConfig::setDisc(args, e.disk, e.file);
        }
    }
    /* then the removals, from the bottom */
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.removed) {
            removed << e.disk;
        }
    }
    std::sort(removed.begin(), removed.end(), [](const auto &a, const auto &b) {
        return qMax(a.line, a.deviceLine) > qMax(b.line, b.deviceLine);
    });
    for (const VmConfig::Disk &d : std::as_const(removed)) {
        VmConfig::removeDisk(args, d);
    }
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.disk.line >= 0 || e.removed) {
            continue;
        }
        if (e.disk.cdrom) {
            VmConfig::addCdrom(args, e.file);
        } else {
            VmConfig::addDisk(args, e.file, e.disk.bus);
            if (e.newGiB > 0) {
                m_pending[e.file] = e.newGiB;
            }
        }
    }
    load(args);
}

bool StoragePage::isModified() const
{
    for (const Entry &e : m_entries) {
        if (e.removed || e.disk.line < 0 || e.file != e.disk.file ||
            (e.newGiB > 0 && e.newGiB != m_pending.value(e.file))) {
            return true;
        }
    }
    return false;
}

bool StoragePage::commit(const ArgsFile &args, const QString &vmDir, QString *error)
{
    QStringList used;

    for (const VmConfig::Disk &d : VmConfig::disks(args)) {
        used << d.file;
    }
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const QString path = QDir(vmDir).filePath(it.key());
        if (used.contains(it.key()) && !QFileInfo::exists(path) &&
            !createDiskImage(path, qint64(it.value()) << 30, error)) {
            return false;
        }
        it = m_pending.erase(it);
    }
    return true;
}

int StoragePage::current() const
{
    const int row = m_table->currentRow();
    int shown = -1;

    for (int i = 0; i < m_entries.size(); i++) {
        if (!m_entries[i].removed && ++shown == row) {
            return i;
        }
    }
    return -1;
}

void StoragePage::fill()
{
    const int row = m_table->currentRow();
    int n = 0;

    m_table->setRowCount(0);
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.removed) {
            continue;
        }
        const QString path = QDir(m_vmDir).absoluteFilePath(e.file);
        QString note;

        if (e.newGiB > 0) {
            note = tr("New, %1 GiB: created when applied").arg(e.newGiB);
        } else if (!e.disk.editable) {
            note = tr("Set by hand: see the Arguments page");
        } else if (!e.file.isEmpty() && !QFileInfo::exists(path)) {
            note = tr("Not found");
        } else if (e.disk.cdrom && e.file.isEmpty()) {
            note = tr("Empty");
        }
        m_table->insertRow(n);
        m_table->setItem(n, 0, new QTableWidgetItem(
                                   Icons::themed({e.disk.cdrom ? "drive-optical" : "drive-harddisk"},
                                                 e.disk.cdrom ? QStyle::SP_DriveCDIcon
                                                              : QStyle::SP_DriveHDIcon),
                                   e.disk.cdrom ? tr("CD/DVD") : tr("Hard disk")));
        m_table->setItem(n, 1, new QTableWidgetItem(QDir::toNativeSeparators(e.file)));
        m_table->setItem(n, 2, new QTableWidgetItem(UiConfig::busName(e.disk.bus)));
        m_table->setItem(n, 3, new QTableWidgetItem(note));
        m_table->item(n, 1)->setToolTip(path);
        if (!e.disk.editable) {
            for (int c = 0; c < 4; c++) {
                m_table->item(n, c)->setFlags(Qt::ItemIsSelectable);
            }
        }
        n++;
    }
    if (n > 0) {
        m_table->setCurrentCell(qBound(0, row, n - 1), 0);
    }
    updateButtons();
}

void StoragePage::updateButtons()
{
    const int i = current();
    const bool editable = i >= 0 && m_entries[i].disk.editable;
    const bool cdrom = editable && m_entries[i].disk.cdrom;

    m_disc->setEnabled(cdrom);
    m_eject->setEnabled(cdrom && !m_entries[i].file.isEmpty());
    m_resize->setEnabled(editable && m_entries[i].newGiB > 0);
    m_remove->setEnabled(editable);
}

QString StoragePage::newDiskName() const
{
    const QDir dir(m_vmDir);

    for (int n = 1;; n++) {
        const QString name = n == 1 ? QString("disk.qcow2") : QString("disk%1.qcow2").arg(n);
        bool used = dir.exists(name) || m_pending.contains(name);
        for (const Entry &e : m_entries) {
            used |= e.file == name;
        }
        if (!used) {
            return name;
        }
    }
}

void StoragePage::addDisk()
{
    QDialog dialog(this);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = Widgets::form();
    auto *newRow = new QHBoxLayout;
    auto *existingRow = new QHBoxLayout;
    auto *create = new QRadioButton(tr("Create a &new disk of"));
    auto *size = new QSpinBox;
    auto *existing = new QRadioButton(tr("&Use an existing disk image:"));
    auto *path = new QLineEdit;
    auto *bus = new QComboBox;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *group = new QButtonGroup(&dialog);

    dialog.setWindowTitle(tr("Add a Hard Disk"));
    size->setRange(1, 65536);
    size->setValue(64);
    size->setSuffix(tr(" GiB"));
    group->addButton(create);
    group->addButton(existing);
    create->setChecked(true);
    newRow->addWidget(create);
    newRow->addWidget(size);
    newRow->addStretch();
    existingRow->addWidget(existing);
    existingRow->addWidget(Widgets::browseRow(path, tr("Disk Image"),
                                              tr("Disk images (*.qcow2 *.img *.raw *.vmdk "
                                                 "*.vdi *.vhdx *.vhd);;All files (*)")),
                           1);
    bus->addItem(tr("VirtIO: the fastest, Windows needs its drivers"),
                 int(VmConfig::Disk::Virtio));
    /* virt has no SATA */
    if (m_virt) {
        bus->addItem(tr("SCSI (virtio-scsi)"), int(VmConfig::Disk::Scsi));
    } else {
        bus->addItem(tr("SATA: works without drivers, e.g. on Windows"),
                     int(VmConfig::Disk::Sata));
    }
    form->addRow(tr("Disk:"), newRow);
    form->addRow(QString(), existingRow);
    form->addRow(tr("&Bus:"), bus);
    layout->addLayout(form);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto update = [&]() {
        size->setEnabled(create->isChecked());
        path->parentWidget()->setEnabled(existing->isChecked());
    };
    connect(group, &QButtonGroup::buttonToggled, &dialog, update);
    update();
    dialog.resize(560, dialog.sizeHint().height());

    while (dialog.exec() == QDialog::Accepted) {
        Entry e;
        e.disk.cdrom = false;
        e.disk.bus = VmConfig::Disk::Bus(bus->currentData().toInt());
        if (create->isChecked()) {
            e.file = newDiskName();
            e.newGiB = size->value();
        } else if (QFileInfo(path->text().trimmed()).isFile()) {
            e.file = QFileInfo(path->text().trimmed()).absoluteFilePath();
        } else {
            Widgets::inform(&dialog, dialog.windowTitle(),
                            tr("Choose the disk image to use."));
            continue;
        }
        m_entries << e;
        fill();
        m_table->setCurrentCell(m_table->rowCount() - 1, 0);
        return;
    }
}

void StoragePage::addCdrom()
{
    Entry e;

    e.disk.cdrom = true;
    e.disk.bus = m_virt ? VmConfig::Disk::Scsi : VmConfig::Disk::Sata;
    m_entries << e;
    fill();
    m_table->setCurrentCell(m_table->rowCount() - 1, 0);
    chooseDisc();
}

void StoragePage::chooseDisc()
{
    const int i = current();

    if (i < 0) {
        return;
    }
    const QString start = m_entries[i].file.isEmpty()
                              ? QDir::homePath()
                              : QFileInfo(QDir(m_vmDir).absoluteFilePath(m_entries[i].file)).path();
    const QString iso = QFileDialog::getOpenFileName(this, tr("Disc Image"), start,
                                                     tr("Disc images (*.iso);;All files (*)"));
    if (!iso.isEmpty()) {
        m_entries[i].file = iso;
        fill();
    }
}

void StoragePage::resize()
{
    const int i = current();
    bool ok = false;

    if (i < 0 || m_entries[i].newGiB <= 0) {
        return;
    }
    const int size = QInputDialog::getInt(this, tr("New Disk"), tr("Size in GiB:"),
                                          m_entries[i].newGiB, 1, 65536, 1, &ok);
    if (ok) {
        m_entries[i].newGiB = size;
        if (m_entries[i].disk.line >= 0) {
            /* added already: the size waits with the name */
            m_pending[m_entries[i].file] = size;
        }
        fill();
    }
}

/* Shared folders */

static const char *const kCacheModes[][2] = {
    {"auto", QT_TRANSLATE_NOOP("SharesPage", "Automatic")},
    {"always", QT_TRANSLATE_NOOP("SharesPage", "Always")},
    {"never", QT_TRANSLATE_NOOP("SharesPage", "Never")},
};

static QString cacheName(const QString &mode)
{
    for (const auto &m : kCacheModes) {
        if (mode == m[0]) {
            return QCoreApplication::translate("SharesPage", m[1]);
        }
    }
    return mode;
}

static bool sameShares(const QList<VmConfig::Share> &a, const QList<VmConfig::Share> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); i++) {
        if (a[i].tag != b[i].tag || a[i].path != b[i].path || a[i].cache != b[i].cache ||
            a[i].readonly != b[i].readonly || a[i].mount != b[i].mount) {
            return false;
        }
    }
    return true;
}

SharesPage::SharesPage(QWidget *parent)
    : SettingsPage(parent), m_virtiofsd(new Banner(Banner::Warning)),
      m_memory(new Banner(Banner::Information)), m_table(new QTableWidget(0, 5)),
      m_edit(new QPushButton(tr("&Edit…"))), m_remove(new QPushButton(tr("&Remove"))),
      m_mount(new QLabel)
{
    auto *layout = new QVBoxLayout(this);
    auto *tableRow = new QHBoxLayout;
    auto *buttons = new QVBoxLayout;
    auto *add = new QPushButton(tr("&Add…"));

    m_table->setObjectName("shares");
    m_table->setHorizontalHeaderLabels(
        {tr("Name in the guest"), tr("Folder"), tr("Mounted at"), tr("Cache"), tr("Access")});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);

    add->setIcon(Icons::themed({"list-add"}, QStyle::SP_FileDialogNewFolder));
    m_remove->setIcon(Icons::themed({"list-remove", "edit-delete"}, QStyle::SP_TrashIcon));
    buttons->addWidget(add);
    buttons->addWidget(m_edit);
    buttons->addWidget(m_remove);
    buttons->addStretch();
    tableRow->addWidget(m_table, 1);
    tableRow->addLayout(buttons);

    /* prose that wraps, and commands to copy: the page does not scroll across */
    m_mount->setTextFormat(Qt::RichText);
    m_mount->setWordWrap(true);
    m_mount->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(m_virtiofsd);
    layout->addWidget(m_memory);
    layout->addWidget(Widgets::note(tr("Folders of this computer the VM can use. They are shared "
                              "with virtiofs, which is fast and works with Linux 5.4 or "
                              "later in the guest.")));
    layout->addLayout(tableRow, 1);
    layout->addWidget(Widgets::heading(tr("In the guest")));
    layout->addWidget(m_mount);
    layout->addWidget(Widgets::hint(tr("Windows guests need the virtio-win drivers and WinFsp.")));

    m_virtiofsd->setText(tr("virtiofsd was not found, so shared folders will not work. "
                            "Install it with <code>sudo dnf install virtiofsd</code>, or "
                            "set its path in the preferences."));
    m_memory->button()->setText(tr("&Use Shared Memory"));
    connect(m_memory->button(), &QPushButton::clicked, this, [this]() {
        m_fixMemory = true;
        updateHints();
    });

    connect(add, &QPushButton::clicked, this, [this]() { edit(-1); });
    connect(m_edit, &QPushButton::clicked, this, [this]() { edit(m_table->currentRow()); });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row) { edit(row); });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        const int row = m_table->currentRow();
        if (row >= 0) {
            m_shares.removeAt(row);
            fill();
        }
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, &SharesPage::updateHints);
}

QIcon SharesPage::icon() const
{
    return Icons::themed({"folder-network", "folder-remote"}, QStyle::SP_DirIcon);
}

void SharesPage::load(const ArgsFile &args)
{
    m_shares = VmConfig::shares(args);
    m_loaded = m_shares;
    m_sharedMemory = VmConfig::hasSharedMemory(args);
    m_fixMemory = false;
    fill();
}

void SharesPage::save(ArgsFile &args)
{
    if (!sameShares(m_shares, m_loaded)) {
        VmConfig::setShares(args, m_shares);
        m_loaded = m_shares;
    }
    if (!m_shares.isEmpty() && m_fixMemory && !VmConfig::hasSharedMemory(args)) {
        VmConfig::useSharedMemory(args);
    }
    m_sharedMemory = VmConfig::hasSharedMemory(args);
    m_fixMemory = false;
}

bool SharesPage::isModified() const
{
    return !sameShares(m_shares, m_loaded) || m_fixMemory;
}

void SharesPage::fill()
{
    const int row = m_table->currentRow();

    m_table->setRowCount(int(m_shares.size()));
    for (int i = 0; i < m_shares.size(); i++) {
        const VmConfig::Share &s = m_shares[i];
        m_table->setItem(i, 0, new QTableWidgetItem(s.tag));
        m_table->setItem(i, 1, new QTableWidgetItem(QDir::toNativeSeparators(s.path)));
        m_table->setItem(i, 2, new QTableWidgetItem(s.mount.isEmpty() ? tr("By hand")
                                                                       : s.mount));
        m_table->setItem(i, 3, new QTableWidgetItem(cacheName(s.cache)));
        m_table->setItem(i, 4, new QTableWidgetItem(s.readonly ? tr("Read only")
                                                               : tr("Read and write")));
        m_table->item(i, 1)->setToolTip(s.path);
    }
    if (!m_shares.isEmpty()) {
        m_table->setCurrentCell(qBound(0, row, int(m_shares.size()) - 1), 0);
    }
    updateHints();
}

/* Commands to copy, in the fixed font */
static QString commands(const QStringList &lines)
{
    return QString("<pre style=\"font-family: '%1'\">%2</pre>")
        .arg(QFontDatabase::systemFont(QFontDatabase::FixedFont).family(),
             lines.join('\n').toHtmlEscaped());
}

void SharesPage::updateHints()
{
    const int row = m_table->currentRow();
    const bool selected = row >= 0 && row < m_shares.size();

    m_edit->setEnabled(selected);
    m_remove->setEnabled(selected);
    m_virtiofsd->setVisible(!m_shares.isEmpty() && Paths::virtiofsd().isEmpty());

    if (m_shares.isEmpty() || m_sharedMemory) {
        m_memory->hide();
    } else if (m_fixMemory) {
        m_memory->setText(tr("The guest memory will come from shared memory (memfd), which "
                             "virtiofs needs. Its size stays the same."));
        m_memory->button()->hide();
        m_memory->show();
    } else {
        m_memory->setText(tr("virtiofs needs the guest memory to be shared memory, which it "
                             "is not in the arguments of this VM."));
        m_memory->button()->show();
        m_memory->show();
    }

    const QString tag = selected ? m_shares[row].tag : QString("TAG");
    if (selected && !m_shares[row].mount.isEmpty()) {
        const QString mount = m_shares[row].mount;
        m_mount->setText(
            "<p>" +
            tr("The guest mounts it at <b>%1</b> at each start. systemd 254 and later do so at "
               "boot (Fedora 39, Debian 13, Ubuntu 24.04 and later); older guests need the QEMU "
               "guest agent, the qemu-guest-agent package, which starts by itself once "
               "installed.")
                .arg(mount.toHtmlEscaped()) +
            "</p><p>" + tr("By hand instead:") + "</p>" +
            commands({QString("sudo mount -t virtiofs %1 %2").arg(tag, mount)}));
        return;
    }
    const QString mount = "/mnt/" + tag;
    m_mount->setText("<p>" + tr("Mount it:") + "</p>" +
                     commands({QString("sudo mkdir -p %1").arg(mount),
                               QString("sudo mount -t virtiofs %1 %2").arg(tag, mount)}) +
                     "<p>" + tr("Or mount it at boot, with this line in /etc/fstab:") + "</p>" +
                     commands({QString("%1 %2 virtiofs defaults,nofail 0 0").arg(tag, mount)}));
}

void SharesPage::edit(int row)
{
    QStringList others;
    const bool adding = row < 0 || row >= m_shares.size();

    for (int i = 0; i < m_shares.size(); i++) {
        if (i != row) {
            others << m_shares[i].tag;
        }
    }
    ShareDialog dialog(adding ? VmConfig::Share() : m_shares[row], others, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (adding) {
        m_shares << dialog.share();
        if (!m_sharedMemory) {
            /* virtiofs needs it: say so rather than ask */
            m_fixMemory = true;
        }
        fill();
        m_table->setCurrentCell(int(m_shares.size()) - 1, 0);
    } else {
        m_shares[row] = dialog.share();
        fill();
    }
}

ShareDialog::ShareDialog(const VmConfig::Share &share, const QStringList &otherTags,
                         QWidget *parent)
    : QDialog(parent), m_path(new QLineEdit), m_tag(new QLineEdit), m_cache(new QComboBox),
      m_cacheInfo(Widgets::hint()), m_readonly(new QCheckBox(tr("&Read only: the VM cannot change "
                                                       "the files"))),
      m_mount(new QCheckBox(tr("&Mount it in the guest at start, at:"))), m_mountDir(new QLineEdit),
      m_error(new QLabel), m_otherTags(otherTags)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

    setWindowTitle(share.tag.isEmpty() ? tr("Add a Shared Folder") : tr("Edit Shared Folder"));
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_path->setObjectName("path");
    m_tag->setObjectName("tag");
    for (const auto &mode : kCacheModes) {
        m_cache->addItem(QCoreApplication::translate("SharesPage", mode[1]), mode[0]);
    }
    m_error->setWordWrap(true);
    {
        QPalette palette = m_error->palette();
        palette.setColor(QPalette::WindowText, QColor(0xda, 0x44, 0x53));
        m_error->setPalette(palette);
    }

    form->addRow(Widgets::label(tr("&Folder:"), m_path),
                 Widgets::browseRow(m_path, tr("Folder to Share"), {}, true));
    form->addRow(tr("&Name in the guest:"), m_tag);
    form->addRow(QString(), Widgets::hint(tr("The guest mounts the folder by this name.")));
    form->addRow(tr("&Cache:"), m_cache);
    form->addRow(QString(), m_cacheInfo);
    form->addRow(QString(), m_readonly);
    {
        auto *mountRow = new QHBoxLayout;
        mountRow->addWidget(m_mount);
        mountRow->addWidget(m_mountDir, 1);
        form->addRow(QString(), mountRow);
    }
    form->addRow(QString(), Widgets::hint(tr("systemd 254 and later mount it at boot; older "
                                             "guests need the QEMU guest agent "
                                             "(qemu-guest-agent).")));
    layout->addLayout(form);
    layout->addWidget(m_error);
    layout->addStretch();
    layout->addWidget(buttons);

    m_path->setText(share.path);
    m_tag->setText(share.tag);
    m_tagEdited = !share.tag.isEmpty();
    m_cache->setCurrentIndex(qMax(0, m_cache->findData(share.cache)));
    m_readonly->setChecked(share.readonly);
    /* new folders are mounted at /mnt/NAME, following the name */
    m_mount->setChecked(share.tag.isEmpty() || !share.mount.isEmpty());
    m_mountEdited = !share.mount.isEmpty();
    m_mountDir->setObjectName("mount");
    m_mountDir->setText(share.mount.isEmpty() ? "/mnt/" + share.tag : share.mount);
    m_mountDir->setEnabled(m_mount->isChecked());

    connect(m_path, &QLineEdit::textChanged, this, [this](const QString &path) {
        if (!m_tagEdited) {
            static const QRegularExpression unsafe("[^A-Za-z0-9_.-]+");
            QString tag = QFileInfo(QDir::cleanPath(path)).fileName();
            tag.replace(unsafe, "-");
            const QSignalBlocker block(m_tag);
            m_tag->setText(tag.left(36));
            if (!m_mountEdited) {
                m_mountDir->setText("/mnt/" + m_tag->text());
            }
        }
        validate();
    });
    connect(m_tag, &QLineEdit::textEdited, this, [this]() { m_tagEdited = true; });
    connect(m_tag, &QLineEdit::textChanged, this, [this](const QString &tag) {
        if (!m_mountEdited) {
            m_mountDir->setText("/mnt/" + tag.trimmed());
        }
        validate();
    });
    connect(m_mountDir, &QLineEdit::textEdited, this, [this]() { m_mountEdited = true; });
    connect(m_mountDir, &QLineEdit::textChanged, this, &ShareDialog::validate);
    connect(m_mount, &QCheckBox::toggled, this, [this](bool on) {
        m_mountDir->setEnabled(on);
        validate();
    });
    auto describeCache = [this]() {
        const QString mode = m_cache->currentData().toString();
        if (mode == "auto") {
            m_cacheInfo->setText(tr("The guest keeps files in its cache for a moment: changes "
                                    "made on this computer show up within a second."));
        } else if (mode == "always") {
            m_cacheInfo->setText(tr("The fastest. The guest keeps files in its cache, so "
                                    "changes made on this computer may not show up in the "
                                    "guest. Best when only the VM uses the folder."));
        } else {
            m_cacheInfo->setText(tr("Always up to date with this computer, but slower."));
        }
    };
    connect(m_cache, &QComboBox::currentIndexChanged, this, describeCache);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    describeCache();
    validate();
    resize(560, sizeHint().height());
}

VmConfig::Share ShareDialog::share() const
{
    VmConfig::Share s;

    s.tag = m_tag->text().trimmed();
    s.path = QDir::cleanPath(m_path->text().trimmed());
    s.cache = m_cache->currentData().toString();
    s.readonly = m_readonly->isChecked();
    s.mount = m_mount->isChecked() ? QDir::cleanPath(m_mountDir->text().trimmed()) : QString();
    return s;
}

void ShareDialog::validate()
{
    static const QRegularExpression valid("^[A-Za-z0-9_.-]{1,36}$");
    const QString path = m_path->text().trimmed();
    const QString tag = m_tag->text().trimmed();
    QString error;

    if (path.isEmpty()) {
        error = " ";
    } else if (!QFileInfo(path).isDir()) {
        error = tr("This folder does not exist.");
    } else if (tag.isEmpty()) {
        error = tr("Give the folder a name for the guest.");
    } else if (!valid.match(tag).hasMatch()) {
        error = tr("The name can have up to 36 letters, digits, dots, dashes and "
                   "underscores.");
    } else if (m_otherTags.contains(tag)) {
        error = tr("Another shared folder has this name.");
    } else if (m_mount->isChecked() && !m_mountDir->text().trimmed().startsWith('/')) {
        error = tr("The guest mounts it at a full path, like /mnt/%1.").arg(tag);
    }
    m_error->setText(error.trimmed());
    m_ok->setEnabled(error.isEmpty());
}

/* PCI */

PciPage::PciPage(QWidget *parent)
    : SettingsPage(parent), m_banner(new Banner(Banner::Warning)), m_tree(new QTreeWidget)
{
    auto *layout = new QVBoxLayout(this);

    m_tree->setObjectName("pci");
    m_tree->setHeaderLabels({tr("Device"), tr("Driver"), tr("Status")});
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setItemsExpandable(false);

    layout->addWidget(m_banner);
    layout->addWidget(Widgets::note(tr("A device passed through belongs to the VM, which drives it "
                              "directly: this computer cannot use it while the VM runs. "
                              "The devices of an IOMMU group go together, and the "
                              "functions of a card, such as a graphics card and its "
                              "sound, are checked together.")));
    layout->addWidget(m_tree, 1);

    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        const QString address = item->data(0, Qt::UserRole).toString();
        if (column != 0 || address.isEmpty() || item->checkState(0) != Qt::Checked) {
            return;
        }
        /* the other functions of the card: 0000:03:00.0 and 0000:03:00.1 */
        const QString slot = address.section('.', 0, 0);
        const QSignalBlocker block(m_tree);
        for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
            const QString other = (*it)->data(0, Qt::UserRole).toString();
            if (!other.isEmpty() && other.section('.', 0, 0) == slot) {
                (*it)->setCheckState(0, Qt::Checked);
            }
        }
    });
}

QIcon PciPage::icon() const
{
    return Icons::themed({"preferences-desktop-peripherals", "video-display"},
                         QStyle::SP_DriveHDIcon);
}

void PciPage::load(const ArgsFile &args)
{
    const QList<PciDevice> devices = HostDevices::pciDevices();
    const qint64 memory = VmConfig::memoryMiB(args);
    QMap<int, QTreeWidgetItem *> groups;
    QStringList missing;
    const QSignalBlocker block(m_tree);

    m_loaded = VmConfig::pciPassthrough(args);
    missing = m_loaded;
    m_tree->clear();

    if (!HostDevices::iommuEnabled()) {
        m_banner->setText(tr("The IOMMU is off, so no device can be passed through. Turn on "
                             "VT-d (Intel) or AMD-Vi in the firmware settings of this "
                             "computer, and add <code>intel_iommu=on</code> or "
                             "<code>amd_iommu=on</code> to the kernel command line."));
        m_banner->show();
    } else if (devices.isEmpty()) {
        m_banner->setText(tr("No PCI devices were found."));
        m_banner->show();
    } else {
        m_banner->hide();
    }

    for (const PciDevice &dev : devices) {
        if ((dev.classCode >> 16) == 0x06) {
            /* bridges stay with the host */
            continue;
        }
        QTreeWidgetItem *&group = groups[dev.iommuGroup];
        if (!group) {
            group = new QTreeWidgetItem(
                {dev.iommuGroup < 0 ? tr("No IOMMU group")
                                    : tr("IOMMU group %1").arg(dev.iommuGroup)});
            group->setFlags(Qt::ItemIsEnabled);
            group->setFirstColumnSpanned(true);
        }

        const QStringList problems = HostDevices::pciProblems(dev, memory);
        auto *item = new QTreeWidgetItem(group, {dev.displayName(), dev.driver,
                                                 problems.isEmpty() ? tr("Ready")
                                                                    : problems.first()});
        item->setData(0, Qt::UserRole, dev.address);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_loaded.contains(dev.address) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(0, QString("%1 [%2:%3]\n%4")
                                .arg(dev.address)
                                .arg(dev.vendorId, 4, 16, QChar('0'))
                                .arg(dev.deviceId, 4, 16, QChar('0'))
                                .arg(dev.className));
        item->setToolTip(2, problems.isEmpty() ? tr("Ready to be passed through")
                                               : problems.join('\n'));
        missing.removeAll(dev.address);
    }
    for (QTreeWidgetItem *group : std::as_const(groups)) {
        m_tree->addTopLevelItem(group);
        group->setExpanded(true);
    }
    if (!missing.isEmpty()) {
        auto *group = new QTreeWidgetItem(m_tree, {tr("Not on this computer")});
        group->setFlags(Qt::ItemIsEnabled);
        group->setFirstColumnSpanned(true);
        for (const QString &address : std::as_const(missing)) {
            auto *item = new QTreeWidgetItem(group, {address, QString(), tr("Not found")});
            item->setData(0, Qt::UserRole, address);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            item->setCheckState(0, Qt::Checked);
        }
        group->setExpanded(true);
    }
    if (m_tree->topLevelItemCount() == 0) {
        auto *empty = new QTreeWidgetItem(m_tree, {tr("No devices to show")});
        empty->setFlags(Qt::NoItemFlags);
    }
    m_tree->resizeColumnToContents(0);
    m_tree->resizeColumnToContents(1);
}

QStringList PciPage::checked() const
{
    QStringList list;

    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const QString address = (*it)->data(0, Qt::UserRole).toString();
        if (!address.isEmpty() && (*it)->checkState(0) == Qt::Checked) {
            list << address;
        }
    }
    return list;
}

void PciPage::save(ArgsFile &args)
{
    const QStringList list = checked();

    if (list != m_loaded) {
        VmConfig::setPciPassthrough(args, list);
        m_loaded = list;
    }
}

bool PciPage::isModified() const
{
    return checked() != m_loaded;
}

/* USB */

UsbPage::UsbPage(QWidget *parent)
    : SettingsPage(parent), m_controller(new Banner(Banner::Warning)), m_tree(new QTreeWidget)
{
    auto *layout = new QVBoxLayout(this);

    m_tree->setObjectName("usb");
    m_tree->setHeaderLabels({tr("Device"), tr("ID"), tr("Status")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);

    m_controller->button()->setText(tr("&Add a USB Controller"));
    connect(m_controller->button(), &QPushButton::clicked, this, [this]() {
        m_addController = true;
        m_controller->button()->hide();
        m_controller->setText(tr("A USB 3 controller (qemu-xhci) will be added."));
    });

    layout->addWidget(m_controller);
    layout->addWidget(Widgets::note(tr("The checked devices are given to the VM when it starts, by "
                              "their vendor and product ID: this computer cannot use them "
                              "while the VM runs. The menu of the VM window can attach "
                              "devices while it runs too.")));
    layout->addWidget(m_tree, 1);
}

QIcon UsbPage::icon() const
{
    return Icons::themed({"drive-removable-media-usb", "media-removable"},
                         QStyle::SP_DriveFDIcon);
}

void UsbPage::load(const ArgsFile &args)
{
    const QList<UsbDevice> devices = HostDevices::usbDevices();
    QList<VmConfig::UsbId> missing;

    m_loaded = VmConfig::usbPassthrough(args);
    missing = m_loaded;
    m_addController = false;
    m_tree->clear();

    if (VmConfig::hasUsbController(args)) {
        m_controller->hide();
    } else {
        m_controller->setText(tr("This VM has no USB controller, so it cannot use USB "
                                 "devices."));
        m_controller->button()->show();
        m_controller->show();
    }

    for (const UsbDevice &dev : devices) {
        const VmConfig::UsbId id{dev.vendorId, dev.productId};
        const bool access = QFileInfo(dev.devNode()).isWritable();
        QString name = QString("%1 %2").arg(dev.manufacturer, dev.product).simplified();

        if (dev.isHub) {
            continue;
        }
        if (name.isEmpty()) {
            name = tr("Unknown device");
        }
        auto *item = new QTreeWidgetItem(
            m_tree, {name,
                     QString("%1:%2")
                         .arg(dev.vendorId, 4, 16, QChar('0'))
                         .arg(dev.productId, 4, 16, QChar('0')),
                     access ? tr("Ready") : tr("No access")});
        item->setData(0, Qt::UserRole, dev.vendorId);
        item->setData(0, Qt::UserRole + 1, dev.productId);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_loaded.contains(id) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(0, tr("Bus %1, port %2").arg(dev.bus).arg(dev.port));
        if (!access) {
            item->setToolTip(
                2, tr("QEMU cannot open %1. Give your user access with a udev rule, e.g. in "
                      "/etc/udev/rules.d/70-qemu-usb.rules:\n"
                      "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"%2\", ATTR{idProduct}==\"%3\", "
                      "TAG+=\"uaccess\"")
                       .arg(dev.devNode())
                       .arg(dev.vendorId, 4, 16, QChar('0'))
                       .arg(dev.productId, 4, 16, QChar('0')));
        }
        missing.removeAll(id);
    }
    for (const VmConfig::UsbId &id : std::as_const(missing)) {
        auto *item = new QTreeWidgetItem(
            m_tree, {tr("Not connected"),
                     QString("%1:%2")
                         .arg(id.vendor, 4, 16, QChar('0'))
                         .arg(id.product, 4, 16, QChar('0')),
                     tr("Attached when connected before the VM starts")});
        item->setData(0, Qt::UserRole, id.vendor);
        item->setData(0, Qt::UserRole + 1, id.product);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);
    }
    if (m_tree->topLevelItemCount() == 0) {
        auto *empty = new QTreeWidgetItem(m_tree, {tr("No USB devices found")});
        empty->setFlags(Qt::NoItemFlags);
    }
    m_tree->resizeColumnToContents(0);
    m_tree->resizeColumnToContents(1);
}

QList<VmConfig::UsbId> UsbPage::checked() const
{
    QList<VmConfig::UsbId> list;

    for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
        const QTreeWidgetItem *item = m_tree->topLevelItem(i);
        if (item->flags() & Qt::ItemIsUserCheckable && item->checkState(0) == Qt::Checked) {
            list << VmConfig::UsbId{quint16(item->data(0, Qt::UserRole).toUInt()),
                                    quint16(item->data(0, Qt::UserRole + 1).toUInt())};
        }
    }
    return list;
}

void UsbPage::save(ArgsFile &args)
{
    const QList<VmConfig::UsbId> list = checked();

    if (list != m_loaded) {
        VmConfig::setUsbPassthrough(args, list);
        m_loaded = list;
    }
    if (m_addController && !VmConfig::hasUsbController(args)) {
        args.add("device", "qemu-xhci");
    }
    m_addController = false;
}

bool UsbPage::isModified() const
{
    return checked() != m_loaded || m_addController;
}

/* Arguments */

ArgumentsPage::ArgumentsPage(const QString &vmDir, QWidget *parent)
    : SettingsPage(parent), m_pane(new ArgsEditorPane)
{
    m_pane->setVmDir(vmDir);
    auto *layout = new QVBoxLayout(this);
    auto *splitter = new Splitter(Qt::Horizontal);
    auto *reference = new ReferencePanel;

    m_pane->setObjectName("argsPane");
    reference->setObjectName("reference");
    reference->setEditor(m_pane->editor());
    connect(m_pane, &ArgsEditorPane::docsChanged, reference, &ReferencePanel::setDocs);
    splitter->addWidget(m_pane);
    splitter->addWidget(reference);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);

    layout->addWidget(Widgets::note(tr("The QEMU command line of this VM, one option per line; lines "
                              "starting with # are comments. The other pages edit these "
                              "same lines. Ctrl+Space completes option and device names.")));
    layout->addWidget(splitter, 1);
}

QIcon ArgumentsPage::icon() const
{
    return Icons::themed({"utilities-terminal", "text-x-script"}, QStyle::SP_FileIcon);
}

void ArgumentsPage::load(const ArgsFile &args)
{
    m_loaded = args.toText();
    m_pane->editor()->setPlainText(m_loaded);
    /* the documentation of the VM's QEMU, at once */
    m_pane->check();
}

void ArgumentsPage::save(ArgsFile &args)
{
    const QString text = m_pane->editor()->toPlainText();

    if (text != m_loaded) {
        args = ArgsFile::parse(text);
        m_loaded = text;
    }
}

bool ArgumentsPage::isModified() const
{
    return m_pane->editor()->toPlainText() != m_loaded;
}
