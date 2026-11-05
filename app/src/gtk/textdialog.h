// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

/*
 * Read-only text to copy, such as a VM's command line: the Qt app's
 * TextDialog.  Copy in the header bar puts all of it on the clipboard.
 */
class TextDialog : public Gtk4::Dialog
{
public:
    /* @title over @subtitle, e.g. "Command Line" over the VM's name */
    TextDialog(const QString &title, const QString &subtitle, const QString &text,
               Presenter presenter);

    QString text() const { return m_text; }
    /* Copy */
    void copy();

private:
    QString m_text;
    AdwToastOverlay *m_toasts;
};
