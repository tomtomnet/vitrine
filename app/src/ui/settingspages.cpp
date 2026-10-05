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
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "core/cardsettings.h"
#include "core/firmware.h"
#include "core/firmwarefiles.h"
#include "core/guestos.h"
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
#include "ui/oschooser.h"
#include "ui/qemudocs.h"
#include "ui/referencepanel.h"
#include "ui/systems.h"
#include "ui/uiconfig.h"
#include "ui/widgets.h"

/* General */

/* The desktops of #guest, by the names the page gives them */
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
    : SettingsPage(parent), m_name(new QLineEdit), m_os(new OsChooser), m_desktop(new QComboBox)
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
    form->addRow(QString(), Widgets::hint(tr("The list of VMs shows its logo; the guest tools "
                                             "are for Fedora.")));
    form->addRow(tr("&Desktop:"), m_desktop);
    form->addRow(QString(), Widgets::hint(tr("The timing of the VM's frames depends on its "
                                             "desktop.")));
    form->addRow(tr("Folder:"), folder);
    form->addRow(QString(), Widgets::hint(tr("The folder holds the arguments (vm.args), the disks the "
                                    "VM creates and its log.")));
    layout->addLayout(form);
    layout->addStretch();
    connect(m_os, &OsChooser::systemChosen, this, &GeneralPage::updateDesktop);
}

QIcon GeneralPage::icon() const
{
    return Icons::themed({"preferences-system", "configure"}, QStyle::SP_ComputerIcon);
}

void GeneralPage::updateDesktop()
{
    const QString os = m_os->family();
    m_desktop->setEnabled(os == "linux" || os.isEmpty());
}

VmConfig::Guest GeneralPage::shown() const
{
    /* a desktop for Linux, or a system not known */
    return {m_os->family(),
            m_desktop->isEnabled() ? m_desktop->currentData().toString() : QString(),
            m_os->id()};
}

void GeneralPage::load(const ArgsFile &args)
{
    const VmConfig::Guest guest = VmConfig::guest(args);

    m_loaded = VmConfig::name(args);
    m_name->setText(m_loaded);
    m_os->setSystem(guest.id, guest.os);
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
    if (guest.os != m_loadedGuest.os || guest.desktop != m_loadedGuest.desktop ||
        guest.id != m_loadedGuest.id) {
        VmConfig::setGuest(args, guest);
        m_loadedGuest = guest;
    }
}

