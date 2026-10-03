// SPDX-License-Identifier: GPL-2.0-or-later
#include "newvmdialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include "core/firmware.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmstore.h"
#include "ui/qemudocs.h"
#include "ui/widgets.h"

using VmTemplate::Os;
using TemplateFirmware = VmTemplate::Firmware;

/* The systems to choose from, with the desktop of a Linux guest */
static const struct {
    Os os;
    const char *desktop;
    const char *name;
} kSystems[] = {
    {Os::Linux, "kde", QT_TRANSLATE_NOOP("NewVmDialog", "Linux with KDE Plasma")},
    {Os::Linux, "gnome", QT_TRANSLATE_NOOP("NewVmDialog", "Linux with GNOME")},
    {Os::Linux, "other", QT_TRANSLATE_NOOP("NewVmDialog", "Linux, another desktop or none")},
    {Os::Windows11, "", QT_TRANSLATE_NOOP("NewVmDialog", "Windows 11")},
    {Os::Windows, "", QT_TRANSLATE_NOOP("NewVmDialog", "Windows 10 or older")},
    {Os::Other, "", QT_TRANSLATE_NOOP("NewVmDialog", "Another system")},
};

NewVmDialog::NewVmDialog(VmStore *store, QWidget *parent)
    : QDialog(parent), m_store(store), m_name(new QLineEdit), m_os(new QComboBox),
      m_memorySlider(new QSlider(Qt::Horizontal)), m_memory(new QSpinBox),
      m_cpuSlider(new QSlider(Qt::Horizontal)), m_cpus(new QSpinBox),
      m_newDisk(new QRadioButton(tr("Create a &new disk of"))), m_diskSize(new QSpinBox),
      m_existingDisk(new QRadioButton(tr("&Use an existing disk image:"))),
      m_diskPath(new QLineEdit), m_iso(new QLineEdit), m_note(Widgets::hint()),
      m_settings(new QCheckBox(tr("Open the &settings after creating it")))
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *memoryRow = new QHBoxLayout;
    auto *cpuRow = new QHBoxLayout;
    auto *disk = new QVBoxLayout;
    auto *newDiskRow = new QHBoxLayout;
    auto *existingRow = new QHBoxLayout;
    auto *diskGroup = new QButtonGroup(this);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    const qint64 hostMiB = Widgets::hostMemoryMiB();

    setWindowTitle(tr("New Virtual Machine"));
    buttons->button(QDialogButtonBox::Ok)->setText(tr("&Create"));

    m_name->setObjectName("name");
    m_name->setPlaceholderText(tr("For example Fedora"));
    m_os->setObjectName("os");
    for (int i = 0; i < int(std::size(kSystems)); i++) {
        m_os->addItem(tr(kSystems[i].name), i);
    }

    m_memory->setObjectName("memory");
    m_memory->setRange(128, int(qMax<qint64>(hostMiB, 1024)));
    m_memory->setSingleStep(256);
    m_memory->setSuffix(tr(" MiB"));
    m_memorySlider->setRange(0, m_memory->maximum() / 256);
    m_memorySlider->setPageStep(4);
    Widgets::link(m_memorySlider, m_memory, 256);
    memoryRow->addWidget(m_memorySlider, 1);
    memoryRow->addWidget(m_memory);

    m_cpus->setObjectName("cpus");
    m_cpus->setRange(1, qMax(QThread::idealThreadCount(), 1));
    m_cpuSlider->setRange(1, m_cpus->maximum());
    Widgets::link(m_cpuSlider, m_cpus, 1);
    cpuRow->addWidget(m_cpuSlider, 1);
    cpuRow->addWidget(m_cpus);

    m_newDisk->setObjectName("newDisk");
    m_existingDisk->setObjectName("existingDisk");
    m_diskSize->setObjectName("diskSize");
    m_diskPath->setObjectName("diskPath");
    m_diskPath->setPlaceholderText(tr("A disk with a system installed, e.g. a cloud image"));
    m_diskSize->setRange(1, 65536);
    m_diskSize->setSuffix(tr(" GiB"));
    diskGroup->addButton(m_newDisk);
    diskGroup->addButton(m_existingDisk);
    m_newDisk->setChecked(true);
    newDiskRow->addWidget(m_newDisk);
    newDiskRow->addWidget(m_diskSize);
    newDiskRow->addStretch();
    existingRow->addWidget(m_existingDisk);
    existingRow->addWidget(Widgets::browseRow(m_diskPath, tr("Disk Image"),
                                              tr("Disk images (*.qcow2 *.img *.raw *.vmdk "
                                                 "*.vdi *.vhdx *.vhd);;All files (*)")),
                           1);
    disk->addLayout(newDiskRow);
    disk->addLayout(existingRow);

    m_iso->setObjectName("iso");
    m_iso->setPlaceholderText(tr("Optional: a disc image to install from"));

    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("&System:"), m_os);
    form->addRow(QString(), m_note);
    form->addRow(Widgets::label(tr("&Memory:"), m_memory), memoryRow);
    form->addRow(Widgets::label(tr("&Processors:"), m_cpus), cpuRow);
    form->addRow(tr("Disk:"), disk);
    form->addRow(Widgets::label(tr("&Install from:"), m_iso),
                 Widgets::browseRow(m_iso, tr("Installation Disc Image"),
                                    tr("Disc images (*.iso);;All files (*)")));
    layout->addLayout(form);
    layout->addWidget(m_settings);
    layout->addStretch();
    layout->addWidget(buttons);

    connect(m_os, &QComboBox::currentIndexChanged, this, &NewVmDialog::applyDefaults);
    connect(diskGroup, &QButtonGroup::buttonToggled, this, &NewVmDialog::updateDisk);
    connect(buttons, &QDialogButtonBox::accepted, this, &NewVmDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    applyDefaults();
    updateDisk();
    resize(620, sizeHint().height());
}

