// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QPointer>

#include "core/hostsettings.h"
#include "core/udmabuf.h"

class QMessageBox;
class QToolButton;
class QWidget;

/*
 * The udmabuf issues of the running VMs (UdmabufWatch) in the main window,
 * as host tuning's warning does it: a warning in the status bar while a
 * VM has one, its tooltip says which and why, a click explains it with
 * the fix - host tuning, or the limits raised at each boot - where the
 * limits are to blame.  The first issue of a run of vitrine that the
 * limits or the device cause, and that they still would, opens that
 * explanation by itself, once, with host tuning on, unless its offer to
 * set up the vitrine group (which raises them too) came up in this run.
 */
class UdmabufNotifier : public QObject
{
    Q_OBJECT

public:
    UdmabufNotifier(UdmabufWatch *watch, HostSettings *host, QWidget *window);

    /* For the status bar: shown while a running VM has an issue */
    QToolButton *button() const { return m_button; }

    /* What the issues are, and how to fix them, rich text; @now: the limits now */
    static QString explanation(const QList<UdmabufWatch::Issue> &issues, const Udmabuf::Limits &now);
    /* The persistent ways to raise the limits, rich text */
    static QString persistentFix();

private:
    void update();
    void explain();

    UdmabufWatch *m_watch;
    QPointer<HostSettings> m_host;
    QWidget *m_window;
    QToolButton *m_button;
    QPointer<QMessageBox> m_box;
    bool m_explained = false;       // opened by itself in this run of vitrine
};
