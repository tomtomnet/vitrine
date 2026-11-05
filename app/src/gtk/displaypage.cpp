// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/displaypage.h"

#include <QHash>

#include <algorithm>

#include "core/cardsettings.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"

using Gtk4::utf8;

/* Where the screen shows, by -display, in the order of the list: vitrine's window first, the
   one new VMs have */
static const char *const kDisplays[][2] = {
    {"dbus", "Vitrine's Window (D-Bus)"},
    {"gtk", "QEMU's GTK Window"},
    {"sdl", "QEMU's SDL Window"},
    {"none", "None"},
};

/* A display of the list as the notes name it */
static QString displayName(const QString &display)
{
    return display == "dbus" ? QString("D-Bus") : display.toUpper();
}

/* The options of QEMU's windows the page has, each window's, and QEMU's defaults */
static const char *const kWindowOptions[][2] = {
    {"show-cursor", "Show the Mouse Pointer"},
    {"zoom-to-fit", "Zoom the Screen to Fit the Window"},
    {"show-menubar", "Menu Bar"},
    {"grab-on-hover", "Take the Keyboard When the Pointer Is Over the Window"},
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
        return {{"virtio-gpu-gl-pci", "3D (virtio-gpu-gl-pci)"},
                {"virtio-gpu-pci", "2D (virtio-gpu-pci)"}};
    }
    return {{"virtio-vga-gl", "3D, with VGA (virtio-vga-gl)"},
            {"virtio-gpu-gl-pci", "3D, without VGA (virtio-gpu-gl-pci)"},
            {"virtio-vga", "2D, with VGA (virtio-vga)"},
            {"VGA", "Standard VGA (VGA)"}};
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

/*
 * The nested desktop's card, which takes the card's place in the list while
 * it is on; the list's entry for the two cards of before too
 * (virtio-gpu-rutabaga-pci beside QEMU's VGA), which a VM has until its next
 * start
 */
static const char kNestedCard[] = "virtio-vga-rutabaga";
static const char kNestedCardText[] = "Nested desktop's, with VGA (virtio-vga-rutabaga)";

