// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QObject>
#include <QString>

#include <functional>

#include "gtk/guarded.h"

namespace Gtk4 {

/*
 * The C++ side of an in-window dialog, an AdwDialog: made with new, it
 * lives as long as its dialog is open, and goes once the dialog closes,
 * from GLib's loop, not from within the dialog's own signal.  A dialog
 * that never opens is deleted by whoever made it.
 *
 * Qt's connections to the core go to qt(): they go with it.  The
 * connections to GTK's signals go through on() (Guarded).  What it
 * presents over itself (an alert, another dialog) goes through the
 * window's present() (MainWindow::present(), which covers the VM's screen
 * meanwhile), which it gets as @presenter.
 */
class Dialog : public Guarded
{
public:
    using Presenter = std::function<void(AdwDialog *dialog)>;

    virtual ~Dialog();

    AdwDialog *dialog() const { return m_dialog; }
    /* Closes it, as its close button would: can-close and close-attempt apply */
    void close();
    /* Closes it, whatever it would ask */
    void forceClose();
    bool isClosed() const { return m_closed; }

protected:
    Dialog(AdwDialog *dialog, Presenter presenter);

    QObject *qt() { return &m_qt; }
    /* @dialog over this one */
    void present(AdwDialog *dialog) const;
    const Presenter &presenter() const { return m_present; }
    /* Once it closed, before it goes */
    virtual void closed() {}

private:
    /* Closed: closed(), then it goes */
    void finish();

    AdwDialog *m_dialog;
    Presenter m_present;
    QObject m_qt;
    bool m_closed = false;
};

}