bool GeneralPage::isModified() const
{
    const QString name = m_name->text().trimmed();
    const VmConfig::Guest guest = shown();

    return (!name.isEmpty() && name != m_loaded) || guest.os != m_loadedGuest.os ||
           guest.desktop != m_loadedGuest.desktop || guest.id != m_loadedGuest.id;
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

/* The options of QEMU's windows the page has, each window's, and QEMU's defaults */
static const char *const kWindowOptions[][2] = {
    {"show-cursor", QT_TRANSLATE_NOOP("DisplayPage", "Show the mouse &pointer")},
    {"zoom-to-fit", QT_TRANSLATE_NOOP("DisplayPage", "&Zoom the screen to fit the window")},
    {"show-menubar", QT_TRANSLATE_NOOP("DisplayPage", "Menu &bar")},
    {"grab-on-hover",
     QT_TRANSLATE_NOOP("DisplayPage", "Take the &keyboard when the pointer is over the window")},
};
static const QHash<QString, QStringList> kWindowKeys = {
    {"sdl", {"show-cursor"}},
    {"gtk", {"show-cursor", "zoom-to-fit", "show-menubar", "grab-on-hover"}},
};
static bool windowOptionDefault(const QString &key)
{
    return key == "show-menubar";
}

/* The cards of the list, by the machine: virt on ARM has no VGA */
static QList<std::pair<QString, QString>> cardChoices(const ArgsFile &args)
{
    if (VmConfig::machineType(args).startsWith("virt")) {
        return {{"virtio-gpu-gl-pci", DisplayPage::tr("3D (virtio-gpu-gl-pci)")},
                {"virtio-gpu-pci", DisplayPage::tr("2D (virtio-gpu-pci)")}};
    }
    return {{"virtio-vga-gl", DisplayPage::tr("3D, with VGA (virtio-vga-gl)")},
            {"virtio-gpu-gl-pci", DisplayPage::tr("3D, without VGA (virtio-gpu-gl-pci)")},
            {"virtio-vga", DisplayPage::tr("2D, with VGA (virtio-vga)")},
            {"VGA", DisplayPage::tr("Standard VGA (VGA)")}};
}

/* A driver of the list as VmConfig's graphics */
static VmConfig::Graphics cardGraphics(const QString &driver)
{
    VmConfig::Graphics g;

    g.device = driver;
    g.kind = VmConfig::isAccelerated(driver) ? VmConfig::Graphics::Accelerated
             : driver == "VGA"               ? VmConfig::Graphics::Standard
                                             : VmConfig::Graphics::Virtio;
    return g;
}

DisplayPage::DisplayPage(QWidget *parent)
    : SettingsPage(parent), m_custom(new Banner(Banner::Information)),
      m_embedded(new QRadioButton(tr("In &Vitrine's window"))),
      m_sdl(new QRadioButton(tr("In QEMU's &SDL window"))),
      m_gtk(new QRadioButton(tr("In QEMU's &GTK window"))),
      m_nowhere(new QRadioButton(tr("&Nowhere"))), m_gtkHint(Widgets::hint()),
      m_windowLabel(new QLabel(tr("QEMU's window:"))), m_card(new QComboBox),
      m_cardHint(Widgets::hint()),
      m_nativeContext(new QCheckBox(tr("&DRM native context"))),
      m_venus(new QCheckBox(tr("V&enus"))),
      m_frameTiming(new QCheckBox(tr("Frames in s&tep with this computer's screen"))),
      m_nativeContextHint(Widgets::hint()), m_venusHint(Widgets::hint()),
      m_frameTimingHint(Widgets::hint())
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *choices = new QVBoxLayout;
    auto *windowOptions = new QVBoxLayout;
    auto *features = new QVBoxLayout;
    auto *group = new QButtonGroup(this);

    m_custom->setObjectName("customDisplay");
    m_embedded->setObjectName("embedded");
    m_sdl->setObjectName("ownWindow");
    m_gtk->setObjectName("gtkWindow");
    m_nowhere->setObjectName("nowhere");
    m_card->setObjectName("card");
    m_cardHint->setObjectName("cardHint");
    m_nativeContext->setObjectName("nativeContext");
    m_venus->setObjectName("venus");
    m_frameTiming->setObjectName("frameTiming");
    for (QRadioButton *b : {m_embedded, m_sdl, m_gtk, m_nowhere}) {
        group->addButton(b);
    }
    choices->addWidget(m_embedded);
    choices->addWidget(Widgets::hint(tr("The screen is part of Vitrine's window, and can go "
                                        "full screen.")));
    choices->addWidget(m_sdl);
    choices->addWidget(Widgets::hint(tr("QEMU shows the screen in a window of its own, which "
                                        "stays open when Vitrine closes.")));
    choices->addWidget(m_gtk);
    choices->addWidget(m_gtkHint);
    choices->addWidget(m_nowhere);
    choices->addWidget(Widgets::hint(tr("The VM runs without a screen.")));
    form->addRow(Widgets::label(tr("Show the VM:"), m_embedded), choices);

    for (const auto &[key, text] : kWindowOptions) {
        auto *box = new QCheckBox(tr(text));
        box->setObjectName(key);
        windowOptions->addWidget(box);
        m_options.insert(key, box);
    }
    form->addRow(m_windowLabel, windowOptions);

    form->addRow(Widgets::label(tr("Graphics &card:"), m_card), m_card);
    form->addRow(QString(), m_cardHint);
    features->addWidget(m_nativeContext);
    features->addWidget(m_nativeContextHint);
    features->addWidget(m_venus);
    features->addWidget(m_venusHint);
    features->addWidget(m_frameTiming);
    features->addWidget(m_frameTimingHint);
    form->addRow(Widgets::label(tr("3D:"), m_nativeContext), features);

    layout->addWidget(m_custom);
    layout->addLayout(form);
    layout->addStretch();

    connect(group, &QButtonGroup::buttonToggled, this, &DisplayPage::update);
    connect(m_card, &QComboBox::currentIndexChanged, this, &DisplayPage::update);
}

QIcon DisplayPage::icon() const
{
    return Icons::themed({"video-display", "preferences-desktop-display"},
                         QStyle::SP_DesktopIcon);
}

QString DisplayPage::chosen3dCard() const
{
    const QString card = m_card->currentData().toString();
    return m_card->isEnabled() && VmConfig::isAccelerated(card) ? card : QString();
}

DisplayPage::Shown DisplayPage::shown() const
{
    Shown s;

    s.screen = m_embedded->isChecked() ? "dbus"
               : m_sdl->isChecked()    ? "sdl"
               : m_gtk->isChecked()    ? "gtk"
               : m_nowhere->isChecked() ? "none"
                                        : QString();
    for (const QString &key : kWindowKeys.value(s.screen)) {
        s.options.insert(key, m_options.value(key)->isChecked());
    }
    s.card = m_card->currentData().toString();
    /* vitrine's settings of a 3D card, of those its QEMU has */
    if (!chosen3dCard().isEmpty()) {
        s.nativeContext = m_nativeContext->isEnabled() && m_nativeContext->isChecked();
        s.venus = m_venus->isEnabled() && m_venus->isChecked();
        s.frameTiming = m_frameTiming->isEnabled() && m_frameTiming->isChecked();
    }
    return s;
}