DisplayPage::DisplayPage()
    : SettingsPage(adw_preferences_page_new()), m_custom(ADW_BANNER(adw_banner_new("")))
{
    auto *page = ADW_PREFERENCES_PAGE(widget());
    AdwPreferencesGroup *screen = Gtk4::group("Screen");
    AdwPreferencesGroup *card = Gtk4::group("Graphics Card");
    AdwPreferencesGroup *threeD = Gtk4::group("3D");

    adw_preferences_page_set_banner(page, m_custom);

    m_display = Gtk4::comboRow("Display", {});
    gtk_widget_set_name(m_display, "display");
    m_displayHint = Gtk4::caption({});
    m_displayLacks = Gtk4::caption({});
    m_nested = Gtk4::switchRow("Nested Desktop");
    gtk_widget_set_name(m_nested, "nested");
    m_gpu = Gtk4::comboRow("GPU", {});
    gtk_widget_set_name(m_gpu, "gpu");
    m_gpuHint = Gtk4::caption({});
    adw_preferences_group_add(screen, m_display);
    adw_preferences_group_add(screen, m_nested);
    adw_preferences_group_add(screen, m_gpu);
    for (GtkWidget *caption : {m_displayHint, m_displayLacks, m_gpuHint}) {
        gtk_widget_set_margin_top(caption, 6);
        adw_preferences_group_add(screen, caption);
    }
    adw_preferences_page_add(page, screen);

    /* QEMU's window: what it has, for the window chosen */
    m_windowGroup = Gtk4::group("QEMU's Window");
    for (const auto &[key, text] : kWindowOptions) {
        GtkWidget *row = Gtk4::switchRow(text);
        gtk_widget_set_name(row, key);
        adw_preferences_group_add(m_windowGroup, row);
        m_options.insert(key, row);
        on<void(GParamSpec *)>(row, "notify::active", [this](GParamSpec *) { changed(); });
    }
    adw_preferences_page_add(page, m_windowGroup);

    m_card = Gtk4::comboRow("Card", {});
    gtk_widget_set_name(m_card, "card");
    m_cardHint = Gtk4::caption({});
    gtk_widget_set_margin_top(m_cardHint, 6);
    adw_preferences_group_add(card, m_card);
    adw_preferences_group_add(card, m_cardHint);
    adw_preferences_page_add(page, card);

    m_nativeContext = Gtk4::switchRow("DRM Native Context");
    gtk_widget_set_name(m_nativeContext, "nativeContext");
    m_venus = Gtk4::switchRow("Venus");
    gtk_widget_set_name(m_venus, "venus");
    m_frameTiming = Gtk4::switchRow("Frames in Step With This Computer's Screen");
    gtk_widget_set_name(m_frameTiming, "frameTiming");
    for (GtkWidget *row : {m_nativeContext, m_venus, m_frameTiming}) {
        adw_preferences_group_add(threeD, row);
        on<void(GParamSpec *)>(row, "notify::active", [this](GParamSpec *) { changed(); });
    }
    adw_preferences_page_add(page, threeD);

    for (GtkWidget *combo : {m_display, m_card}) {
        on<void(GParamSpec *)>(combo, "notify::selected", [this](GParamSpec *) {
            if (!m_filling && !m_loading) {
                update();
                changed();
            }
        });
    }
    on<void(GParamSpec *)>(m_gpu, "notify::selected", [this](GParamSpec *) {
        if (m_filling || m_loading) {
            return;
        }
        m_gpuChosen = m_gpuShown.value(Gtk4::selected(m_gpu), 0);
        update();
        changed();
    });
    on<void(GParamSpec *)>(m_nested, "notify::active", [this](GParamSpec *) {
        if (!m_loading) {
            nestedToggled(adw_switch_row_get_active(ADW_SWITCH_ROW(m_nested)));
            changed();
        }
    });
}

void DisplayPage::setCaption(GtkWidget *caption, const QString &text)
{
    gtk_label_set_text(GTK_LABEL(caption), utf8(text).constData());
    gtk_widget_set_visible(caption, !text.isEmpty());
}

QString DisplayPage::chosenCard() const
{
    return m_cardDrivers.value(Gtk4::selected(m_card));
}

QString DisplayPage::chosenDisplay() const
{
    return m_displayKeys.value(Gtk4::selected(m_display));
}

QString DisplayPage::chosenGpu() const
{
    return m_gpuEntries.value(m_gpuChosen).path;
}

QString DisplayPage::chosen3dCard() const
{
    const QString card = chosenCard();
    return gtk_widget_get_sensitive(m_card) && VmConfig::isAccelerated(card) ? card : QString();
}

DisplayPage::Shown DisplayPage::shown() const
{
    Shown s;

    s.screen = chosenDisplay();
    for (const QString &key : kWindowKeys.value(s.screen)) {
        s.options.insert(key, adw_switch_row_get_active(ADW_SWITCH_ROW(m_options.value(key))));
    }
    s.card = chosenCard();
    s.nested = adw_switch_row_get_active(ADW_SWITCH_ROW(m_nested));
    s.gpu = chosenGpu();
    /* vitrine's settings of a 3D card, of those its QEMU has */
    if (!chosen3dCard().isEmpty()) {
        const auto on = [](GtkWidget *row) {
            return gtk_widget_get_sensitive(row) && adw_switch_row_get_active(ADW_SWITCH_ROW(row));
        };
        s.nativeContext = on(m_nativeContext);
        s.venus = on(m_venus);
        s.frameTiming = on(m_frameTiming);
    }
    return s;
}

