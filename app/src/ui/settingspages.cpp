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
#include "core/vmtemplate.h"
#include "ui/argseditor.h"
#include "ui/banner.h"
#include "ui/firmwarerepair.h"
#include "ui/icons.h"
#include "ui/qemudocs.h"
#include "ui/referencepanel.h"
#include "ui/uiconfig.h"
#include "ui/widgets.h"

/* General */

/* The systems and desktops of #guest, by the names the page gives them */
static const char *const kSystems[][2] = {
    {"linux", QT_TRANSLATE_NOOP("GeneralPage", "Linux")},
    {"windows", QT_TRANSLATE_NOOP("GeneralPage", "Windows")},
    {"other", QT_TRANSLATE_NOOP("GeneralPage", "Another system")},
};
static const char *const kDesktops[][2] = {
    {"kde", QT_TRANSLATE_NOOP("GeneralPage", "KDE Plasma")},
    {"gnome", QT_TRANSLATE_NOOP("GeneralPage", "GNOME")},
    {"other", QT_TRANSLATE_NOOP("GeneralPage", "Another desktop, or none")},
};

/* @combo with @choices and "Not known", and @value as written if it is none of them */
template<size_t N>
static void fillChoices(QComboBox *combo, const char *const (&choices)[N][2], const QString &value)
{
    const QSignalBlocker block(combo);

    combo->clear();
    for (const auto &c : choices) {
        combo->addItem(QCoreApplication::translate("GeneralPage", c[1]), QString(c[0]));
    }
    combo->addItem(QCoreApplication::translate("GeneralPage", "Not known"), QString());
    if (combo->findData(value) < 0) {
        combo->addItem(value, value);
    }
    combo->setCurrentIndex(combo->findData(value));
}

GeneralPage::GeneralPage(Vm *vm, QWidget *parent)
    : SettingsPage(parent), m_name(new QLineEdit), m_os(new QComboBox), m_desktop(new QComboBox)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *folder = Widgets::note(QString("<a href=\"%1\">%2</a>")
                            .arg(QUrl::fromLocalFile(vm->dir()).toString(),
                                 QDir::toNativeSeparators(vm->dir()).toHtmlEscaped()));

    m_name->setObjectName("name");
    m_name->setMaximumWidth(Widgets::em(this) * 20);
    m_os->setObjectName("os");
    m_desktop->setObjectName("desktop");
    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("&System:"), m_os);
    form->addRow(tr("&Desktop:"), m_desktop);
    form->addRow(QString(), Widgets::hint(tr("The timing of the VM's frames depends on its "
                                             "desktop.")));
    form->addRow(tr("Folder:"), folder);
    form->addRow(QString(), Widgets::hint(tr("The folder holds the arguments (vm.args), the disks the "
                                    "VM creates and its log.")));
    layout->addLayout(form);
    layout->addStretch();
    connect(m_os, &QComboBox::currentIndexChanged, this, &GeneralPage::updateDesktop);
}

QIcon GeneralPage::icon() const
{
    return Icons::themed({"preferences-system", "configure"}, QStyle::SP_ComputerIcon);
}

void GeneralPage::updateDesktop()
{
    const QString os = m_os->currentData().toString();
    m_desktop->setEnabled(os == "linux" || os.isEmpty());
}

VmConfig::Guest GeneralPage::shown() const
{
    const QString os = m_os->currentData().toString();
    /* a desktop for Linux, or a system not known */
    return {os, m_desktop->isEnabled() ? m_desktop->currentData().toString() : QString()};
}

void GeneralPage::load(const ArgsFile &args)
{
    const VmConfig::Guest guest = VmConfig::guest(args);

    m_loaded = VmConfig::name(args);
    m_name->setText(m_loaded);
    fillChoices(m_os, kSystems, guest.os);
    fillChoices(m_desktop, kDesktops, guest.desktop);
    updateDesktop();
    /* as the page shows it, so that an untouched page writes nothing */
    m_loadedGuest = shown();
}

