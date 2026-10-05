// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QProcess;
class QPushButton;
class QTimer;

/*
 * The tools the manager runs: Vitrine's QEMU, built and updated from here
 * too, or another one (advanced), and virtiofsd
 */
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

    void accept() override;

private:
    void updateStack();
    void checkQemu();
    void checkVirtiofsd();
    /* The VM folder chosen, and how many VMs it holds */
    void updateVmsDir();

    QLabel *m_stack;
    QLabel *m_stackState;
    QPushButton *m_build;
    QCheckBox *m_custom;
    QWidget *m_qemuRow;
    QLineEdit *m_qemu;
    QLabel *m_qemuStatus;
    QLineEdit *m_virtiofsd;
    QLabel *m_virtiofsdStatus;
    QCheckBox *m_updates;
    /* The folder of the VMs, as chosen here until OK */
    QString m_vmsDir;
    QLabel *m_vmsDirLabel;
    QLabel *m_vmsDirState;
    QPushButton *m_vmsDirDefault;
    QTimer *m_timer;
    QProcess *m_version = nullptr;
};
