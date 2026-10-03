// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

#include "core/stackbuilder.h"

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

/*
 * Builds Vitrine's QEMU (StackBuilder: host/build.sh), or updates it when
 * this version of Vitrine pins other sources or patches than the last
 * build.  Nothing to choose: the window shows what gets built, the steps,
 * and the log.  Closing it leaves the build running; Stop stops it.
 */
class QemuBuildDialog : public QDialog
{
    Q_OBJECT

public:
    explicit QemuBuildDialog(QWidget *parent = nullptr);

    /* "Build Vitrine's QEMU", or "Update Vitrine's QEMU" when out of date */
    static QString title(StackBuilder::State state);
    /* "Vitrine's build of 3 October 2026, QEMU 11.1.50 + 7 patches" */
    static QString describe(const StackBuilder::Build &build);
    /* What the state means for the user, in a sentence or two */
    static QString explain(StackBuilder::State state, const StackBuilder::Build &build);

signals:
    /* The QEMU of the VMs changed: a build replaced the one they used */
    void qemuChanged(const QString &binary);
    /* A build ended well */
    void built();

private:
    void build();
    void started();
    void stepStarted(int step, int total, const QString &text);
    void finished(const QString &error);
    void updateState();
    void showMissing();

    StackBuilder *m_builder;
    QLabel *m_about;
    QLabel *m_status;
    QLabel *m_missing;
    QLabel *m_step;
    QProgressBar *m_progress;
    QLabel *m_background;
    QPlainTextEdit *m_log;
    QPushButton *m_build;
    QPushButton *m_cancel;
    QString m_qemuBefore;
};
