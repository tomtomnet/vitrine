// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmdetails.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "core/diskinfo.h"
#include "core/gpucontexts.h"
#include "core/hostdevices.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/icons.h"
#include "ui/qemudocs.h"
#include "ui/uiconfig.h"

/* How to build a guest Mesa with native context, in the repository */
static const char kGuestMesaGuide[] =
    "https://github.com/tomtomnet/qemu-gui-manager/blob/main/docs/guest-mesa.md";

/* The guest shut down, but -no-shutdown keeps QEMU open until Force Off */
static bool keptOpen(const Vm *vm)
{
    return vm->runner()->state() == VmRunner::State::Stopping &&
           vm->args().indexOf("no-shutdown") >= 0;
}

QString stateText(const Vm *vm)
{
    if (keptOpen(vm)) {
        return QCoreApplication::translate("VmDetails", "Shut down, QEMU still open");
    }
    switch (vm->runner()->state()) {
    case VmRunner::State::Stopped:
        break;
    case VmRunner::State::Starting:
        return QCoreApplication::translate("VmDetails", "Starting…");
    case VmRunner::State::Running:
        return QCoreApplication::translate("VmDetails", "Running");
    case VmRunner::State::Paused:
        return QCoreApplication::translate("VmDetails", "Paused");
    case VmRunner::State::Stopping:
        return QCoreApplication::translate("VmDetails", "Shutting down…");
    }
    return QCoreApplication::translate("VmDetails", "Powered off");
}

static QString sizeText(qint64 mib)
{
    return mib % 1024 == 0 ? QCoreApplication::translate("VmDetails", "%1 GiB").arg(mib / 1024)
                           : QCoreApplication::translate("VmDetails", "%1 MiB").arg(mib);
}

static QString link(const QString &path)
{
    return QString("<a href=\"%1\">%2</a>")
        .arg(QUrl::fromLocalFile(path).toString().toHtmlEscaped(),
             QDir::toNativeSeparators(path).toHtmlEscaped());
}

VmDetails::VmDetails(QWidget *parent)
    : QWidget(parent), m_icon(new QLabel), m_name(new QLabel), m_state(new QLabel),
      m_note(new Banner(Banner::Information)), m_error(new Banner(Banner::Warning)),
      m_text(new QTextBrowser), m_growing(new QTimer(this)), m_contexts(new GpuContexts(this)),
      m_contextsNote(new Banner(Banner::Warning))
{
    auto *layout = new QVBoxLayout(this);
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    QFont font = m_name->font();

    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.4);
    m_name->setFont(font);
    m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_icon->setPixmap(Icons::themed({"computer"}, QStyle::SP_ComputerIcon).pixmap(48, 48));
    /* the name and the state together, beside the middle of the icon */
    titles->setSpacing(0);
    titles->addStretch();
    titles->addWidget(m_name);
    titles->addWidget(m_state);
    titles->addStretch();
    header->addWidget(m_icon);
    header->addLayout(titles, 1);

    m_note->setText(tr("The guest has shut down. QEMU stays open because of the "
                       "-no-shutdown option: Force Off closes it."));
    m_note->hide();
    m_error->button()->setText(tr("Show &Log"));
    m_error->button()->show();
    m_error->hide();
    m_text->setObjectName("details");
    /* QTextBrowser would open file: links itself, and show nothing */
    m_text->setOpenLinks(false);
    connect(m_text, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        if (!url.scheme().isEmpty()) {
            QDesktopServices::openUrl(url);
        }
    });
    /* on the page, not in a box of its own, and in line with the icon */
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->viewport()->setAutoFillBackground(false);
    m_text->document()->setDocumentMargin(0);

    layout->addLayout(header);
    layout->addWidget(m_note);
    layout->addWidget(m_error);
    layout->addWidget(m_contextsNote);
    layout->addWidget(m_text, 1);

    connect(m_error->button(), &QPushButton::clicked, this, &VmDetails::showLog);

    /* the disks fill up as the VM runs, and its guest starts drawing */
    m_contextsNote->hide();
    connect(m_contexts, &GpuContexts::changed, this, &VmDetails::refresh);
    m_growing->setInterval(5000);
    connect(m_growing, &QTimer::timeout, this, [this]() {
        if (m_vm && m_vm->runner()->isActive() && isVisible()) {
            const VmConfig::Graphics g = VmConfig::graphics(m_vm->args());
            if (g.nativeContext || g.venus) {
                m_contexts->update(m_vm->runner()->qmp());
            }
            refresh();
        }
    });
    m_growing->start();
}

void VmDetails::setVm(Vm *vm)
{
    if (vm != m_vm) {
        m_contexts->reset();
    }
    m_vm = vm;
    refresh();
}

void VmDetails::setError(const QString &error)
{
    m_error->setText(error.toHtmlEscaped().replace('\n', "<br>"));
    m_error->setVisible(!error.isEmpty());
}

