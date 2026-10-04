// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

#include <functional>

#include "core/hostsettings.h"

class QToolButton;
class QWidget;

/*
 * Host tuning in the main window.  While tuning is on and VMs run untuned
 * (HostSettings::untuned()), a warning stays in the status bar: its tooltip
 * says why, a click asks the state again and explains it with the fix.
 * Once per run, when a VM runs untuned for want of the vitrine group, it
 * offers to set the group up.  The state is asked again each time the
 * window comes back to the front while VMs run untuned: the helper
 * installed or the group joined from a terminal counts then.  Nothing of
 * this while tuning is off: that is the user's choice.
 */
class HostTuningNotifier : public QObject
{
    Q_OBJECT

public:
    HostTuningNotifier(HostSettings *host, QWidget *window);

    /* For the status bar: shown while VMs run untuned */
    QToolButton *button() const { return m_button; }

    /* What tuning does, one sentence */
    static QString summary();
    /* What to do about @status, rich text: commands, the docs */
    static QString fix(const HostSettings::Status &status);
    /*
     * The vitrine group set up (HostSettings::setUpGroup(): the desktop's
     * polkit dialog asks for an administrator's password), and how it went
     * told over @parent; then @done, if @parent is still there
     */
    static void setUp(QWidget *parent, const std::function<void()> &done = {});
    /* Tuning off, now: the Preferences' checkbox unticked */
    static void turnOff();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void update();
    /* The state asked again, then explainNow() */
    void explain();
    void explainNow(const HostSettings::Status &status);
    void ask(const HostSettings::Status &status);

    HostSettings *m_host;
    QWidget *m_window;
    QToolButton *m_button;
};