void DisplayPage::fillDisplays(const QString &display)
{
    const QString card = chosen3dCard();
    const std::optional<CardSettings::Offers> offers = CardSettings::offers(m_args, card);
    QStringList names;

    m_filling = true;
    m_displayKeys.clear();
    for (const auto &[key, text] : kDisplays) {
        /* what the VM's QEMU lacks, unless the VM has it now */
        const bool lacked = QString(key) != "none" && offers && offers->displays &&
                            !offers->displays->contains(key);
        if (!lacked || display == key) {
            m_displayKeys << key;
            names << text;
        }
    }
    /* a screen none of the entries is: what the VM has, as the first entry */
    if (!m_placeholder.isEmpty()) {
        m_displayKeys.prepend(QString());
        names.prepend(m_placeholder);
    }
    Gtk4::setItems(m_display, names);
    Gtk4::setSelected(m_display, int(qMax<qsizetype>(0, m_displayKeys.indexOf(display))));
    m_filling = false;
}

void DisplayPage::fillCards(const QString &driver, const QString &ownText)
{
    QStringList names;

    m_filling = true;
    m_cardDrivers.clear();
    for (const auto &[choice, text] : cardChoices(m_args)) {
        m_cardDrivers << choice;
        names << text;
    }
    if (!m_cardDrivers.contains(driver)) {
        m_cardDrivers << driver;
        names << (ownText.isEmpty() ? driver : ownText);
    }
    Gtk4::setItems(m_card, names);
    Gtk4::setSelected(m_card, int(m_cardDrivers.indexOf(driver)));
    m_filling = false;
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
        GtkWidget *row;
        CardSettings::Feature feature;
        QString text;
    };
    const Feature features[] = {
        {m_nativeContext, CardSettings::Feature::NativeContext,
         "The guest's 3D goes through this computer's GPU driver, with the Mesa the guest tools "
         "install."},
        {m_venus, CardSettings::Feature::Venus, "Vulkan for a GPU without native context."},
        {m_frameTiming, CardSettings::Feature::FrameTiming,
         "Smooth frames in Vitrine's window and the SDL one, with the guest tools' driver."},
    };

    /* the displays its QEMU has, and the one it uses; none for -nographic */
    const bool nographic = m_args.indexOf("nographic") >= 0;
    QStringList lacking;
    for (const auto &[key, text] : kDisplays) {
        if (QString(key) != "none" && lacks(key)) {
            lacking << displayName(key);
        }
    }
    fillDisplays(s.screen);
    gtk_widget_set_sensitive(m_display, !nographic);
    QString hint;
    if (s.nested) {
        hint = s.screen == "dbus"
                   ? QString("Vitrine's window shows the guest's desktop beside the VM's screen, "
                             "which shows the boot and the login screen. Quitting Vitrine ends the "
                             "guest's session: the guest goes back to its login screen.")
               : s.screen == "gtk" || s.screen == "sdl"
                   ? QString("The guest's desktop opens as a window of its own; QEMU's %1 window "
                             "shows the boot and the login screen.")
                         .arg(displayName(s.screen))
               : s.screen == "none" ? QString("The guest's desktop opens as a window of its own, "
                                              "with nothing for the boot and the login screen.")
                                    : QString();
    } else {
        hint = s.screen == "dbus" ? QString("The screen is part of Vitrine's window, and can go "
                                            "full screen.")
               : s.screen == "gtk" ? QString("QEMU shows the screen in a window of its own with "
                                             "its menus, which stays open when Vitrine closes; the "
                                             "frames there do not keep step with this computer's "
                                             "screen.")
               : s.screen == "sdl" ? QString("QEMU shows the screen in a window of its own, which "
                                             "stays open when Vitrine closes.")
               : s.screen == "none" ? QString("The VM runs without a screen.")
                                    : QString();
    }
    setCaption(m_displayHint, hint);

    /* the nested desktop: with a QEMU that has its card, for a card not set up by hand */
    const VmConfig::Graphics loaded = VmConfig::graphics(m_args);
    const bool hadNested = loaded.kind == VmConfig::Graphics::Nested;
    const bool byHand = loaded.kind == VmConfig::Graphics::Custom;
    gtk_widget_set_sensitive(m_nested,
                             !nographic && !byHand && (m_nestedOffer.offered || hadNested));
    Gtk4::setSubtitle(
        m_nested,
        byHand || nographic ? QString()
        : s.nested          ? QString("For Fedora KDE guests with the guest tools: a login at their "
                                      "login screen starts the nested desktop.")
        : m_nestedOffer.offered
            ? QString("The guest's KDE session through a KWin of its own, as a window of this "
                      "computer's desktop or in Vitrine's (virtio-gpu-rutabaga).")
        : m_nestedOffer.answered
            ? QString("This VM's QEMU has no virtio-gpu-rutabaga with cross-domain.")
            : QString("This VM's QEMU did not say whether it has virtio-gpu-rutabaga."));

    /*
     * the host GPU it renders on: with several, or with a choice made; with
     * a QEMU that cannot choose, Automatic alone, back from a choice made
     */
    const bool chose = !NestedDesktop::renderNode(m_args).isEmpty();
    const bool gpuShown = s.nested && (m_gpus.size() > 1 || chose);
    fillGpus();
    gtk_widget_set_visible(m_gpu, gpuShown);
    gtk_widget_set_sensitive(m_gpu, !lacksGpuChoice() || m_gpuChosen > 0);
    setCaption(m_gpuHint, gpuShown ? gpuHint() : QString());

    setCaption(m_displayLacks,
               lacking.isEmpty() || nographic ? QString()
               : lacking.size() == 1
                   ? QString("This VM's QEMU has no %1 display.").arg(lacking.first())
                   : QString("This VM's QEMU has none of these displays: %1.").arg(lacking.join(", ")));

    /* each window its options */
    const QStringList keys = kWindowKeys.value(s.screen);
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it) {
        gtk_widget_set_visible(it.value(), keys.contains(it.key()));
    }
    gtk_widget_set_visible(GTK_WIDGET(m_windowGroup), !keys.isEmpty());

    /* the card */
    const QString driver = chosenCard();
    QString cardHint;
    if (s.nested && hadNested && loaded.device != kNestedCard) {
        cardHint = "The nested desktop's card, virtio-gpu-rutabaga with cross-domain. This VM still "
                   "has it beside the standard VGA, as Vitrine set it up before: one card with VGA "
                   "takes their place at its next start.";
    } else if (s.nested) {
        cardHint = "The nested desktop's card, virtio-gpu-rutabaga with cross-domain. It shows the "
                   "firmware, the boot and the login screen too.";
    } else if (!gtk_widget_get_sensitive(m_card)) {
        cardHint = "The graphics of this VM are set up by hand: change them on the Arguments page.";
    } else if (VmConfig::machineType(m_args).startsWith("virt")) {
        cardHint.clear();
    } else if (VmConfig::isVgaDevice(driver)) {
        cardHint = "It shows the firmware, the boot loader and the first boot messages too.";
    } else if (driver.startsWith("virtio-gpu")) {
        cardHint = "Nothing shows before the guest's driver starts, but UEFI's own screens.";
    }
    setCaption(m_cardHint, cardHint);

    /* its 3D, with what the VM's QEMU has */
    for (const Feature &f : features) {
        const bool offered = offers && CardSettings::isOffered(*offers, f.feature);
        gtk_widget_set_sensitive(f.row, !card.isEmpty() && offered);
        Gtk4::setSubtitle(f.row,
                          s.nested ? (f.feature == CardSettings::Feature::NativeContext
                                          ? QString("The nested desktop's card has it, through "
                                                    "rutabaga.")
                                          : QString("Not with the nested desktop's card."))
                          : card.isEmpty() ? f.text
                          : !offers        ? QString("This VM's QEMU did not say what it has.")
                          : offered        ? f.text
                          : !offers->card  ? QString("Vitrine's QEMU is built without it.")
                                           : QString("This VM's QEMU lacks it."));
    }
}

