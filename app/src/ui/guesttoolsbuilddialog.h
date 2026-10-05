// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

#include "core/guesttoolsbuilder.h"

class Banner;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

/*
 * Builds the guest tools (GuestToolsBuilder: guest/build-rpms.sh, then
 * guest/build-medium.sh), or updates them when this version of Vitrine
 * has other guest tools sources than the medium.  Nothing to choose: the
 * window shows what gets built, the steps, and the log.  Closing it leaves
 * the build running; Stop stops it.
 */
class GuestToolsBuildDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GuestToolsBuildDialog(QWidget *parent = nullptr);

    /* The window, not modal, over @from's window: the one open already if
       it is over that one, else a new one */
    static void present(QWidget *from);

    /* "Build Guest Tools", or "Update Guest Tools" when out of date */
    static QString title(GuestToolsBuilder::State state);
    /* "Built on 5 October 2026: vitrine-guest-tools 0.1.0-14, Mesa 26.2.3, KWin 6.7.5" */
    static QString describe();
    /* What the state means for the user, in a sentence or two */
    static QString explain(GuestToolsBuilder::State state);
    /* "step 2 of 4: building Mesa 26.2.3 (compiling, 35%)" of the build running */
    static QString progressText(const GuestToolsBuilder *builder, bool percent = true);

private:
    void build();
    void started();
    void showStep();
    void finished(const QString &error);
    void updateState();
    void showMissing();
    void showMemory();

    GuestToolsBuilder *m_builder;
    QLabel *m_about;
    QLabel *m_status;
    QLabel *m_missing;
    Banner *m_memory;
    QLabel *m_step;
    QProgressBar *m_progress;
    QLabel *m_background;
    QPlainTextEdit *m_log;
    QPushButton *m_build;
    QPushButton *m_cancel;
};

/*
 * Where a VM's guest tools are offered (Install Guest Tools): the medium
 * not built yet, being built, failed to build, or out of date, with Build…,
 * Show or Update…, which open the build window.  Hidden while the medium
 * is up to date.
 */
class GuestToolsBuildBanner : public QWidget
{
    Q_OBJECT

public:
    explicit GuestToolsBuildBanner(QWidget *parent = nullptr);

signals:
    /* A build made a new medium */
    void mediumChanged();

private:
    void refresh();

    Banner *m_warning;
    Banner *m_note;
    int m_percent = -1;
};
