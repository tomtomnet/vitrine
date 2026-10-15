// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

#include <optional>

class QKeyEvent;

/*
 * Which view of a VM the console shows, for a VM whose nested desktop
 * shows in vitrine's window (NestedDesktop::Mode::Integrated):
 *
 *  - Screen: the VM's screen, QEMU's console of the nested desktop's one
 *    card (NestedDesktop::screenConsole()) - the firmware and the boot
 *    loader on its VGA part, then the guest's own: the boot's and the
 *    shutdown's messages, its login screen;
 *  - Desktop: the guest's KDE session (its nested KWin's window).
 *
 * It follows the guest's window: Screen from power-on and at a reboot,
 * Desktop while the window is there (the nested KWin connected), Screen
 * again when it goes - a logout's login screen, a shutdown's messages.
 * The user's choice - the Console button's menu, the View menu,
 * Ctrl+Alt+1/2 - holds until the window comes or goes, or the guest
 * restarts.
 *
 * With the two cards of vitrine before 2026-10-15 there were three views:
 * Boot (QEMU's VGA) and Screen (the virtio card) apart, which only the
 * guest's agent could tell between.  One card shows both on one screen.
 */
class ConsoleViews : public QObject
{
    Q_OBJECT

public:
    enum class View { Screen, Desktop };
    Q_ENUM(View)

    explicit ConsoleViews(QObject *parent = nullptr);

    View view() const;
    /*
     * Ctrl+Alt and the key of 1 or 2 above the letters, or on the keypad:
     * its view.  By the key's place, not its character, which the layout
     * gives (AZERTY's are & and é): XKB's keycode, as Wayland and X11 give
     * it, else Qt's key when the platform gives none.
     */
    static std::optional<View> viewForKey(const QKeyEvent *event);

    /* The user's choice */
    void choose(View view);
    /*
     * A run starts, is found running, or the guest restarts (the console
     * then serves its nested desktop anew): Screen, the window forgotten
     */
    void powerOn();
    /* The guest's window came (Desktop), or went (Screen) */
    void setDesktop(bool shown);
    bool hasDesktop() const;

signals:
    void viewChanged(ConsoleViews::View view);

private:
    void show(View view);

    View m_view = View::Screen;
    bool m_desktop = false;
};
