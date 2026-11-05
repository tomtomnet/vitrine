// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class Vm;

/*
 * How the guest tools of the VM shown are doing, with what goes with it:
 * Install Guest Tools…, Cancel, Try Again…, Restart Guest, Install
 * Again…, Update… (the Qt app's GuestToolsBanner), as a row on the
 * Console's home page and as the Details' Guest Tools group.  They are
 * offered to a guest known to run a system they are for, as it said
 * through an agent when it last ran (GuestTools::supports()), with the 3D
 * card their driver is for; not to one that never said.  Hidden when
 * there is nothing to say.
 */
class GuestToolsStatus
{
public:
    GuestToolsStatus();
    ~GuestToolsStatus();
    GuestToolsStatus(const GuestToolsStatus &) = delete;
    GuestToolsStatus &operator=(const GuestToolsStatus &) = delete;

    /* For the home page: a boxed list of one row */
    GtkWidget *homeWidget() const { return m_home; }
    /* For the Details */
    AdwPreferencesGroup *detailsGroup() const { return m_details; }

    void setVm(Vm *vm);
    Vm *vm() const;

    /* Install Guest Tools… for @vm (its dialog) */
    std::function<void(Vm *vm)> install;
    /* The install pending for @vm called off: also the restart its install waits to make */
    std::function<void(Vm *vm)> cancelPending;

    /* For tests: what it says (plain text) and its button; empty while hidden */
    QString text() const;
    QString button() const;
    /* Its button */
    void activate();

private:
    struct Row {
        GtkWidget *row;
        GtkWidget *icon;
        GtkWidget *button;
    };
    Row makeRow();
    void update();

    QObject m_qt;
    QPointer<Vm> m_vm;
    QString m_text;
    QString m_button;
    GtkWidget *m_home;
    AdwPreferencesGroup *m_details;
    Row m_homeRow;
    Row m_detailsRow;
};
