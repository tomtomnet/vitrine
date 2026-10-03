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
#include "core/vmstore.h"
#include "ui/widgets.h"

using VmTemplate::Graphics;
using VmTemplate::Os;
using TemplateFirmware = VmTemplate::Firmware;

NewVmDialog::NewVmDialog(VmStore *store, QWidget *parent)
    : QDialog(parent), m_store(store), m_name(new QLineEdit), m_os(new QComboBox),
      m_memorySlider(new QSlider(Qt::Horizontal)), m_memory(new QSpinBox),
      m_cpuSlider(new QSlider(Qt::Horizontal)), m_cpus(new QSpinBox),
      m_newDisk(new QRadioButton(tr("Create a &new disk of"))), m_diskSize(new QSpinBox),
      m_existingDisk(new QRadioButton(tr("&Use an existing disk:"))),
      m_diskPath(new QLineEdit), m_noDisk(new QRadioButton(tr("N&o disk"))),
      m_iso(new QLineEdit), m_firmware(new QComboBox), m_graphics(new QComboBox),
      m_nativeContext(new QCheckBox(tr("DRM &native context: the guest uses the GPU through "
                                       "its own driver"))),
      m_note(Widgets::hint()), m_settings(new QCheckBox(tr("Open the &settings after creating it")))
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
    m_os->addItem(tr("Linux"), int(Os::Linux));
    m_os->addItem(tr("Windows 11"), int(Os::Windows11));
    m_os->addItem(tr("Windows 10 or older"), int(Os::Windows));
    m_os->addItem(tr("Other"), int(Os::Other));

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
    m_noDisk->setObjectName("noDisk");
    m_diskSize->setObjectName("diskSize");
    m_diskPath->setObjectName("diskPath");
    m_diskSize->setRange(1, 65536);
    m_diskSize->setSuffix(tr(" GiB"));
    diskGroup->addButton(m_newDisk);
    diskGroup->addButton(m_existingDisk);
    diskGroup->addButton(m_noDisk);
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
    disk->addWidget(m_noDisk);

    m_iso->setObjectName("iso");
    m_iso->setPlaceholderText(tr("Optional: a disc image to install from"));

    m_firmware->setObjectName("firmware");
    m_firmware->addItem(tr("UEFI"), int(TemplateFirmware::Uefi));
    m_firmware->addItem(tr("UEFI with Secure Boot"), int(TemplateFirmware::UefiSecureBoot));
    /* ARM's virt boots with UEFI only, and has no VGA */
    if (VmTemplate::hasBios()) {
        m_firmware->addItem(tr("BIOS, for old systems"), int(TemplateFirmware::Bios));
    }
    m_graphics->setObjectName("graphics");
    m_graphics->addItem(tr("3D accelerated (virtio-gpu with OpenGL)"),
                        int(Graphics::Accelerated));
    if (VmTemplate::hasVga()) {
        m_graphics->addItem(tr("Standard (virtio-vga)"), int(Graphics::Standard));
        m_graphics->addItem(tr("Compatible (VGA)"), int(Graphics::Compatible));
    } else {
        m_graphics->addItem(tr("2D (virtio-gpu)"), int(Graphics::Standard));
    }
    m_nativeContext->setObjectName("nativeContext");

    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("&System:"), m_os);
    form->addRow(QString(), m_note);
    form->addRow(Widgets::label(tr("&Memory:"), m_memory), memoryRow);
    form->addRow(Widgets::label(tr("&Processors:"), m_cpus), cpuRow);
    form->addRow(tr("Disk:"), disk);
    form->addRow(Widgets::label(tr("&Install from:"), m_iso),
                 Widgets::browseRow(m_iso, tr("Installation Disc Image"),
                                    tr("Disc images (*.iso);;All files (*)")));
    form->addRow(tr("&Firmware:"), m_firmware);
    form->addRow(tr("&Graphics:"), m_graphics);
    form->addRow(QString(), m_nativeContext);
    form->addRow(QString(), Widgets::hint(tr("Native context needs a virglrenderer built with it "
                                             "for this GPU (File > Build QEMU) and native "
                                             "context support in the guest's Mesa.")));
    layout->addLayout(form);
    layout->addWidget(m_settings);
    layout->addStretch();
    layout->addWidget(buttons);

    connect(m_os, &QComboBox::currentIndexChanged, this, &NewVmDialog::applyDefaults);
    connect(m_graphics, &QComboBox::currentIndexChanged, this, [this]() {
        m_nativeContext->setEnabled(m_graphics->currentData().toInt() ==
                                    int(Graphics::Accelerated));
    });
    connect(diskGroup, &QButtonGroup::buttonToggled, this, &NewVmDialog::updateDisk);
    connect(buttons, &QDialogButtonBox::accepted, this, &NewVmDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    applyDefaults();
    updateDisk();
    resize(620, sizeHint().height());
}

void NewVmDialog::applyDefaults()
{
    const Os os = Os(m_os->currentData().toInt());
    const VmTemplate::Defaults d = VmTemplate::defaults(os);

    /* at most half this computer */
    m_memory->setValue(int(qMin<qint64>(d.memoryMiB, qMax(m_memory->maximum() / 2, 1024))));
    m_cpus->setValue(qMin(d.cpus, qMax(m_cpus->maximum() / 2, 1)));
    m_diskSize->setValue(d.diskGiB);
    m_firmware->setCurrentIndex(qMax(0, m_firmware->findData(int(d.firmware))));
    /* no Compatible without VGA: 2D */
    m_graphics->setCurrentIndex(m_graphics->findData(int(d.graphics)) >= 0
                                    ? m_graphics->findData(int(d.graphics))
                                    : m_graphics->findData(int(Graphics::Standard)));
    m_nativeContext->setEnabled(m_graphics->currentData().toInt() == int(Graphics::Accelerated));

    switch (os) {
    case Os::Linux:
        m_note->setText(tr("Linux uses fast virtio devices and 3D graphics."));
        break;
    case Os::Windows11:
    case Os::Windows:
        if (!VmTemplate::hasVga()) {
            m_note->setText(tr("Windows on ARM uses virtio devices: keep the virtio-win "
                               "drivers at hand."));
        } else if (os == Os::Windows11) {
            m_note->setText(tr("Windows uses a SATA disk and an Intel network card, which need "
                               "no extra drivers. Windows 11 also checks for a TPM, which the "
                               "manager does not provide yet."));
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
    const TemplateFirmware choice = TemplateFirmware(m_firmware->currentData().toInt());
    std::optional<Firmware> firmware;
    VmTemplate::Options o;
    QString firmwareError;

    o.name = m_name->text().trimmed();
    o.os = Os(m_os->currentData().toInt());
    o.memoryMiB = m_memory->value();
    o.cpus = m_cpus->value();
    o.graphics = Graphics(m_graphics->currentData().toInt());
    o.nativeContext = o.graphics == Graphics::Accelerated && m_nativeContext->isChecked();
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
    } else if (m_existingDisk->isChecked()) {
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
