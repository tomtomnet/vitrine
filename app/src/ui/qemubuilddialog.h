// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QemuBuilder;

/*
 * Builds qemu-gui, kept up to date from its repository, and makes it the
 * QEMU of the VMs.  With DRM native context, it first builds a
 * virglrenderer of its own with the renderers of every GPU that has one.
 */
class QemuBuildDialog : public QDialog
{
    Q_OBJECT

public:
    explicit QemuBuildDialog(QWidget *parent = nullptr);

signals:
    /* The binary chosen as the QEMU of the VMs */
    void qemuChanged(const QString &binary);
    /* A build ended well */
    void built();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void build();
    void finished(const QString &error);
    void updateState();
    void updateVirglStatus();
    /* The fork's branches, from GitHub; master and the chosen one until then */
    void listBranches();
    void setBranches(QStringList names, const QString &chosen);
    QString branch() const;
    void updateBranchNote();

    QemuBuilder *m_builder;
    QComboBox *m_branch;
    QLabel *m_branchNote;
    QComboBox *m_preset;
    QLineEdit *m_configure;
    QCheckBox *m_virgl;
    QLabel *m_virglStatus;
    QLabel *m_step;
    QProgressBar *m_progress;
    QPlainTextEdit *m_log;
    QPushButton *m_build;
    QPushButton *m_cancel;
    QPushButton *m_use;
};