void DisplayPage::update()
{
    const Shown s = shown();
    const QString card = chosen3dCard();
    /* asked about the 3D card chosen, else the VM's own if it has one */
    const std::optional<CardSettings::Offers> offers = CardSettings::offers(m_args, card);
    const auto lacks = [&offers](const QString &display) {
        return offers && offers->displays && !offers->displays->contains(display);
    };
    struct Feature {
        QCheckBox *box;
        QLabel *hint;
        CardSettings::Feature feature;
        QString text;
    };
    const Feature features[] = {
        {m_nativeContext, m_nativeContextHint, CardSettings::Feature::NativeContext,
         tr("The guest's 3D goes through this computer's GPU driver, with the Mesa the "
            "guest tools install.")},
        {m_venus, m_venusHint, CardSettings::Feature::Venus,
         tr("Vulkan for a GPU without native context.")},
        {m_frameTiming, m_frameTimingHint, CardSettings::Feature::FrameTiming,
         tr("Smooth frames in Vitrine's window and the SDL one, with the guest tools' "
            "driver.")},
    };

    /* the displays its QEMU has, and the one it uses; none for -nographic */
    const bool nographic = m_args.indexOf("nographic") >= 0;
    for (const auto &[button, display] :
         {std::pair(m_embedded, "dbus"), std::pair(m_sdl, "sdl"), std::pair(m_gtk, "gtk")}) {
        button->setEnabled(!nographic && (button->isChecked() || !lacks(display)));
    }
    m_nowhere->setEnabled(!nographic);
    m_gtkHint->setText(lacks("gtk") ? tr("This VM's QEMU has no GTK display.")
                                    : tr("The same, with QEMU's menus; there the frames do "
                                         "not keep step with this computer's screen."));

    /* each window its options */
    const QStringList keys = kWindowKeys.value(s.screen);
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it) {
        it.value()->setVisible(keys.contains(it.key()));
    }
    m_windowLabel->setVisible(!keys.isEmpty());

    /* the card */
    const QString driver = m_card->currentData().toString();
    if (!m_card->isEnabled()) {
        m_cardHint->setText(tr("The graphics of this VM are set up by hand: change them on "
                               "the Arguments page."));
    } else if (VmConfig::machineType(m_args).startsWith("virt")) {
        m_cardHint->clear();
    } else if (VmConfig::isVgaDevice(driver)) {
        m_cardHint->setText(tr("It shows the firmware, the boot loader and the first boot "
                               "messages too."));
    } else if (driver.startsWith("virtio-gpu")) {
        m_cardHint->setText(tr("Nothing shows before the guest's driver starts, but UEFI's "
                               "own screens."));
    } else {
        m_cardHint->clear();
    }
    m_cardHint->setVisible(!m_cardHint->text().isEmpty());

    /* its 3D, with what the VM's QEMU has */
    for (const Feature &f : features) {
        const bool offered = offers && CardSettings::isOffered(*offers, f.feature);
        f.box->setEnabled(!card.isEmpty() && offered);
        f.hint->setText(card.isEmpty() ? f.text
                        : !offers      ? tr("This VM's QEMU did not say what it has.")
                        : offered      ? f.text
                        : !offers->card ? tr("Vitrine's QEMU is built without it.")
                                        : tr("This VM's QEMU lacks it."));
    }
}