void GeneralPage::save(ArgsFile &args)
{
    const QString name = m_name->text().trimmed();
    const VmConfig::Guest guest = shown();

    if (!name.isEmpty() && name != m_loaded) {
        VmConfig::setName(args, name);
        m_loaded = name;
    }
    if (guest.os != m_loadedGuest.os || guest.desktop != m_loadedGuest.desktop) {
        VmConfig::setGuest(args, guest);
        m_loadedGuest = guest;
    }
}

bool GeneralPage::isModified() const
{
    const QString name = m_name->text().trimmed();
    const VmConfig::Guest guest = shown();

    return (!name.isEmpty() && name != m_loaded) || guest.os != m_loadedGuest.os ||
           guest.desktop != m_loadedGuest.desktop;
}

/* Hardware */

HardwarePage::HardwarePage(QWidget *parent)
    : SettingsPage(parent), m_memorySlider(new QSlider(Qt::Horizontal)), m_memory(new QSpinBox),
      m_cpuSlider(new QSlider(Qt::Horizontal)), m_cpus(new QSpinBox), m_topology(Widgets::hint())
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

    form->addRow(Widgets::label(tr("M&emory:"), m_memory), memoryRow);
    form->addRow(QString(), Widgets::hint(tr("This computer has %1 GiB.")
                                     .arg(QString::number(hostMiB / 1024.0, 'f', 1))));
    form->addRow(Widgets::label(tr("&Processors:"), m_cpus), cpuRow);
    form->addRow(QString(), Widgets::hint(tr("This computer has %n.", nullptr, hostCpus)));
    form->addRow(QString(), m_topology);
    layout->addLayout(form);
    layout->addStretch();
}

QIcon HardwarePage::icon() const
{
    return Icons::themed({"cpu", "computer"}, QStyle::SP_ComputerIcon);
}

void HardwarePage::load(const ArgsFile &args)
{
    const qint64 memory = VmConfig::memoryMiB(args);
    const VmConfig::Cpus cpus = VmConfig::cpus(args);

    m_memory->setMaximum(int(qMax<qint64>(m_memory->maximum(), memory)));
    m_memorySlider->setMaximum(m_memory->maximum() / 256);
    m_memory->setValue(int(memory > 0 ? memory : 128));
    m_cpus->setMaximum(qMax(m_cpus->maximum(), cpus.count));
    m_cpuSlider->setMaximum(m_cpus->maximum());
    m_cpus->setValue(cpus.count);
    /* compare with what the page shows, e.g. 128 MiB for no -m, so that
       an untouched page writes nothing */
    m_loadedMemory = m_memory->value();
    m_loadedCpus = m_cpus->value();
    /* maxcpus=, dies=...: a count written without them would not start */
    m_cpus->setEnabled(!cpus.custom);
    m_cpuSlider->setEnabled(!cpus.custom);
    if (cpus.custom) {
        m_topology->setText(tr("The processors of this VM are set up by hand (-smp %1): "
                               "change them on the Arguments page.")
                                .arg(args.lines[args.indexOf("smp")].value.trimmed()
                                         .toHtmlEscaped()));
    } else {
        m_topology->setText(cpus.threads > 1
                                ? tr("%n threads per core, as far as the number allows: the "
                                     "Machine page sets the topology.",
                                     nullptr, cpus.threads)
                                : QString());
    }
    m_topology->setVisible(!m_topology->text().isEmpty());
}

void HardwarePage::save(ArgsFile &args)
{
    if (m_memory->value() != m_loadedMemory) {
        VmConfig::setMemoryMiB(args, m_memory->value());
        m_loadedMemory = m_memory->value();
    }
    if (m_cpus->value() != m_loadedCpus) {
        VmConfig::setCpuCount(args, m_cpus->value());
        m_loadedCpus = m_cpus->value();
    }
}

bool HardwarePage::isModified() const
{
    return m_memory->value() != m_loadedMemory || m_cpus->value() != m_loadedCpus;
}

/* Display */

