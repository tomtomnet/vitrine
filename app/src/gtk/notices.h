// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"
#include "gtk/guarded.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "core/hostsettings.h"

class QTimer;
class UdmabufWatch;
class Vm;
class VmStore;

/*
 * What the running VMs want the user to know, as the Qt app's status bar
 * shows it (HostTuningNotifier, UdmabufNotifier, MemoryMonitor): a warning
 * button in the VM page's header bar while any of it holds, its tooltip
 * naming them; its popover lists each with What to Do… or Details…, which
 * explain it in an alert with the fix.
 *
 *  - Host tuning on, and VMs running untuned (HostSettings::untuned()):
 *    why, the fix, Set Up… where the vitrine group is what it takes, Turn
 *    Off Tuning.  Once per run, when a VM starts untuned for want of the
 *    group, the offer to set it up comes by itself (Not Now the default).
 *    The state is asked again each time the window comes back to the front
 *    while VMs run untuned: the helper installed or the group joined from
 *    a terminal counts then.  Nothing of this while tuning is off.
 *  - udmabuf (UdmabufWatch): guest windows copied, the device, the limits;
 *    once per run its explanation comes by itself, with tuning on, when
 *    the limits or the device are in the way and still would be, unless
 *    the group's offer, which covers it, came up in this run.
 *  - Memory short: the running VMs could take more than the host has free.
 *
 * Not banners: a banner over the VM's screen resizes the guest's screen as
 * it comes and as it goes.  The popover is a surface of its own over it.
 */
class Notices : public Gtk4::Guarded
{
public:
    enum class Kind { HostTuning, Udmabuf, Memory };

    /* @present: the window's present(), which covers the VM's screen under an alert */
    Notices(HostSettings *host, VmStore *store, GtkWindow *window, Gtk4::Dialog::Presenter present);
    ~Notices();
    Notices(const Notices &) = delete;
    Notices &operator=(const Notices &) = delete;

    /* For the VM page's header bar: shown while a notice holds */
    GtkWidget *button() const { return m_button; }
    /* The notices that hold now, in their order */
    QList<Kind> shown() const;
    /* What the popover says of each: a title and a line */
    QString title(Kind kind) const;
    QString text(Kind kind) const;
    /* What to Do… or Details…: its explanation, in an alert */
    void explain(Kind kind);
    UdmabufWatch *udmabufWatch() const { return m_udmabuf; }
    /* The memory read again (every 3 s, and as VMs start and stop) */
    void refreshMemory();

    /*
     * The vitrine group set up (HostSettings::setUpGroup(): the desktop's
     * polkit dialog asks for an administrator's password), and how it went
     * told through @present; then @done
     */
    static void setUpGroup(const Gtk4::Dialog::Presenter &present, std::function<void()> done = {});
    /* Tuning off, now: as the Preferences' switch */
    static void turnOffTuning();

private:
    void updateHost();
    void updateUdmabuf();
    /* The button, its tooltip and its popover, after a change of the notices */
    void update();
    /* The state asked again, then explainHostNow() */
    void explainHost();
    void explainHostNow(const HostSettings::Status &status);
    void offerGroup(const HostSettings::Status &status);
    void explainUdmabuf();
    void explainMemory();
    void watch(Vm *vm);

    QObject m_qt;
    QPointer<HostSettings> m_host;
    VmStore *m_store;
    Gtk4::Dialog::Presenter m_present;
    UdmabufWatch *m_udmabuf;
    QTimer *m_memoryTimer;
    /* what each says now; empty while it does not hold */
    QString m_hostText;
    QString m_udmabufTitle;
    QString m_udmabufText;
    QString m_memoryText;
    /* the udmabuf explanation came up by itself in this run of vitrine */
    bool m_explained = false;
    /* the udmabuf explanation open, if it is */
    AdwDialog *m_udmabufBox = nullptr;
    /* the host tuning's state asked again for its explanation */
    bool m_rechecking = false;

    GtkWidget *m_button;
    GtkWidget *m_popover;
    GtkWidget *m_list;
};