void VmDetails::refresh()
{
    if (!m_vm) {
        m_name->clear();
        m_state->clear();
        m_text->clear();
        m_note->hide();
        m_contextsNote->hide();
        return;
    }
    m_name->setText(m_vm->name());
    m_state->setText(stateText(m_vm));
    m_note->setVisible(keptOpen(m_vm));

    /* native context asked for: what the guest does with it, while it runs */
    const VmConfig::Graphics graphics = VmConfig::graphics(m_vm->args());
    const bool native = graphics.nativeContext;
    if (!m_vm->runner()->isActive()) {
        m_contexts->reset();
    } else if ((native || graphics.venus) &&
               m_contexts->status() == GpuContexts::Status::Unknown) {
        m_contexts->update(m_vm->runner()->qmp());
    }
    switch (native && m_vm->runner()->isActive() ? m_contexts->status()
                                                 : GpuContexts::Status::Unknown) {
    case GpuContexts::Status::Virgl:
        m_contextsNote->setText(
            tr("DRM native context was enabled, but this guest's Mesa doesn't support it. You may "
               "refer to <a href=\"%1\">Building Mesa with native context</a>.")
                .arg(kGuestMesaGuide));
        m_contextsNote->show();
        break;
    case GpuContexts::Status::NotOffered:
        m_contextsNote->setText(
            tr("This computer offers the guest no DRM native context: the virglrenderer QEMU "
               "uses has none for its GPU. File > Build QEMU builds one with native context."));
        m_contextsNote->show();
        break;
    default:
        m_contextsNote->hide();
    }

    /* the documentation of the VM's QEMU tells the kinds of its devices */
    QemuDocs *docs = QemuDocs::forArgs(m_vm->args());
    if (docs != m_docs) {
        if (m_docs) {
            m_docs->disconnect(this);
        }
        m_docs = docs;
        connect(docs, &QemuDocs::changed, this, &VmDetails::refresh);
    }

    /* keep the scroll position across updates */
    const int scroll = m_text->verticalScrollBar()->value();
    m_text->setHtml(html());
    m_text->verticalScrollBar()->setValue(scroll);
}

/* "12.4 GiB used of 64 GiB" */
static QString usageText(const DiskInfo::Usage &u)
{
    const QLocale locale;
    auto size = [&locale](qint64 bytes) {
        return locale.formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat);
    };

    return u.capacity > 0 ? VmDetails::tr("%1 used of %2").arg(size(u.used), size(u.capacity))
                          : VmDetails::tr("%1 used").arg(size(u.used));
}