void DisplayPage::load(const ArgsFile &args)
{
    const VmConfig::Screen screen = VmConfig::screen(args);
    const VmConfig::Graphics g = VmConfig::graphics(args);
    const QString display = g.display;
    QString radio;
    QString custom;

    m_args = args;
    /* the screen: none checked for one shown elsewhere, or in another window */
    if (screen == VmConfig::Screen::Embedded) {
        radio = "dbus";
    } else if (screen == VmConfig::Screen::OwnWindow && (display == "sdl" || display == "gtk")) {
        radio = display;
    } else if (screen == VmConfig::Screen::None && !VmConfig::hasRemoteDisplay(args) &&
               args.indexOf("nographic") < 0 && (display == "none" || display == "egl-headless")) {
        radio = "none";
    }
    /*
     * The buttons' group keeps one checked while it is exclusive, whatever
     * their autoExclusive says, and a button left checked would make an
     * untouched page modified
     */
    {
        const QSignalBlocker block(m_embedded->group());
        m_embedded->group()->setExclusive(false);
        m_embedded->setChecked(radio == "dbus");
        m_sdl->setChecked(radio == "sdl");
        m_gtk->setChecked(radio == "gtk");
        m_nowhere->setChecked(radio == "none");
        m_embedded->group()->setExclusive(true);
    }

    if (args.indexOf("nographic") >= 0) {
        custom = tr("This VM has no screen (-nographic): change it on the Arguments page.");
    } else if (screen == VmConfig::Screen::None && radio.isEmpty() &&
               (display.isEmpty() || display == "default" || display == "none" ||
                display == "egl-headless")) {
        custom = tr("This VM shows its screen over VNC or SPICE only. Choosing below adds a "
                    "window.");
    } else if (screen == VmConfig::Screen::None && radio.isEmpty()) {
        custom = tr("This VM shows its screen nowhere Vitrine can (-display %1). Choosing "
                    "below replaces it.")
                     .arg(display.toHtmlEscaped());
    } else if (screen == VmConfig::Screen::OwnWindow && radio.isEmpty()) {
        custom = display.isEmpty() || display == "default"
                     ? tr("The screen shows in QEMU's default window.")
                     : tr("The screen shows in QEMU's %1 window.")
                           .arg(display.toUpper().toHtmlEscaped());
    }
    m_custom->setText(custom);
    m_custom->setVisible(!custom.isEmpty());

    /* the window's options, as QEMU takes them when not given */
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it) {
        it.value()->setChecked(
            VmConfig::displayFlag(args, it.key(), windowOptionDefault(it.key())));
    }

    /* the card: one of the list, else the VM's own as it is */
    {
        const QSignalBlocker block(m_card);
        const bool custom = g.kind == VmConfig::Graphics::Custom;
        QString own = g.device;

        m_card->clear();
        for (const auto &[driver, text] : cardChoices(args)) {
            m_card->addItem(text, driver);
        }
        if (custom) {
            own = tr("Set up by hand (%1)").arg(g.custom);
        } else if (own.isEmpty()) {
            own = g.kind == VmConfig::Graphics::None    ? tr("None")
                  : g.kind == VmConfig::Graphics::Virtio ? QString("virtio-vga")
                                                         : QString("VGA");
        }
        if (m_card->findData(own) < 0) {
            m_card->addItem(own, own);
        }
        m_card->setCurrentIndex(m_card->findData(own));
        m_card->setEnabled(!custom);
    }
    m_nativeContext->setChecked(CardSettings::isOn(args, CardSettings::Feature::NativeContext));
    m_venus->setChecked(CardSettings::isOn(args, CardSettings::Feature::Venus));
    m_frameTiming->setChecked(CardSettings::isOn(args, CardSettings::Feature::FrameTiming));

    /* as the page shows it, so that an untouched page writes nothing */
    m_loaded.card = m_card->currentData().toString();
    update();
    m_loaded = shown();
}

void DisplayPage::save(ArgsFile &args)
{
    const Shown s = shown();

    if (s == m_loaded) {
        return;
    }
    const bool cardChanged = s.card != m_loaded.card;
    /* the card, its 3D as it was: its settings follow */
    if (cardChanged) {
        VmConfig::Graphics g = cardGraphics(s.card);
        const VmConfig::Graphics now = VmConfig::graphics(args);
        g.display = now.display;
        g.nativeContext = now.nativeContext && g.kind == VmConfig::Graphics::Accelerated;
        g.venus = now.venus && g.kind == VmConfig::Graphics::Accelerated;
        VmConfig::setGraphics(args, g);
    }
    if (!chosen3dCard().isEmpty()) {
        if (const std::optional<CardSettings::Offers> offers =
                CardSettings::offers(args, chosen3dCard())) {
            const std::pair<CardSettings::Feature, bool> features[] = {
                {CardSettings::Feature::NativeContext, s.nativeContext},
                {CardSettings::Feature::Venus, s.venus},
                {CardSettings::Feature::FrameTiming, s.frameTiming},
            };
            for (const auto &[feature, on] : features) {
                if (CardSettings::isOn(args, feature) != on) {
                    CardSettings::set(args, feature, on, *offers);
                }
            }
        }
    }
    /* the screen, after the card: a 3D one shows nowhere through egl-headless */
    if (!s.screen.isEmpty() && (s.screen != m_loaded.screen || (cardChanged && s.screen == "none"))) {
        VmConfig::setScreen(args, s.screen == "dbus"   ? VmConfig::Screen::Embedded
                                  : s.screen == "none" ? VmConfig::Screen::None
                                                       : VmConfig::Screen::OwnWindow,
                            s.screen);
    }
    /* in the page's order; QEMU's default, said by leaving the key out */
    for (const auto &[key, text] : kWindowOptions) {
        const bool on = s.options.value(key);
        const bool qemuDefault = windowOptionDefault(key);
        if (s.options.contains(key) && VmConfig::displayFlag(args, key, qemuDefault) != on) {
            VmConfig::setDisplayOption(args, key,
                                       on == qemuDefault ? QString()
                                       : on              ? QString("on")
                                                         : QString("off"));
        }
    }
    load(args);
}

