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
 *    loader, the kernel's messages, and the guest's console while it is
 *    there (bochs-drm drives that card);
 *  - Screen: console 1, the virtio-gpu card's own - the login screen, and
 *    the guest's console when it is there;
 *  - Desktop: the guest's KDE session (its nested KWin's window).
 *
 * It follows what the guest shows: Boot from power-on and at a reboot;
 * Screen once the guest says its own screen shows its login screen or its
 * console (the guest tools' agent: GuestToolsMonitor::screenShown());
 * Desktop while the guest's window is there (the nested KWin connected).
 * When the window goes: Screen if the guest's screen shows something, Boot
 * if its agent went (the guest shuts down or restarts), else nothing moves
 * until the guest tells (a logout's login screen comes, or the agent goes).
 *
 * Not as an agent answers: agents start early in a boot, seconds or minutes
 * (the guest tools' install at boot) before the login screen.  Nor by what
 * QEMU says of the card's screen: the firmware drives it too (OVMF sets a
 * mode there), QEMU keeps the size of a screen that went, and a display
 * client hears only of the console it shows.  The user's choice - the
 * Console button's menu, the View menu, Ctrl+Alt+1/2/3 - holds until the
 * next of those changes.
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
    /*
     * The guest reboots: Boot.  Its session, its agent and its screen went
     * with it: what is said of them late - their window going after the
     * reset - moves nothing.
     */
    void reset();
    /*
     * What the guest says through the guest tools' agent: @up, the agent
     * answers (its system runs; the agent goes as the guest shuts down or
     * restarts, and at the end of the tools' install at boot); @screen, the
     * guest's own screen shows its login screen or its console.  Screen as
     * that comes, Boot as the agent goes; nothing while the desktop shows.
     */
    void setGuest(bool up, bool screen);
    /* The guest's window came (Desktop), or went (see above) */
    void setDesktop(bool shown);
    bool hasDesktop() const;

signals:
    void viewChanged(ConsoleViews::View view);

private:
    void show(View view);

    View m_view = View::Boot;
    bool m_up = false;
    bool m_screen = false;
    bool m_desktop = false;
};