void DisplayPage::nestedToggled(bool on)
{
    const QString current = chosenCard();

    if (on) {
        if (current != kNestedCard) {
            m_cardBeforeNested = current;
            m_nativeBeforeNested = adw_switch_row_get_active(ADW_SWITCH_ROW(m_nativeContext));
        }
        /* the nested desktop's card has it, through rutabaga */
        adw_switch_row_set_active(ADW_SWITCH_ROW(m_nativeContext), true);
        fillCards(kNestedCard, kNestedCardText);
        gtk_widget_set_sensitive(m_card, false);
    } else {
        /* the card it replaced, else the 3D card of new VMs, with their 3D */
        QString back = m_cardBeforeNested;
        const QList<std::pair<QString, QString>> choices = cardChoices(m_args);
        const bool listed = std::any_of(choices.cbegin(), choices.cend(),
                                        [&back](const auto &c) { return c.first == back; });
        if (back.isEmpty() || back == kNestedCard || !listed) {
            back = choices.first().first;
            adw_switch_row_set_active(ADW_SWITCH_ROW(m_nativeContext), true);
            adw_switch_row_set_active(ADW_SWITCH_ROW(m_frameTiming), true);
        } else {
            adw_switch_row_set_active(ADW_SWITCH_ROW(m_nativeContext), m_nativeBeforeNested);
        }
        fillCards(back);
        gtk_widget_set_sensitive(m_card, true);
    }
    update();
}