bool DisplayPage::isModified() const
{
    return shown() != m_loaded;
}

/* Storage */

StoragePage::StoragePage(const QString &vmDir, QWidget *parent)
    : SettingsPage(parent), m_vmDir(vmDir), m_table(new QTableWidget(0, 4)),
      m_disc(new QPushButton(tr("Choose &Disc…"))), m_eject(new QPushButton(tr("&Eject"))),
      m_resize(new QPushButton(tr("Si&ze…"))), m_remove(new QPushButton(tr("&Remove"))),
      m_boot(new QListWidget), m_bootUp(new QPushButton(tr("Move &Up"))),
      m_bootDown(new QPushButton(tr("Move Dow&n"))), m_bootHint(Widgets::hint()),
      m_system(Widgets::hint())
{
    /* the boot order's buttons in the column of the disks' */
    auto *layout = new QGridLayout(this);
    auto *buttons = new QVBoxLayout;
    auto *bootButtons = new QVBoxLayout;
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

    /* each button of the column with its icon, not some */
    addDisk->setIcon(Icons::themed({"drive-harddisk", "list-add"}, QStyle::SP_DriveHDIcon));
    addCdrom->setIcon(Icons::themed({"drive-optical", "list-add"}, QStyle::SP_DriveCDIcon));
    m_disc->setIcon(Icons::themed({"document-open"}, QStyle::SP_DialogOpenButton));
    m_eject->setIcon(Icons::themed({"media-eject"}, QStyle::SP_ArrowUp));
    m_resize->setIcon(Icons::themed({"transform-scale", "document-edit"},
                                    QStyle::SP_FileDialogDetailedView));
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

    /* the devices to start from, as virt-manager has them: checked and in order */
    m_boot->setObjectName("bootOrder");
    m_boot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_bootUp->setObjectName("bootUp");
    m_bootDown->setObjectName("bootDown");
    m_bootUp->setIcon(Icons::themed({"go-up", "arrow-up"}, QStyle::SP_ArrowUp));
    m_bootDown->setIcon(Icons::themed({"go-down", "arrow-down"}, QStyle::SP_ArrowDown));
    bootButtons->addWidget(m_bootUp);
    bootButtons->addWidget(m_bootDown);
    bootButtons->addStretch();

    layout->setColumnStretch(0, 1);
    layout->addWidget(Widgets::note(tr("The disks and CD/DVD drives of the VM. New disks are "
                                       "created in the VM folder when you apply; removing a "
                                       "disk takes it out of the VM, but its file stays.")),
                      0, 0, 1, 2);
    auto *disks = new QVBoxLayout;
    disks->addWidget(m_table, 1);
    disks->addWidget(m_system);
    m_system->setObjectName("discSystem");
    m_system->hide();
    layout->addLayout(disks, 1, 0);
    layout->addLayout(buttons, 1, 1);
    layout->setRowStretch(1, 1);
    layout->addWidget(Widgets::heading(tr("Boot order")), 2, 0, 1, 2);
    layout->addWidget(m_boot, 3, 0, Qt::AlignTop);
    layout->addLayout(bootButtons, 3, 1);
    layout->addWidget(m_bootHint, 4, 0, 1, 2);

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
        /* out of the boot order too */
        const int id = m_entries[i].id;
        m_bootItems.removeIf([id](const BootItem &item) { return item.entry == id; });
        if (m_entries[i].disk.line < 0) {
            m_entries.removeAt(i);
        } else {
            m_entries[i].removed = true;
        }
        fill();
    });
    connect(m_boot, &QListWidget::itemChanged, this, &StoragePage::bootChanged);
    connect(m_boot, &QListWidget::currentRowChanged, this, &StoragePage::updateButtons);
    connect(m_bootUp, &QPushButton::clicked, this, [this]() { moveBoot(-1); });
    connect(m_bootDown, &QPushButton::clicked, this, [this]() { moveBoot(1); });
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
    QStringList cards;
    const VmConfig::Guest guest = VmConfig::guest(args);

    m_virt = VmConfig::machineType(args).startsWith("virt");
    m_systemSet = !guest.id.isEmpty();
    m_family = guest.os;
    m_toldDisc.clear();
    m_toldSystem.clear();
    m_entries.clear();
    for (const VmConfig::Disk &d : VmConfig::disks(args)) {
        m_entries << Entry{d, d.file, m_pending.value(d.file), false, m_nextId++};
    }
    /* the network cards, as VmConfig counts them: devices of a -netdev */
    for (int i : args.indexesOf("device")) {
        if (args.valueAt(i).has("netdev")) {
            cards << args.valueAt(i).implied();
        }
    }
    m_bootSet = VmConfig::hasBootOrder(args);
    m_bootChanged = false;
    m_bootItems.clear();
    for (const VmConfig::BootEntry &b : VmConfig::bootOrder(args)) {
        BootItem item;
        item.kind = b.kind;
        item.on = b.on;
        item.editable = b.editable;
        if (b.kind == VmConfig::BootEntry::Network) {
            item.nic = b.index;
            item.card = cards.value(b.index);
        } else {
            item.entry = m_entries.value(b.index).id;
        }
        m_bootItems << item;
    }
    fill();
}

