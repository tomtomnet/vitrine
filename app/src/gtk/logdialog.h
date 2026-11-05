// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QByteArray>
#include <QString>

class QTimer;

/*
 * The log of a VM, which grows as QEMU writes: the Qt app's LogView in a
 * dialog.  While it shows, it reads what was added, and follows the end
 * if it was at the end.  QEMU appends to the same file at each start, so
 * only its last part is read.  Clear empties the file; the text editor
 * opens it too.
 */
class LogDialog : public Gtk4::Dialog
{
public:
    LogDialog(const QString &title, const QString &path, Presenter presenter);
    ~LogDialog() override;

    /* What it shows, for tests */
    QString text() const;
    /* Reads what was added now, rather than at the next poll */
    void poll();
    void clear();

private:
    void reload();
    void append(const QByteArray &bytes);
    bool atEnd() const;
    void update();

    QString m_path;
    QTimer *m_timer;
    qint64 m_offset = 0;
    /* a line QEMU has not ended yet */
    QByteArray m_partial;
    int m_lines = 0;

    GtkTextBuffer *m_buffer;
    GtkWidget *m_view;
    GtkWidget *m_scroll;
    GtkWidget *m_stack;
    GtkWidget *m_clear;
    GtkWidget *m_open;
};