bool DisplayPage::lacksGpuChoice() const
{
    return m_nestedOffer.properties && !m_nestedOffer.properties->contains("x-render-node");
}

QString DisplayPage::gpuText(const HostGpu &gpu) const
{
    QString text = gpu.name();

    /* two of a model, told apart by their place */
    if (std::count_if(m_gpus.cbegin(), m_gpus.cend(),
                      [&gpu](const HostGpu &g) { return g.name() == gpu.name(); }) > 1) {
        text += QString(" (%1)").arg(gpu.shortAddress());
    }
    if (m_desktopGpu && m_desktopGpu->address == gpu.address) {
        text = QString("%1, the desktop's").arg(text);
    }
    return text;
}

void DisplayPage::loadGpus(const QString &chosen)
{
    /* what Automatic renders on: the desktop's, else the renderer's own choice */
    std::optional<HostGpu> automatic = HostGpus::automatic(m_gpus);
    if (!automatic) {
        automatic = HostGpus::rendererChoice(m_gpus);
    }

    m_gpuEntries.clear();
    m_gpuEntries << GpuEntry{automatic ? QString("Automatic: %1").arg(gpuText(*automatic))
                                       : QString("Automatic"),
                             QString(), true};
    for (const HostGpu &gpu : m_gpus) {
        m_gpuEntries << GpuEntry{gpu.hasNativeContext()
                                     ? gpuText(gpu)
                                     : QString("%1, without native context").arg(gpuText(gpu)),
                                 gpu.path(), gpu.hasNativeContext()};
    }
    m_gpuChosen = 0;
    /* the VM's: by its path, or any other to the same node */
    if (!chosen.isEmpty()) {
        const std::optional<HostGpu> gpu = HostGpus::find(m_gpus, chosen);
        m_gpuChosen = -1;
        for (int i = 0; gpu && i < m_gpuEntries.size(); i++) {
            if (m_gpuEntries[i].path == gpu->path()) {
                m_gpuChosen = i;
            }
        }
        if (m_gpuChosen < 0) {
            const bool gone = HostGpus::pathState(chosen) == HostGpus::PathState::Missing;
            m_gpuEntries << GpuEntry{gone ? QString("Not on this computer (%1)")
                                                .arg(HostGpus::describe(chosen))
                                          : QString("Not a GPU (%1)").arg(chosen),
                                     chosen, false};
            m_gpuChosen = int(m_gpuEntries.size()) - 1;
        }
    }
    fillGpus();
}

