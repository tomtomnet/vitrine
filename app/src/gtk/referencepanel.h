// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

#include "gtk/guarded.h"
#include "gtk/qemureference.h"

class QemuDocs;
class QTimer;

/*
 * The documentation of a QEMU binary, searchable, beside the Arguments
 * page's text: its options, devices and their properties, machines, CPU
 * models, objects and backends (QemuReference), the Qt app's
 * ReferencePanel.  Insert puts a line for the entry chosen into the text.
 * Without a search the groups are closed, but Options: a click on a
 * group's title opens it.
 */
class ReferencePanel : public Gtk4::Guarded
{
public:
    ReferencePanel();
    ~ReferencePanel();
    ReferencePanel(const ReferencePanel &) = delete;
    ReferencePanel &operator=(const ReferencePanel &) = delete;

    GtkWidget *widget() const { return m_root; }
    /* The documentation shown, by default of the preferred QEMU */
    void setDocs(QemuDocs *docs);
    /* What Insert does with the entry's line */
    void setInsert(std::function<void(const QString &line)> insert) { m_insert = std::move(insert); }
    void setSearchText(const QString &text);

    /* For tests: the group titles shown, "Options (12)"; the entry chosen, and its line */
    QStringList groups() const;
    bool choose(QemuReference::Kind kind, const QString &name);
    QString chosenLine() const;

private:
    void refresh();
    void showCurrent();
    void use();
    void clearDoc();
    void addBlock(const QemuReference::Block &block);

    QObject m_qt;
    QTimer *m_timer;
    QemuDocs *m_docs = nullptr;
    std::function<void(const QString &)> m_insert;
    /* the groups opened by a click, without a search */
    QSet<int> m_opened;
    bool m_filling = false;
    /* an entry selected by the click going on */
    bool m_justSelected = false;

    GtkWidget *m_root;
    GtkWidget *m_search;
    GtkWidget *m_status;
    GtkWidget *m_results;
    GtkWidget *m_doc;
    GtkWidget *m_use;
};
