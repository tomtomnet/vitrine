// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

#include "core/importer.h"

class Banner;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
struct QemuInfo;
class Vm;
class VmStore;

/*
 * Creates a VM from the QEMU command of a launch script, or of a command
 * line pasted in, showing the arguments it gets before creating it.
 */
class ImportDialog : public QDialog
{
    Q_OBJECT

public:
    /* @info, if loaded, tells which options take a value */
    ImportDialog(VmStore *store, const QemuInfo *info, QWidget *parent = nullptr);

    /* The VM created, once accepted */
    Vm *vm() const { return m_vm; }

    void accept() override;

private:
    void load(const QString &path);
    void update();

    VmStore *m_store;
    const QemuInfo *m_info;
    std::optional<Importer::Result> m_result;
    Vm *m_vm = nullptr;
    bool m_nameEdited = false;

    QPlainTextEdit *m_script;
    QLineEdit *m_baseDir;
    QLineEdit *m_name;
    QPlainTextEdit *m_preview;
    QLabel *m_notes;
    Banner *m_missing;
    QPushButton *m_create;
};
