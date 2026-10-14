// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

#include <optional>

class QKeyEvent;

/*
 * Which view of a VM the console shows, for a VM whose nested desktop
 * shows in vitrine's window (NestedDesktop::Mode::Integrated):
 *
 *  - Boot: QEMU's console 0, the VGA card's screen - the firmware, the boot
 *    loader, the kernel's first messages;
 *  - Screen: console 1, the virtio-gpu card's own - the kernel's console,
 *    the login screen, a recovery shell;
 *  - Desktop: the guest's KDE session (its nested KWin's window).
 *
 * It goes by itself: Boot from power-on; Screen once the guest's system
 * runs (an agent of the guest's came up: its login screen is next);
 * Desktop once the guest's window comes (the nested KWin connected); back
 * to Screen when it goes (the session ended: the login screen is there),
 * and Boot on a reboot.  Not by the card's screen: the firmware drives it
 * too (OVMF sets a mode there, shown black under rutabaga), and a reboot
 * changes nothing QEMU says of it.  The user's choice - the console's
 * switcher, the View menu, Ctrl+Alt+1/2/3 - holds until the next of those.
 */
class ConsoleViews : public QObject
{
    Q_OBJECT

public:
    enum class View { Boot, Screen, Desktop };
    Q_ENUM(View)

    explicit ConsoleViews(QObject *parent = nullptr);

    View view() const;
    /* The QEMU console of @view, for the D-Bus display: 0 for Boot, else 1 */
    static int qemuConsole(View view);
    /*
     * Ctrl+Alt and the key of 1, 2 or 3 above the letters, or on the keypad:
     * its view.  By the key's place, not its character, which the layout
     * gives (AZERTY's are &, é, "): XKB's keycode, as Wayland and X11 give
     * it, else Qt's key when the platform gives none.
     */
    static std::optional<View> viewForKey(const QKeyEvent *event);

    /* The user's choice */
    void choose(View view);
    /* A run starts, or is found running: Boot */
    void powerOn();
    /* The guest reboots: Boot */
    void reset();
    /* The guest's system runs, once per boot: Screen, from Boot */
    void systemUp();
    /* The guest's window came (Desktop), or went (Screen) */
    void setDesktop(bool shown);
    bool hasDesktop() const;

signals:
    void viewChanged(ConsoleViews::View view);

private:
    void show(View view);

    View m_view = View::Boot;
    bool m_systemUp = false;
    bool m_desktop = false;
};
