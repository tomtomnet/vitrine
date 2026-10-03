// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QPointer>
#include <QWidget>

class ArgsEditor;
class QemuDocs;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * The documentation of the QEMU binary, searchable: its options, devices
 * and their properties, machines, CPU models, objects and backends.  With
 * an editor attached, Insert puts a line for the selected entry into it;
 * alone, Copy puts it on the clipboard.
 */
class ReferencePanel : public QWidget
{
    Q_OBJECT

public:
    explicit ReferencePanel(QWidget *parent = nullptr);

    /* The documentation shown, by default of the preferred QEMU */
    void setDocs(QemuDocs *docs);
    void setEditor(ArgsEditor *editor);
    void setSearchText(const QString &text);

private:
    enum Kind {
        Option, Device, Machine, Cpu, Object, Netdev, Chardev, Audiodev, Display, Accel,
        Property,
    };

    void refresh();
    void showCurrent();
    void use();
    QString lineFor(const QTreeWidgetItem *item) const;

    QLineEdit *m_search;
    QLabel *m_status;
    QTreeWidget *m_results;
    QTextBrowser *m_doc;
    QPushButton *m_use;
    QTimer *m_timer;
    QemuDocs *m_docs = nullptr;
    QPointer<ArgsEditor> m_editor;
};

/* The reference alone, from the File menu */
class ReferenceWindow : public QWidget
{
    Q_OBJECT

public:
    static void open(QWidget *parent);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    explicit ReferenceWindow(QWidget *parent);
};