/* Automatic, the GPUs that can be chosen, and the VM's choice */
void DisplayPage::fillGpus()
{
    QStringList names;

    m_filling = true;
    m_gpuShown.clear();
    for (int i = 0; i < m_gpuEntries.size(); i++) {
        if (i == 0 || i == m_gpuChosen || (!lacksGpuChoice() && m_gpuEntries[i].choosable)) {
            m_gpuShown << i;
            names << m_gpuEntries[i].text;
        }
    }
    Gtk4::setItems(m_gpu, names);
    Gtk4::setSelected(m_gpu, int(qMax<qsizetype>(0, m_gpuShown.indexOf(m_gpuChosen))));
    m_filling = false;
}

QString DisplayPage::gpuHint() const
{
    const QString chosen = chosenGpu();

    if (lacksGpuChoice()) {
        return "This VM's QEMU cannot choose the GPU: an update of Vitrine's QEMU brings it.";
    }
    if (chosen.isEmpty()) {
        /* what Automatic takes: the desktop's, else the renderer's own choice */
        std::optional<HostGpu> gpu = HostGpus::automatic(m_gpus);
        if (!gpu) {
            gpu = HostGpus::rendererChoice(m_gpus);
        }
        if (!gpu || !m_desktopGpu || gpu->address == m_desktopGpu->address) {
            return {};
        }
        return QString("This computer's desktop runs on %1, which has no native context in "
                       "Vitrine's QEMU: the guest's frames cross from one GPU to the other, which "
                       "is slower.")
            .arg(m_desktopGpu->name());
    }
    const std::optional<HostGpu> gpu = HostGpus::find(m_gpus, chosen);
    if (!gpu) {
        return HostGpus::pathState(chosen) == HostGpus::PathState::Missing
                   ? QString("This GPU is not on this computer: the VM does not start until "
                             "another GPU, or Automatic, is chosen.")
                   : QString("This is not a GPU: the VM does not start until a GPU, or Automatic, "
                             "is chosen.");
    }
    if (!gpu->hasNativeContext()) {
        return QString("Vitrine's QEMU has no native context for this GPU's driver (%1): the VM "
                       "does not start on it.")
            .arg(gpu->driver.isEmpty() ? QString("none") : gpu->driver);
    }
    if (m_desktopGpu && gpu->address != m_desktopGpu->address) {
        return QString("This computer's desktop runs on %1: the guest's frames cross from one GPU "
                       "to the other, which is slower, and the guest may show nothing if the two "
                       "cannot share buffers.")
            .arg(m_desktopGpu->name());
    }
    return {};
}

