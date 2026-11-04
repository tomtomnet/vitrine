// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/settingspage.h"

#include <QList>
#include <QMap>
#include <QStringList>

#include <optional>

#include "core/hostgpus.h"
#include "core/nesteddesktop.h"

/*
 * Where the screen shows (vitrine's window, QEMU's GTK or SDL window, or
 * none), the nested desktop and the host GPU it renders on, what QEMU's
 * window does, and the graphics card with vitrine's settings for its 3D:
 * the Qt app's DisplayPage, through the same core calls.  Its lists hold
 * what can be chosen, and what the VM has now; the notes under them say
 * what the VM's QEMU lacks.
 */
class DisplayPage : public SettingsPage
{
public:
    DisplayPage();

    QString title() const override { return "Display"; }
    const char *icon() const override { return "video-display-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

    /* For tests: the lists' entries by what they write, and the choices */
    QStringList displays() const { return m_displayKeys; }
    QStringList cards() const { return m_cardDrivers; }
    void chooseDisplay(const QString &display);
    void chooseCard(const QString &driver);
    void setNested(bool on);

private:
    /* What the page shows; save() writes what differs from what load() showed */
    struct Shown {
        /* -display of the screen chosen: dbus, gtk, sdl, none; empty when none is */
        QString screen;
        /* the options of QEMU's window that it has, e.g. show-cursor */
        QMap<QString, bool> options;
        /* the nested desktop, whose card takes the card's place */
        bool nested = false;
        /* the host GPU it renders on, its card's x-render-node: empty for Automatic */
        QString gpu;
        /* the card chosen: a driver of the list, or the VM's own as it is */
        QString card;
        bool nativeContext = false;
        bool venus = false;
        bool frameTiming = false;

        bool operator==(const Shown &other) const = default;
    };
    Shown shown() const;
    /* What can be chosen, and what the notes say, for the choices of now */
    void update();
    /* The 3D card chosen in the list, or empty */
    QString chosen3dCard() const;
    QString chosenCard() const;
    QString chosenDisplay() const;
    QString chosenGpu() const;
    /* The nested desktop's card in the list, or the card it replaced back */
    void nestedToggled(bool on);
    /* The card list: the choices, and the VM's own, @driver chosen */
    void fillCards(const QString &driver, const QString &ownText = {});
    /* The display list: those the VM's QEMU has, and @display chosen */
    void fillDisplays(const QString &display);
    /* The GPUs of the list, and the VM's choice @chosen (a path) current */
    void loadGpus(const QString &chosen);
    void fillGpus();
    QString gpuText(const HostGpu &gpu) const;
    QString gpuHint() const;
    /* The VM's QEMU has no x-render-node: Automatic only */
    bool lacksGpuChoice() const;
    static void setCaption(GtkWidget *caption, const QString &text);

    AdwBanner *m_custom;
    GtkWidget *m_display;
    QStringList m_displayKeys;
    /* what the list shows for a screen none of its entries: what the VM has, in short */
    QString m_placeholder;
    GtkWidget *m_displayHint;
    GtkWidget *m_displayLacks;
    GtkWidget *m_nested;
    /* what the VM's QEMU has of it, asked once per load() */
    NestedDesktop::Offer m_nestedOffer;
    /* the card of the list before the nested desktop's took its place, and its native context */
    QString m_cardBeforeNested;
    bool m_nativeBeforeNested = false;
    GtkWidget *m_gpu;
    GtkWidget *m_gpuHint;
    /* every GPU entry, what it writes, and whether it can be chosen */
    struct GpuEntry {
        QString text;
        QString path;
        bool choosable = false;
    };
    QList<GpuEntry> m_gpuEntries;
    /* the entries listed now, by their place in m_gpuEntries */
    QList<int> m_gpuShown;
    int m_gpuChosen = 0;
    QList<HostGpu> m_gpus;
    std::optional<HostGpu> m_desktopGpu;
    AdwPreferencesGroup *m_windowGroup;
    QMap<QString, GtkWidget *> m_options;
    GtkWidget *m_card;
    QStringList m_cardDrivers;
    GtkWidget *m_cardHint;
    GtkWidget *m_nativeContext;
    GtkWidget *m_venus;
    GtkWidget *m_frameTiming;
    /* The arguments load() read: what their QEMU offers depends on the card */
    ArgsFile m_args;
    Shown m_loaded;
    /* while the lists are made again: no choice of the user's */
    bool m_filling = false;
};