DisplayPage::DisplayPage(QWidget *parent)
    : SettingsPage(parent), m_custom(new Banner(Banner::Information)),
      m_embedded(new QRadioButton(tr("In &Vitrine's window"))),
      m_ownWindow(new QRadioButton(tr("In a &window of its own (SDL)")))
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *choices = new QVBoxLayout;
    auto *group = new QButtonGroup(this);

    m_custom->setObjectName("customDisplay");
    m_embedded->setObjectName("embedded");
    m_ownWindow->setObjectName("ownWindow");
    group->addButton(m_embedded);
    group->addButton(m_ownWindow);
    choices->addWidget(m_embedded);
    choices->addWidget(Widgets::hint(tr("The screen is part of Vitrine's window, and can go "
                                        "full screen.")));
    choices->addWidget(m_ownWindow);
    choices->addWidget(Widgets::hint(tr("QEMU shows the screen in a window of its own, which "
                                        "stays open when Vitrine closes.")));
    form->addRow(Widgets::label(tr("Show the VM:"), m_embedded), choices);
    form->addRow(QString(), Widgets::hint(tr("Vitrine chooses the graphics card and its 3D "
                                             "acceleration; the Arguments page can change "
                                             "them.")));
    layout->addWidget(m_custom);
    layout->addLayout(form);
    layout->addStretch();
}

QIcon DisplayPage::icon() const
{
    return Icons::themed({"video-display", "preferences-desktop-display"},
                         QStyle::SP_DesktopIcon);
}

VmConfig::Screen DisplayPage::chosen() const
{
    return m_embedded->isChecked()    ? VmConfig::Screen::Embedded
           : m_ownWindow->isChecked() ? VmConfig::Screen::OwnWindow
                                      : VmConfig::Screen::None;
}

void DisplayPage::load(const ArgsFile &args)
{
    const VmConfig::Screen screen = VmConfig::screen(args);
    const QString display = VmConfig::graphics(args).display;
    QString custom;

    m_loaded = screen;
    /*
     * None checked for a VM that shows nowhere.  The buttons' group keeps
     * one checked while it is exclusive, whatever their autoExclusive says,
     * and a button left checked would make an untouched page modified
     */
    QButtonGroup *group = m_embedded->group();
    group->setExclusive(false);
    m_embedded->setChecked(screen == VmConfig::Screen::Embedded);
    m_ownWindow->setChecked(screen == VmConfig::Screen::OwnWindow);
    group->setExclusive(true);

    if (args.indexOf("nographic") >= 0) {
        custom = tr("This VM has no screen (-nographic): change it on the Arguments page.");
    } else if (screen == VmConfig::Screen::None &&
               (display.isEmpty() || display == "default")) {
        custom = tr("This VM shows its screen over VNC or SPICE only. Choosing below adds a "
                    "window.");
    } else if (screen == VmConfig::Screen::None) {
        custom = tr("This VM shows its screen nowhere Vitrine can (-display %1). Choosing "
                    "below replaces it.")
                     .arg(display.toHtmlEscaped());
    } else if (screen == VmConfig::Screen::OwnWindow && display != "sdl") {
        custom = display.isEmpty() || display == "default"
                     ? tr("The screen shows in QEMU's default window.")
                     : tr("The screen shows in QEMU's %1 window.")
                           .arg(display.toUpper().toHtmlEscaped());
    }
    m_custom->setText(custom);
    m_custom->setVisible(!custom.isEmpty());
    m_embedded->setEnabled(args.indexOf("nographic") < 0);
    m_ownWindow->setEnabled(args.indexOf("nographic") < 0);
}

void DisplayPage::save(ArgsFile &args)
{
    if (chosen() != m_loaded && chosen() != VmConfig::Screen::None) {
        VmConfig::setScreen(args, chosen());
        load(args);
    }
}

