// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/settingspage.h"

#include <QString>

class QemuDocs;
class QTimer;

/*
 * The Arguments page: the VM's QEMU command line as text, one option per
 * line, as the Qt app's ArgumentsPage edits it.  The problems with it,
 * as the QEMU documentation of the VM's QEMU tells them (checkArgs()), are
 * listed under it, a click going to the line.  The documentation is that
 * of the QEMU of the text's #qemu line, else the preferred one.
 */
class ArgumentsPage : public SettingsPage
{
public:
    explicit ArgumentsPage(const QString &vmDir);
    ~ArgumentsPage() override;

    QString title() const override { return "Arguments"; }
    const char *icon() const override { return "utilities-terminal-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

    QString text() const;
    void setText(const QString &text);
    /* Checks the text now, rather than after typing pauses */
    void check();
    /* The problems listed, as shown: "Line 3: Unknown option -foo" */
    QStringList problems() const;
    void goToLine(int line);

private:
    void setDocs(QemuDocs *docs);

    QString m_vmDir;
    QString m_loaded;
    QemuDocs *m_docs = nullptr;
    QTimer *m_timer;
    QStringList m_problems;

    GtkTextBuffer *m_buffer;
    GtkWidget *m_view;
    GtkWidget *m_problemsScroll;
    GtkWidget *m_problemsList;
};