QString VmDetails::html() const
{
    using Rows = QList<std::pair<QString, QString>>;
    const ArgsFile &args = m_vm->args();
    const QemuInfo *info = m_docs->info();
    const QString dim = palette().color(QPalette::PlaceholderText).name();
    QString html;

    auto section = [&](const QString &title, const Rows &rows) {
        if (rows.isEmpty()) {
            return;
        }
        html += QString("<h3>%1</h3><table cellspacing=\"0\" cellpadding=\"2\">")
                    .arg(title.toHtmlEscaped());
        for (const auto &[key, value] : rows) {
            html += QString("<tr><td style=\"color:%1\">%2&nbsp;&nbsp;&nbsp;</td>"
                            "<td>%3</td></tr>")
                        .arg(dim, key.toHtmlEscaped(), value);
        }
        html += "</table>";
    };
    auto text = [](const QString &plain) { return plain.toHtmlEscaped(); };

    /* System */
    const qint64 memory = VmConfig::memoryMiB(args);
    const VmConfig::Cpus cpus = VmConfig::cpus(args);
    const QString machine = VmConfig::machineType(args);
    const QString accel = VmConfig::accel(args);
    QString processors = QString::number(cpus.count);
    QString accelText;

    if (!cpus.model.isEmpty()) {
        processors += " · " + cpus.model;
    }
    if (cpus.sockets > 0 || cpus.cores > 0 || cpus.threads > 0) {
        processors += tr(" (%1 sockets, %2 cores, %3 threads)")
                          .arg(qMax(cpus.sockets, 1))
                          .arg(qMax(cpus.cores, 1))
                          .arg(qMax(cpus.threads, 1));
    }
    if (accel == "kvm") {
        accelText = tr("KVM");
    } else if (accel.isEmpty() || accel == "tcg") {
        accelText = tr("TCG, software emulation");
    } else {
        accelText = accel;
    }
    section(tr("System"),
            {{tr("Memory"), text(memory > 0 ? sizeText(memory) : tr("QEMU default"))},
             {tr("Processors"), text(processors)},
             {tr("Machine"), text(machine.isEmpty() ? tr("QEMU default") : machine)},
             {tr("Acceleration"), text(accelText)},
             {tr("Firmware"), text(UiConfig::firmwareSummary(args))}});

    Rows display{{tr("Graphics"), text(UiConfig::displaySummary(args, info))}};
    const VmConfig::Graphics graphics = VmConfig::graphics(args);
    if (graphics.kind == VmConfig::Graphics::Accelerated) {
        /* what the guest does with them, once it runs */
        const bool running = m_vm->runner()->isActive();
        const GpuContexts::Status status =
            running ? m_contexts->status() : GpuContexts::Status::Unknown;
        QString native = tr("off");
        QString venus = tr("off");

        if (graphics.nativeContext) {
            switch (status) {
            case GpuContexts::Status::InUse:
                native = tr("on, in use");
                break;
            case GpuContexts::Status::Virgl:
                native = tr("on, but the guest draws through virgl");
                break;
            case GpuContexts::Status::NotOffered:
                native = tr("on, but this computer does not offer it");
                break;
            case GpuContexts::Status::Waiting:
                native = tr("on; the guest has not drawn in 3D yet");
                break;
            case GpuContexts::Status::Unknown:
                native = tr("on");
                break;
            }
        }
        if (graphics.venus) {
            venus = status == GpuContexts::Status::Unknown ? tr("on")
                    : m_contexts->venusUsed()              ? tr("on, in use")
                                                           : tr("on, not used yet");
        }
        display << std::pair(tr("DRM native context"), text(native))
                << std::pair(tr("Venus"), text(venus));
    }
    section(tr("Display"), display);

    /* Storage */
    Rows storage;
    for (const VmConfig::Disk &disk : VmConfig::disks(args)) {
        QString kind = disk.cdrom ? tr("CD/DVD") : tr("Disk");
        if (disk.bus != VmConfig::Disk::Other) {
            kind += QString(" (%1)").arg(UiConfig::busName(disk.bus));
        }
        QString value = disk.file.isEmpty() ? text(tr("empty")) : text(disk.file);
        if (!disk.cdrom && !disk.file.isEmpty()) {
            /* the path is relative to the folder QEMU runs in */
            const DiskInfo::Usage u =
                DiskInfo::usage(QDir(m_vm->dir()).absoluteFilePath(disk.file));
            if (u.used >= 0) {
                value += QString("<br><span style=\"color:%1\">%2</span>")
                             .arg(dim, text(usageText(u)));
            }
        }
        storage << std::pair(kind, value);
    }
    if (storage.isEmpty()) {
        storage << std::pair(tr("Disks"), text(tr("none")));
    }
    section(tr("Storage"), storage);

    section(tr("Network and Sound"),
            {{tr("Network"), text(UiConfig::networkSummary(args, info))},
             {tr("Sound"), text(UiConfig::audioSummary(args, info))}});

    /* Shared folders */
    Rows shares;
    for (const VmConfig::Share &s : VmConfig::shares(args)) {
        shares << std::pair(s.tag, link(s.path) + (s.readonly ? text(tr(" · read only"))
                                                              : QString()));
    }
    section(tr("Shared Folders"), shares);

    /* Passthrough */
    const QList<VmConfig::UsbId> usbIds = VmConfig::usbPassthrough(args);
    Rows usb;
    if (!usbIds.isEmpty()) {
        const QList<UsbDevice> devices = HostDevices::usbDevices();
        for (const VmConfig::UsbId &id : usbIds) {
            QString name = tr("not connected");
            for (const UsbDevice &dev : devices) {
                if (dev.vendorId == id.vendor && dev.productId == id.product) {
                    name = QString("%1 %2").arg(dev.manufacturer, dev.product).simplified();
                }
            }
            usb << std::pair(QString("%1:%2")
                                 .arg(id.vendor, 4, 16, QChar('0'))
                                 .arg(id.product, 4, 16, QChar('0')),
                             text(name));
        }
        if (!VmConfig::hasUsbController(args)) {
            usb << std::pair(tr("Warning"), text(tr("no USB controller")));
        }
    }
    section(tr("USB Devices"), usb);

    const QStringList pciAddresses = VmConfig::pciPassthrough(args);
    Rows pci;
    if (!pciAddresses.isEmpty()) {
        const QList<PciDevice> devices = HostDevices::pciDevices();
        for (const QString &address : pciAddresses) {
            QString name = tr("not found");
            for (const PciDevice &dev : devices) {
                if (dev.address == address) {
                    name = QString("%1 %2").arg(dev.vendorName, dev.deviceName).simplified();
                }
            }
            pci << std::pair(address, text(name));
        }
    }
    section(tr("PCI Devices"), pci);

    /* Files */
    Rows files = {{tr("Folder"), link(m_vm->dir())}};
    if (QFileInfo::exists(m_vm->runner()->logPath())) {
        files << std::pair(tr("Log"), link(m_vm->runner()->logPath()));
    }
    section(tr("Files"), files);
    return html;
}