void StoragePage::save(ArgsFile &args)
{
    QList<VmConfig::Disk> removed;

    /* the system the disc chosen told, for a VM without one */
    if (const QString system = toldSystem(); !system.isEmpty()) {
        VmConfig::Guest guest = VmConfig::guest(args);
        if (guest.id.isEmpty()) {
            guest.id = system;
            guest.os = GuestOs::guestFamily(GuestOs::Catalogue::instance().find(system));
            if (guest.os != "linux") {
                guest.desktop.clear();
            } else if (guest.desktop.isEmpty()) {
                guest.desktop = m_toldDesktop;
            }
            VmConfig::setGuest(args, guest);
        }
        m_systemSet = true;
        m_toldSystem.clear();
        updateSystem();
    }

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
    if (m_bootChanged) {
        /* the disks now: those kept, then those added, as the page lists them */
        QList<int> ids;
        QList<VmConfig::BootEntry> order;
        for (const Entry &e : std::as_const(m_entries)) {
            if (!e.removed) {
                ids << e.id;
            }
        }
        for (const BootItem &item : std::as_const(m_bootItems)) {
            VmConfig::BootEntry b;
            b.kind = item.kind;
            b.index = item.kind == VmConfig::BootEntry::Network ? item.nic
                                                                 : int(ids.indexOf(item.entry));
            b.on = item.on;
            b.editable = item.editable;
            if (b.index >= 0) {
                order << b;
            }
        }
        VmConfig::setBootOrder(args, order);
    }
    load(args);
}

bool StoragePage::isModified() const
{
    if (m_bootChanged) {
        return true;
    }
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

    updateSystem();
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
    fillBoot();
    updateButtons();
}

/*
 * A disk added on the page: in the firmware's own order (none set), with
 * the others of its kind, on; in an order set, off at the end, which is
 * what the VM does with it until the order changes
 */
void StoragePage::addBootItem(const Entry &entry)
{
    BootItem item;

    item.kind = entry.disk.cdrom ? VmConfig::BootEntry::Cdrom : VmConfig::BootEntry::HardDisk;
    item.entry = entry.id;
    if (m_bootSet || m_bootChanged) {
        m_bootItems << item;
        return;
    }
    item.on = true;
    /* after the last of its kind, or before the first of a kind that comes after */
    qsizetype at = m_bootItems.size();
    for (qsizetype i = 0; i < m_bootItems.size(); i++) {
        if (m_bootItems[i].kind > item.kind || !m_bootItems[i].on) {
            at = i;
            break;
        }
    }
    m_bootItems.insert(at, item);
}

