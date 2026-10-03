// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class Vm;
class VmCloner;
class VmStore;

/* Clones a stopped VM under a new name, showing what it copies */
class CloneDialog : public QDialog
{
    Q_OBJECT

public:
    CloneDialog(VmStore *store, Vm *source, QWidget *parent = nullptr);

    /* Once accepted */
    Vm *clone() const { return m_clone; }

    void accept() override;
    void reject() override;

private:
    Vm *m_source;
    Vm *m_clone = nullptr;
    VmCloner *m_cloner;
    QLineEdit *m_name;
    QLabel *m_error;
    QProgressBar *m_progress;
    QPushButton *m_ok;
};