bool DisplayPage::isModified() const
{
    return chosen() != m_loaded;
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

/* Network */

NetworkPage::NetworkPage(Vm *vm, QWidget *parent)
    : SettingsPage(parent), m_vm(vm), m_custom(new Banner(Banner::Information)),
      m_nat(new QCheckBox(tr("&Connect the VM to the network"))),
      m_ssh(new QCheckBox(tr("&Forward a port of this computer to the guest's SSH server:"))),
      m_port(new QSpinBox), m_sshInfo(Widgets::hint())
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *sshRow = new QHBoxLayout;

    m_custom->setObjectName("customNetwork");
    m_nat->setObjectName("nat");
    m_ssh->setObjectName("ssh");
    m_port->setObjectName("sshPort");
    m_port->setRange(1024, 65535);
    m_sshInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sshRow->addWidget(m_ssh);
    sshRow->addWidget(m_port);
    sshRow->addStretch();

    form->addRow(QString(), m_nat);
    form->addRow(QString(), Widgets::hint(tr("Through this computer's connection (NAT): the VM "
                                             "reaches the network and the internet, other "
                                             "computers do not reach the VM.")));
    form->addRow(QString(), sshRow);
    form->addRow(QString(), m_sshInfo);
    layout->addWidget(m_custom);
    layout->addLayout(form);
    layout->addStretch();

    connect(m_nat, &QCheckBox::toggled, this, &NetworkPage::update);
    connect(m_ssh, &QCheckBox::toggled, this, [this](bool on) {
        if (on && m_loaded.sshPort == 0 && m_port->value() == m_port->minimum()) {
            /* a port no other VM uses, free now */
            QList<int> taken;
            if (auto *store = m_vm ? qobject_cast<VmStore *>(m_vm->parent()) : nullptr) {
                for (const Vm *other : store->vms()) {
                    if (other != m_vm && VmConfig::network(other->args()).sshPort > 0) {
                        taken << VmConfig::network(other->args()).sshPort;
                    }
                }
            }
            m_port->setValue(qMax(VmConfig::freePort(taken), m_port->minimum()));
        }
        update();
    });
    connect(m_port, &QSpinBox::valueChanged, this, &NetworkPage::update);
}

QIcon NetworkPage::icon() const
{
    return Icons::themed({"network-wired", "preferences-system-network"},
                         QStyle::SP_DriveNetIcon);
}

void NetworkPage::update()
{
    const bool custom = m_loaded.kind == VmConfig::Network::Custom;

    m_nat->setEnabled(!custom);
    m_ssh->setEnabled(!custom && m_nat->isChecked());
    m_port->setEnabled(!custom && m_nat->isChecked() && m_ssh->isChecked());
    m_sshInfo->setVisible(!custom && m_nat->isChecked() && m_ssh->isChecked());
    m_sshInfo->setText(tr("From this computer only: ssh -p %1 USER@127.0.0.1")
                           .arg(m_port->value()));
}

void NetworkPage::load(const ArgsFile &args)
{
    const QSignalBlocker a(m_nat), b(m_ssh), c(m_port);

    m_loaded = VmConfig::network(args);
    /* where the VM had none: passt, if at hand */
    m_backend = VmTemplate::hasPasst(QemuDocs::forArgs(args)->info()) ? "passt" : "user";
    m_nat->setChecked(m_loaded.kind == VmConfig::Network::Nat);
    m_ssh->setChecked(m_loaded.sshPort > 0);
    m_port->setMinimum(qMin(1024, m_loaded.sshPort > 0 ? m_loaded.sshPort : 1024));
    m_port->setValue(m_loaded.sshPort > 0 ? m_loaded.sshPort : m_port->minimum());
    m_custom->setText(tr("The network of this VM is set up by hand (%1): change it on the "
                         "Arguments page.")
                          .arg(m_loaded.custom.toHtmlEscaped()));
    m_custom->setVisible(m_loaded.kind == VmConfig::Network::Custom);
    update();
}

VmConfig::Network NetworkPage::shown() const
{
    VmConfig::Network n = m_loaded;

    n.kind = m_nat->isChecked() ? VmConfig::Network::Nat : VmConfig::Network::Off;
    n.sshPort = m_nat->isChecked() && m_ssh->isChecked() ? m_port->value() : 0;
    if (n.backend.isEmpty()) {
        n.backend = m_backend;
    }
    return n;
}

void NetworkPage::save(ArgsFile &args)
{
    if (isModified()) {
        VmConfig::setNetwork(args, shown());
        load(args);
    }
}

bool NetworkPage::isModified() const
{
    const VmConfig::Network n = shown();

    if (m_loaded.kind == VmConfig::Network::Custom) {
        return false;
    }
    return n.kind != m_loaded.kind || (n.kind == VmConfig::Network::Nat &&
                                       n.sshPort != m_loaded.sshPort);
}