void DisplayPage::load(const ArgsFile &args)
{
    const Loading loading(m_loading);
    const VmConfig::Screen screen = VmConfig::screen(args);
    const VmConfig::Graphics g = VmConfig::graphics(args);
    const QString display = g.display;
    QString choice;
    QString custom;
    QString placeholder;

    m_args = args;
    m_nestedOffer = NestedDesktop::offer(args);
    m_gpus = HostGpus::list();
    m_desktopGpu = HostGpus::desktop(m_gpus);
    loadGpus(NestedDesktop::renderNode(args));
    /* the screen: no choice for one shown elsewhere, or in another window */
    if (screen == VmConfig::Screen::Embedded) {
        choice = "dbus";
    } else if (screen == VmConfig::Screen::OwnWindow && (display == "sdl" || display == "gtk")) {
        choice = display;
    } else if (screen == VmConfig::Screen::None && !VmConfig::hasRemoteDisplay(args) &&
               args.indexOf("nographic") < 0 && (display == "none" || display == "egl-headless")) {
        choice = "none";
    }

    if (args.indexOf("nographic") >= 0) {
        custom = "This VM has no screen (-nographic): change it on the Arguments page.";
        placeholder = "No screen (-nographic)";
    } else if (screen == VmConfig::Screen::None && choice.isEmpty() &&
               (display.isEmpty() || display == "default" || display == "none" ||
                display == "egl-headless")) {
        custom = "This VM shows its screen over VNC or SPICE only. Choosing below adds a window.";
        placeholder = "VNC or SPICE only";
    } else if (screen == VmConfig::Screen::None && choice.isEmpty()) {
        custom = QString("This VM shows its screen nowhere Vitrine can (-display %1). Choosing "
                         "below replaces it.")
                     .arg(display);
        placeholder = QString("Elsewhere (-display %1)").arg(display);
    } else if (screen == VmConfig::Screen::OwnWindow && choice.isEmpty()) {
        const bool qemuDefault = display.isEmpty() || display == "default";
        custom = qemuDefault ? QString("The screen shows in QEMU's default window.")
                             : QString("The screen shows in QEMU's %1 window.").arg(display.toUpper());
        placeholder = qemuDefault ? QString("QEMU's default window")
                                  : QString("QEMU's %1 window").arg(display.toUpper());
    }
    adw_banner_set_title(m_custom, utf8(custom.toHtmlEscaped()).constData());
    adw_banner_set_revealed(m_custom, !custom.isEmpty());
    /* the entry for what the VM has, current: one of the others would make an untouched
       page modified */
    m_placeholder = choice.isEmpty() ? placeholder : QString();

    /* the window's options, as QEMU takes them when not given */
    for (auto it = m_options.cbegin(); it != m_options.cend(); ++it) {
        adw_switch_row_set_active(ADW_SWITCH_ROW(it.value()),
                                  VmConfig::displayFlag(args, it.key(), windowOptionDefault(it.key())));
    }

    /* the card: one of the list, else the VM's own as it is */
    {
        const bool byHand = g.kind == VmConfig::Graphics::Custom;
        QString own = g.device;
        QString ownText;

        if (byHand) {
            own = QString("Set up by hand (%1)").arg(g.custom);
        } else if (g.kind == VmConfig::Graphics::Nested) {
            /* the two cards of before too: the list's one entry for it */
            own = kNestedCard;
            ownText = kNestedCardText;
        } else if (own.isEmpty()) {
            own = g.kind == VmConfig::Graphics::None     ? QString("None")
                  : g.kind == VmConfig::Graphics::Virtio ? QString("virtio-vga")
                                                         : QString("VGA");
        }
        fillCards(own, ownText);
        gtk_widget_set_sensitive(m_card, !byHand && g.kind != VmConfig::Graphics::Nested);
    }
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_nested), g.kind == VmConfig::Graphics::Nested);
    m_cardBeforeNested.clear();
    /* the nested desktop's card has it, through rutabaga */
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_nativeContext),
                              g.kind == VmConfig::Graphics::Nested ||
                                  CardSettings::isOn(args, CardSettings::Feature::NativeContext));
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_venus),
                              CardSettings::isOn(args, CardSettings::Feature::Venus));
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_frameTiming),
                              CardSettings::isOn(args, CardSettings::Feature::FrameTiming));

    fillDisplays(choice);
    /* as the page shows it, so that an untouched page writes nothing */
    m_loaded.card = chosenCard();
    update();
    m_loaded = shown();
}

void DisplayPage::save(ArgsFile &args)
{
    const Shown s = shown();

    if (s == m_loaded) {
        return;
    }
    /* the nested desktop's card in the card's place, or the card back in its place */
    if (s.nested != m_loaded.nested) {
        NestedDesktop::set(args, s.nested, m_nestedOffer.properties);
    }
    /* the GPU on its card, a new one too */
    if (s.nested && (s.gpu != m_loaded.gpu || s.nested != m_loaded.nested)) {
        NestedDesktop::setRenderNode(args, s.gpu);
    }
    const bool cardChanged = !s.nested && s.card != m_loaded.card;
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
    if (!s.screen.isEmpty() &&
        (s.screen != m_loaded.screen || (cardChanged && s.screen == "none"))) {
        VmConfig::setScreen(args,
                            s.screen == "dbus"   ? VmConfig::Screen::Embedded
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

void DisplayPage::chooseDisplay(const QString &display)
{
    Gtk4::setSelected(m_display, int(m_displayKeys.indexOf(display)));
}

void DisplayPage::chooseCard(const QString &driver)
{
    Gtk4::setSelected(m_card, int(m_cardDrivers.indexOf(driver)));
}

void DisplayPage::setNested(bool on)
{
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_nested), on);
}
