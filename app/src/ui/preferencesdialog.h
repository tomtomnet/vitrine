// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QProcess;
class QTimer;

/* The tools the manager runs: QEMU and virtiofsd */
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

    void accept() override;

private:
    void checkQemu();
    void checkVirtiofsd();

    QLineEdit *m_qemu;
    QLabel *m_qemuStatus;
    QLineEdit *m_virtiofsd;
    QLabel *m_virtiofsdStatus;
    QCheckBox *m_updates;
    QTimer *m_timer;
    QProcess *m_version = nullptr;
};