void NewVmDialog::applyDefaults()
{
    const Os os = kSystems[m_os->currentData().toInt()].os;
    const VmTemplate::Defaults d = VmTemplate::defaults(os);

    /* at most half this computer */
    m_memory->setValue(int(qMin<qint64>(d.memoryMiB, qMax(m_memory->maximum() / 2, 1024))));
    m_cpus->setValue(qMin(d.cpus, qMax(m_cpus->maximum() / 2, 1)));
    m_diskSize->setValue(d.diskGiB);

    switch (os) {
    case Os::Linux:
        m_note->setText(VmTemplate::hasVga()
                            ? tr("Fast virtio devices, 3D graphics through this computer's "
                                 "GPU driver, and UEFI without Secure Boot.")
                            : tr("Fast virtio devices and 3D graphics, with UEFI."));
        break;
    case Os::Windows11:
    case Os::Windows:
        if (!VmTemplate::hasVga()) {
            m_note->setText(tr("Windows on ARM uses virtio devices: keep the virtio-win "
                               "drivers at hand."));
        } else if (os == Os::Windows11) {
            m_note->setText(tr("Windows uses a SATA disk and an Intel network card, which need "
                               "no extra drivers, and UEFI with Secure Boot. Windows 11 also "
                               "checks for a TPM, which Vitrine does not provide yet."));
        } else {
            m_note->setText(tr("Windows uses a SATA disk and an Intel network card, which need "
                               "no extra drivers."));
        }
        break;
    case Os::Other:
        m_note->setText(tr("Devices most systems have drivers for."));
        break;
    }
}

void NewVmDialog::updateDisk()
{
    m_diskSize->setEnabled(m_newDisk->isChecked());
    m_diskPath->parentWidget()->setEnabled(m_existingDisk->isChecked());
}

bool NewVmDialog::openSettings() const
{
    return m_settings->isChecked();
}