/* The boot order's list from m_bootItems: checked, unchecked, or greyed */
void StoragePage::fillBoot()
{
    const QSignalBlocker block(m_boot);
    const int row = m_boot->currentRow();

    m_boot->clear();
    for (qsizetype i = 0; i < m_bootItems.size(); i++) {
        const BootItem &b = m_bootItems[i];
        QString text;
        QIcon icon;

        if (b.kind == VmConfig::BootEntry::Network) {
            icon = Icons::themed({"network-wired"}, QStyle::SP_DriveNetIcon);
            text = tr("Network (PXE): %1").arg(b.card);
        } else {
            const auto e = std::find_if(m_entries.cbegin(), m_entries.cend(),
                                        [&b](const Entry &e) { return e.id == b.entry; });
            if (e == m_entries.cend() || e->removed) {
                continue;
            }
            const bool cdrom = b.kind == VmConfig::BootEntry::Cdrom;
            const QString file = QFileInfo(e->file).fileName();
            icon = Icons::themed({cdrom ? "drive-optical" : "drive-harddisk"},
                                 cdrom ? QStyle::SP_DriveCDIcon : QStyle::SP_DriveHDIcon);
            text = cdrom ? (file.isEmpty() ? tr("CD/DVD: empty") : tr("CD/DVD: %1").arg(file))
                         : tr("Hard disk: %1").arg(file);
        }
        auto *item = new QListWidgetItem(icon, text, m_boot);
        item->setData(Qt::UserRole, int(i));
        if (b.editable) {
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            item->setCheckState(b.on ? Qt::Checked : Qt::Unchecked);
        } else {
            /* -drive if=scsi, or a -blockdev without its -device */
            item->setFlags(Qt::NoItemFlags);
            item->setCheckState(Qt::Unchecked);
            item->setToolTip(tr("Set up by hand: see the Arguments page"));
        }
    }
    /* as high as its rows: a few devices, never scrolled */
    int height = 2 * m_boot->frameWidth();
    for (int i = 0; i < m_boot->count(); i++) {
        height += m_boot->sizeHintForRow(i);
    }
    m_boot->setFixedHeight(qMax(height, m_boot->fontMetrics().height() + 2 * m_boot->frameWidth()));
    if (m_boot->count() > 0) {
        m_boot->setCurrentRow(qBound(0, row, m_boot->count() - 1));
    }
    m_bootHint->setText(m_bootSet || m_bootChanged
                            ? tr("The VM starts from the first checked device it can start "
                                 "from. The others stay in the VM, but it never starts from "
                                 "them.")
                            : tr("No order is set: the firmware starts from the devices in an "
                                 "order of its own. Changing the list sets one."));
}

/* A device checked or unchecked: one stays checked, to start from */
void StoragePage::bootChanged(QListWidgetItem *item)
{
    const int i = item->data(Qt::UserRole).toInt();
    const bool on = item->checkState() == Qt::Checked;

    if (i < 0 || i >= m_bootItems.size() || m_bootItems[i].on == on) {
        return;
    }
    if (!on && std::count_if(m_bootItems.cbegin(), m_bootItems.cend(),
                             [](const BootItem &b) { return b.on; }) <= 1) {
        const QSignalBlocker block(m_boot);
        item->setCheckState(Qt::Checked);
        return;
    }
    m_bootItems[i].on = on;
    m_bootChanged = true;
    fillBoot();
}

void StoragePage::moveBoot(int by)
{
    QListWidgetItem *item = m_boot->currentItem();
    const int row = m_boot->currentRow();

    if (!item || row + by < 0 || row + by >= m_boot->count()) {
        return;
    }
    const int from = item->data(Qt::UserRole).toInt();
    const int to = m_boot->item(row + by)->data(Qt::UserRole).toInt();
    m_bootItems.swapItemsAt(from, to);
    m_bootChanged = true;
    fillBoot();
    m_boot->setCurrentRow(row + by);
}

void StoragePage::updateButtons()
{
    const int i = current();
    const bool editable = i >= 0 && m_entries[i].disk.editable;
    const bool cdrom = editable && m_entries[i].disk.cdrom;
    const int boot = m_boot->currentRow();
    const QListWidgetItem *item = m_boot->currentItem();
    /* the devices set up by hand stay where they are: last, off */
    const bool movable = item && item->flags() & Qt::ItemIsEnabled;

    m_disc->setEnabled(cdrom);
    m_eject->setEnabled(cdrom && !m_entries[i].file.isEmpty());
    m_resize->setEnabled(editable && m_entries[i].newGiB > 0);
    m_remove->setEnabled(editable);
    m_bootUp->setEnabled(movable && boot > 0);
    m_bootDown->setEnabled(movable && boot + 1 < m_boot->count() &&
                           m_boot->item(boot + 1)->flags() & Qt::ItemIsEnabled);
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
    Widgets::resizeToWidth(&dialog, 560);

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
        e.id = m_nextId++;
        m_entries << e;
        addBootItem(e);
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
    e.id = m_nextId++;
    m_entries << e;
    addBootItem(e);
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
        /* a VM whose system is not set: the disc tells it, as in the New VM dialog */
        if (!m_systemSet) {
            const GuestOs::Detection d = GuestOs::detect(iso);
            const QString family =
                GuestOs::guestFamily(GuestOs::Catalogue::instance().find(d.id));
            const bool fits = !d.id.isEmpty() && (m_family.isEmpty() || m_family == family);
            m_toldDisc = fits ? iso : QString();
            m_toldSystem = fits ? d.id : QString();
            m_toldDesktop = fits ? d.desktop : QString();
        }
        fill();
    }
}

QString StoragePage::toldSystem() const
{
    if (m_systemSet || m_toldSystem.isEmpty()) {
        return {};
    }
    for (const Entry &e : m_entries) {
        if (!e.removed && e.disk.cdrom && e.file == m_toldDisc) {
            return m_toldSystem;
        }
    }
    return {};
}