/* Machine */

MachinePage::MachinePage(QWidget *parent)
    : SettingsPage(parent), m_topology(new QCheckBox(tr("Set the &topology"))),
      m_sockets(new QSpinBox), m_cores(new QSpinBox), m_threads(new QSpinBox),
      m_count(Widgets::hint()), m_model(new QComboBox), m_modelInfo(Widgets::hint()),
      m_machine(new QComboBox), m_machineInfo(Widgets::hint()),
      m_defaultQemu(new QRadioButton), m_ownQemu(new QRadioButton(tr("This &build:"))),
      m_qemuPath(new QLineEdit), m_qemuInfo(Widgets::hint())
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();

    m_topology->setObjectName("topology");
    m_sockets->setObjectName("sockets");
    m_cores->setObjectName("cores");
    m_threads->setObjectName("threads");
    for (QSpinBox *spin : {m_sockets, m_cores, m_threads}) {
        spin->setRange(1, 1024);
        spin->setEnabled(false);
        connect(spin, &QSpinBox::valueChanged, this, &MachinePage::updateTopology);
    }
    connect(m_topology, &QCheckBox::toggled, this, [this](bool on) {
        for (QSpinBox *spin : {m_sockets, m_cores, m_threads}) {
            spin->setEnabled(on);
        }
        if (on && m_sockets->value() * m_cores->value() * m_threads->value() !=
                      m_loadedCpus.count) {
            /* one socket of cores */
            const QSignalBlocker a(m_sockets), b(m_cores), c(m_threads);
            m_sockets->setValue(1);
            m_cores->setValue(m_loadedCpus.count);
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
    connect(m_model, &QComboBox::currentTextChanged, this, &MachinePage::describe);
    connect(m_machine, &QComboBox::currentTextChanged, this, &MachinePage::describe);

    form->addRow(tr("Processor mode&l:"), m_model);
    form->addRow(QString(), m_modelInfo);
    form->addRow(QString(), m_topology);
    form->addRow(tr("Sockets:"), m_sockets);
    form->addRow(tr("Cores:"), m_cores);
    form->addRow(tr("Threads:"), m_threads);
    form->addRow(QString(), m_count);
    form->addRow(tr("Ma&chine:"), m_machine);
    form->addRow(QString(), m_machineInfo);

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
    layout->addLayout(form);
    layout->addStretch();

    connect(qemuGroup, &QButtonGroup::buttonToggled, this, &MachinePage::updateQemu);
    connect(m_qemuPath, &QLineEdit::textChanged, this, &MachinePage::updateQemu);
    updateQemu();
}

QIcon MachinePage::icon() const
{
    return Icons::themed({"computer", "cpu"}, QStyle::SP_ComputerIcon);
}

QString MachinePage::chosenQemu() const
{
    return m_ownQemu->isChecked() ? m_qemuPath->text().trimmed() : QString();
}

void MachinePage::updateQemu()
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
        connect(docs, &QemuDocs::changed, this, &MachinePage::fillLists);
    }
    fillLists();
}

void MachinePage::fillLists()
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

void MachinePage::describe()
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

void MachinePage::updateTopology()
{
    const int count = m_topology->isChecked() && !m_loadedCpus.custom
                          ? m_sockets->value() * m_cores->value() * m_threads->value()
                          : m_loadedCpus.count;
    m_count->setText(m_loadedCpus.custom
                         ? tr("%n processors in all. The -smp line has keys this page does "
                              "not follow, such as maxcpus or dies: change it on the "
                              "Arguments page.",
                              nullptr, count)
                         : tr("%n processors in all.", nullptr, count));
}

