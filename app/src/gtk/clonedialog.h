// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QPointer>

#include <functional>

class Vm;
class VmCloner;
class VmStore;

/*
 * Clones a stopped VM under a new name, showing what it copies: the Qt
 * app's CloneDialog.  The copy is the window's (VmCloner, which @start
 * begins for the name chosen): closing the dialog during the copy does
 * not stop it, Stop does.  Opened again on the same VM meanwhile, it shows
 * that copy (@cloner).  It closes once the copy is over and well, the
 * window then showing the clone; it says why if the copy failed.
 */
class CloneDialog : public Gtk4::Dialog
{
public:
    using Start = std::function<VmCloner *(Vm *source, const QString &name)>;

    CloneDialog(VmStore *store, Vm *source, VmCloner *cloner, Start start, Presenter presenter);

    VmCloner *cloner() const { return m_cloner; }
    QString name() const;
    void setName(const QString &name);
    /* What Clone does */
    void clone();

private:
    void watch(VmCloner *cloner);
    void update();

    QPointer<Vm> m_source;
    QPointer<VmCloner> m_cloner;
    Start m_start;

    GtkWidget *m_name;
    GtkWidget *m_progress;
    GtkWidget *m_background;
    GtkWidget *m_error;
    GtkWidget *m_ok;
    GtkWidget *m_cancel;
    GtkWidget *m_stop;
    GtkWidget *m_close;
};