void StoragePage::updateSystem()
{
    const QString system = toldSystem();

    m_system->setText(tr("The disc is %1: the VM's system, not set yet, is set to it when you "
                         "apply.")
                          .arg(Systems::name({{}, {}, system}).toHtmlEscaped()));
    m_system->setVisible(!system.isEmpty());
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
    m_edit->setIcon(Icons::themed({"document-edit"}, QStyle::SP_FileDialogDetailedView));
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
    /* a new one read-only: a guest that can write to a host folder can
       change what the host runs from it, which the user allows by choice */
    VmConfig::Share fresh;
    fresh.readonly = true;
    ShareDialog dialog(adding ? fresh : m_shares[row], others, this);
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
    Widgets::resizeToWidth(this, 560);
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
                              "while the VM runs. The USB Devices menu gives devices to the "
                              "VM while it runs too.")));
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
    /* it shows an address as written in vm.args */
    m_sshInfo->setTextFormat(Qt::PlainText);
    sshRow->addWidget(m_ssh);
    sshRow->addWidget(m_port);
    sshRow->addStretch();

    form->addRow(QString(), m_nat);
    form->addRow(QString(), Widgets::hint(tr("Through this computer's connection (NAT): the VM "
                                             "reaches the network and the internet; other "
                                             "computers reach the VM only through the ports "
                                             "forwarded to it.")));
    form->addRow(QString(), sshRow);
    form->addRow(QString(), m_sshInfo);
    layout->addWidget(m_custom);
    layout->addLayout(form);
    layout->addStretch();

    connect(m_nat, &QCheckBox::toggled, this, &NetworkPage::update);
    connect(m_ssh, &QCheckBox::toggled, this, [this](bool on) {
        if (on && m_loaded.sshPort == 0 && m_port->value() == m_port->minimum()) {
            /* a port no other VM forwards, whatever for, nor this one's
               other forwards, which stay; and free now */
            QList<int> taken = m_otherForwards;
            if (auto *store = m_vm ? qobject_cast<VmStore *>(m_vm->parent()) : nullptr) {
                for (const Vm *other : store->vms()) {
                    if (other != m_vm) {
                        taken << VmConfig::forwardedPorts(other->args());
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

    /* where the forward listens: as written, unless saving writes it anew, on 127.0.0.1 */
    const bool kept = m_loaded.sshPort > 0 && m_port->value() == m_loaded.sshPort;
    const QString address = kept ? m_loaded.sshAddress : QString("127.0.0.1");
    const QString host = address.section('%', 0, 0);
    const QString port = QString::number(m_port->value());
    QString text;

    if (VmConfig::isLoopback(address)) {
        text = tr("From this computer only: ssh -p %1 USER@%2").arg(port, host);
    } else if (host.isEmpty() || host == "0.0.0.0" || host == "::" || host == "[::]") {
        text = address.contains('%')
                   ? tr("From other computers, through %2: ssh -p %1 USER@ADDRESS, with an "
                        "address of this computer there.")
                         .arg(port, address.section('%', 1))
                   : tr("From this computer, ssh -p %1 USER@127.0.0.1, and from other "
                        "computers too: it listens on all the addresses of this computer.")
                         .arg(port);
    } else {
        text = tr("On %2 only, which other computers may reach: ssh -p %1 USER@%2")
                   .arg(port, host);
    }
    m_sshInfo->setText(text);
}

void NetworkPage::load(const ArgsFile &args)
{
    const QSignalBlocker a(m_nat), b(m_ssh), c(m_port);

    m_loaded = VmConfig::network(args);
    m_otherForwards = VmConfig::forwardedPorts(args);
    m_otherForwards.removeOne(m_loaded.sshPort);
    /* where the VM had none: passt, if at hand in its QEMU, vitrine's unless chosen */
    const QString own = VmConfig::qemuBinary(args);
    const QString chosen = own.isEmpty() ? Paths::customQemuBinary() : own;
    const QemuDocs *docs = QemuDocs::forArgs(args);
    m_backend = VmTemplate::hasPasst(docs->binary() == chosen ? docs->info() : nullptr, chosen)
                    ? "passt" : "user";
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
      m_bootMenu(new QCheckBox(tr("Show the boot men&u when the VM starts")))
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *resetRow = new QHBoxLayout;

    m_firmware->setObjectName("firmware");
    m_bootMenu->setObjectName("bootMenu");
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
    /* one place for it: with the devices */
    form->addRow(tr("Start from:"), Widgets::note(tr("The devices checked in the boot order "
                                                     "of the Storage page, in that order.")));
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
}

bool BootPage::isModified() const
{
    return m_firmware->currentData().toInt() != int(m_loadedFirmware) ||
           m_bootMenu->isChecked() != m_loadedBootMenu;
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