void MachinePage::load(const ArgsFile &args)
{
    m_loadedCpus = VmConfig::cpus(args);
    m_loadedMachine = VmConfig::machineType(args);
    {
        const QSignalBlocker a(m_sockets), b(m_cores), c(m_threads), d(m_topology);
        /* what -smp leaves out, as QEMU works it out, e.g. 2 sockets for "8,cores=4" */
        const VmConfig::Cpus shown = VmConfig::derivedTopology(m_loadedCpus);

        m_loadedTopology = m_loadedCpus.sockets > 0 || m_loadedCpus.cores > 0 ||
                           m_loadedCpus.threads > 0;
        m_sockets->setValue(shown.sockets);
        m_cores->setValue(shown.cores);
        m_threads->setValue(shown.threads);
        m_topology->setChecked(m_loadedTopology);
        /* maxcpus=, dies=...: numbers written without them would not start */
        m_topology->setEnabled(!m_loadedCpus.custom);
        for (QSpinBox *spin : {m_sockets, m_cores, m_threads}) {
            spin->setEnabled(m_loadedTopology && !m_loadedCpus.custom);
        }
        /* compare with what the page shows */
        m_shownCpus.sockets = m_sockets->value();
        m_shownCpus.cores = m_cores->value();
        m_shownCpus.threads = m_threads->value();
    }
    updateTopology();
    m_model->setCurrentText(m_loadedCpus.model);
    m_machine->setCurrentText(m_loadedMachine);

    m_loadedQemu = VmConfig::qemuBinary(args);
    {
        const QSignalBlocker a(m_defaultQemu), b(m_ownQemu), c(m_qemuPath);
        m_qemuPath->setText(m_loadedQemu);
        (m_loadedQemu.isEmpty() ? m_defaultQemu : m_ownQemu)->setChecked(true);
    }
    updateQemu();
    describe();
}

bool MachinePage::topologyChanged() const
{
    if (m_loadedCpus.custom) {
        return false;
    }
    if (m_topology->isChecked() != m_loadedTopology) {
        return true;
    }
    return m_topology->isChecked() && (m_sockets->value() != m_shownCpus.sockets ||
                                       m_cores->value() != m_shownCpus.cores ||
                                       m_threads->value() != m_shownCpus.threads);
}

void MachinePage::save(ArgsFile &args)
{
    const QString machine = m_machine->currentText().trimmed();
    /* "host,topoext=on": the model, then flags for the -cpu line */
    const QString model = m_model->currentText().trimmed();
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
        if (m_topology->isChecked()) {
            cpus.sockets = m_sockets->value();
            cpus.cores = m_cores->value();
            cpus.threads = m_threads->value();
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
    m_loadedTopology = m_topology->isChecked();
    m_shownCpus.sockets = m_sockets->value();
    m_shownCpus.cores = m_cores->value();
    m_shownCpus.threads = m_threads->value();
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
        m_model->setCurrentText(modelName);
    }
    if (!machine.isEmpty() && machine != m_loadedMachine) {
        VmConfig::setMachineType(args, machine);
        m_loadedMachine = machine;
    }
    if (chosenQemu() != m_loadedQemu) {
        VmConfig::setQemuBinary(args, chosenQemu());
        m_loadedQemu = chosenQemu();
    }
}

bool MachinePage::isModified() const
{
    const QString machine = m_machine->currentText().trimmed();

    return m_model->currentText().trimmed() != m_loadedCpus.model ||
           (!machine.isEmpty() && machine != m_loadedMachine) || chosenQemu() != m_loadedQemu ||
           topologyChanged();
}

/* Boot */

BootPage::BootPage(Vm *vm, QWidget *parent)
    : SettingsPage(parent), m_vm(vm), m_firmware(new QComboBox),
      m_firmwareInfo(Widgets::hint()), m_resetVars(new QPushButton(tr("&Reset UEFI Variables…"))),
      m_bootMenu(new QCheckBox(tr("Show the boot men&u when the VM starts"))),
      m_bootDevice(new QComboBox)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *resetRow = new QHBoxLayout;

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
    m_resetVars->setObjectName("resetVars");
    resetRow->addWidget(m_resetVars);
    resetRow->addStretch();
    connect(m_resetVars, &QPushButton::clicked, this, &BootPage::resetVars);
    if (m_vm) {
        connect(m_vm->runner(), &VmRunner::stateChanged, this, &BootPage::updateResetVars);
    }

    form->addRow(tr("F&irmware:"), m_firmware);
    form->addRow(QString(), m_firmwareInfo);
    form->addRow(QString(), resetRow);
    form->addRow(tr("&Start from:"), m_bootDevice);
    form->addRow(QString(), m_bootMenu);
    layout->addLayout(form);
    layout->addStretch();
    connect(m_firmware, &QComboBox::currentIndexChanged, this, &BootPage::describeFirmware);
}