void NewVmDialog::accept()
{
    const QString name = m_name->text().trimmed();
    const QString diskPath = m_diskPath->text().trimmed();
    const QString iso = m_iso->text().trimmed();
    QString error;

    if (name.isEmpty()) {
        Widgets::inform(this, windowTitle(), tr("Give the VM a name."));
        m_name->setFocus();
        return;
    }
    if (m_existingDisk->isChecked() && !QFileInfo(diskPath).isFile()) {
        Widgets::inform(this, windowTitle(), tr("Choose the disk image to use."));
        m_diskPath->setFocus();
        return;
    }
    if (!iso.isEmpty() && !QFileInfo(iso).isFile()) {
        Widgets::inform(this, windowTitle(), tr("The disc image does not exist."));
        m_iso->setFocus();
        return;
    }

    Vm *vm = m_store->create(name, &error);
    if (!vm) {
        Widgets::warn(this, tr("Cannot Create the VM"), error);
        return;
    }
    m_warnings.clear();
    if (!create(vm, &error)) {
        /* nothing worth keeping yet */
        QDir(vm->dir()).removeRecursively();
        m_store->reload();
        Widgets::warn(this, tr("Cannot Create the VM"), error);
        return;
    }
    m_vm = vm;
    QDialog::accept();
}

bool NewVmDialog::create(Vm *vm, QString *error)
{
    const int system = m_os->currentData().toInt();
    const VmTemplate::Defaults defaults = VmTemplate::defaults(kSystems[system].os);
    const TemplateFirmware choice = defaults.firmware;
    /* the QEMU it runs with, which may lack some of what vitrine's has */
    const QemuInfo *info = QemuDocs::preferred()->info();
    std::optional<Firmware> firmware;
    VmTemplate::Options o;
    QString firmwareError;

    o.name = m_name->text().trimmed();
    o.os = kSystems[system].os;
    o.desktop = kSystems[system].desktop;
    o.memoryMiB = m_memory->value();
    o.cpus = m_cpus->value();
    o.graphics = defaults.graphics;
    o.passt = VmTemplate::hasPasst(info);
    o.gpuProperties = VmTemplate::gpuProperties(info);
    if (o.os == Os::Linux) {
        /* the guest's SSH, on a port no other VM forwards */
        QList<int> taken;
        for (const Vm *other : m_store->vms()) {
            if (other != vm && VmConfig::network(other->args()).sshPort > 0) {
                taken << VmConfig::network(other->args()).sshPort;
            }
        }
        o.sshPort = VmConfig::freePort(taken);
    }
    o.iso = QDir::cleanPath(m_iso->text().trimmed());
    if (m_iso->text().trimmed().isEmpty()) {
        o.iso.clear();
    }
    if (m_newDisk->isChecked()) {
        if (!createDiskImage(QDir(vm->dir()).filePath("disk.qcow2"),
                             qint64(m_diskSize->value()) << 30, error)) {
            return false;
        }
        o.disk = "disk.qcow2";
    } else {
        o.disk = QFileInfo(m_diskPath->text().trimmed()).absoluteFilePath();
    }

    if (choice != TemplateFirmware::Bios) {
        firmware = FirmwareDb::find(choice == TemplateFirmware::UefiSecureBoot);
        if (!firmware && choice == TemplateFirmware::UefiSecureBoot) {
            firmware = FirmwareDb::find(false);
            if (firmware) {
                m_warnings << tr("No UEFI firmware with Secure Boot was found, so the VM "
                                 "uses UEFI without it.");
            }
        }
        if (!firmware) {
            m_warnings << tr("No UEFI firmware was found, so the VM uses BIOS. Install "
                             "edk2-ovmf (sudo dnf install edk2-ovmf) for UEFI.");
        }
    }

    const ArgsFile args = VmTemplate::build(o, [&](ArgsFile &a) {
        if (firmware && !FirmwareDb::apply(a, *firmware, vm->dir(), &firmwareError)) {
            m_warnings << tr("The UEFI firmware could not be set up (%1), so the VM uses "
                             "BIOS.")
                              .arg(firmwareError);
        }
    });
    return vm->save(args, error);
}