QIcon BootPage::icon() const
{
    return Icons::themed({"system-reboot", "media-playback-start"}, QStyle::SP_MediaPlay);
}

void BootPage::load(const ArgsFile &args)
{
    using VmConfig::FirmwareKind;
    const QList<VmConfig::Disk> disks = VmConfig::disks(args);
    bool disk = false, cdrom = false, nic = false;

    m_machine = VmConfig::machineType(args);
    const bool virt = m_machine.startsWith("virt");
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
        if (!virt || FirmwareDb::find(true, m_machine)) {
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

void BootPage::updateResetVars()
{
    const bool running = m_vm && m_vm->runner()->isActive();

    m_resetVars->setVisible(!resettableVars(m_vm).path.isEmpty());
    m_resetVars->setEnabled(!running);
    m_resetVars->setToolTip(running ? tr("Shut the VM down first.")
                                    : tr("For a system that no longer starts: the boot "
                                         "entries and keys go back to those of a new VM."));
}

void BootPage::resetVars()
{
    const FirmwareFiles::File vars = resettableVars(m_vm);

    if (!vars.path.isEmpty() && FirmwareRepair::reset(this, m_vm, {vars}, tr("&Reset"))) {
        m_firmwareInfo->setText(tr("The UEFI variables are those of a new VM again. The old "
                                   "ones are kept in the VM folder, in a .bak file."));
    }
}

void BootPage::save(ArgsFile &args)
{
    using VmConfig::FirmwareKind;
    const auto kind = FirmwareKind(m_firmware->currentData().toInt());
    const auto device = VmConfig::BootDevice(m_bootDevice->currentData().toInt());

    if (kind != m_loadedFirmware && kind != FirmwareKind::Custom) {
        if (kind == FirmwareKind::Bios) {
            VmConfig::useBios(args);
        } else if (const std::optional<Firmware> fw = FirmwareDb::find(
                       kind == FirmwareKind::UefiSecureBoot, m_machine)) {
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

bool BootPage::isModified() const
{
    return m_firmware->currentData().toInt() != int(m_loadedFirmware) ||
           m_bootMenu->isChecked() != m_loadedBootMenu ||
           m_bootDevice->currentData().toInt() != int(m_loadedBootDevice);
}

void BootPage::describeFirmware()
{
    using VmConfig::FirmwareKind;
    const auto kind = FirmwareKind(m_firmware->currentData().toInt());

    if (kind == FirmwareKind::Custom) {
        m_firmwareInfo->setText(tr("The firmware is set by hand: change it on the Arguments "
                                   "page."));
    } else if ((kind == FirmwareKind::Uefi || kind == FirmwareKind::UefiSecureBoot) &&
               !FirmwareDb::find(kind == FirmwareKind::UefiSecureBoot, m_machine)) {
        m_firmwareInfo->setText(tr("No such UEFI firmware was found: install edk2-ovmf "
                                   "(edk2-aarch64 on ARM)."));
    } else if (kind != m_loadedFirmware) {
        m_firmwareInfo->setText(tr("A system installed with UEFI needs UEFI, one installed "
                                   "with BIOS needs BIOS: changing it may keep the installed "
                                   "system from starting."));
    } else if (kind == FirmwareKind::UefiSecureBoot) {
        m_firmwareInfo->setText(tr("Secure Boot starts only signed kernels and modules: the "
                                   "guest tools' graphics driver is not signed."));
    } else if (kind == FirmwareKind::Bios) {
        m_firmwareInfo->setText(QString());
    } else {
        m_firmwareInfo->setText(tr("The firmware and its variables are kept in the VM "
                                   "folder."));
    }
}

bool BootPage::commit(const ArgsFile &args, const QString &vmDir, QString *error)
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
