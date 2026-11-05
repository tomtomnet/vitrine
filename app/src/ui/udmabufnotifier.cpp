// SPDX-License-Identifier: GPL-2.0-or-later
#include "udmabufnotifier.h"

#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

#include "ui/hosttuningnotifier.h"
#include "ui/icons.h"
#include "ui/udmabuftext.h"
#include "ui/widgets.h"

UdmabufNotifier::UdmabufNotifier(UdmabufWatch *watch, HostSettings *host, QWidget *window)
    : QObject(window), m_watch(watch), m_host(host), m_window(window),
      m_button(Widgets::statusButton(
          "udmabuf", Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning)))
{
    connect(m_button, &QToolButton::clicked, this, &UdmabufNotifier::explain);
    connect(watch, &UdmabufWatch::changed, this, &UdmabufNotifier::update);
    update();
}

QString UdmabufNotifier::persistentFix()
{
    return UdmabufText::persistentFix();
}

QString UdmabufNotifier::explanation(const QList<UdmabufWatch::Issue> &issues,
                                     const Udmabuf::Limits &now)
{
    return UdmabufText::explanation(issues, now);
}

void UdmabufNotifier::update()
{
    const QList<UdmabufWatch::Issue> issues = m_watch->issues();
    QStringList lines;
    bool refused = false, device = false, blamed = false;

    for (const UdmabufWatch::Issue &issue : issues) {
        lines << UdmabufText::line(issue);
        refused |= issue.log.any();
        device |= issue.device();
        blamed |= issue.limitsBlamed();
    }
    if (!issues.isEmpty()) {
        m_button->setText(UdmabufText::label(refused, device));
        m_button->setToolTip(lines.join('\n') + '\n' + tr("Click for what to do."));
    }
    m_button->setVisible(!issues.isEmpty());
    /*
     * Once per run, by itself, when there is something to do now: the
     * limits (or the device) still in the way, with host tuning on.  Off
     * by the user's choice, the button is enough, as host tuning's own
     * warning says nothing then; and buffers refused for another reason,
     * or before the limits were raised (in the log of a VM found
     * running), call for no fix.
     */
    if (!m_explained && HostSettings::enabled() && (blamed || device) &&
        m_watch->limitsNow().low()) {
        m_explained = true;
        /*
         * After the turn of the event loop: a VM start that also offers to
         * set up the vitrine group (host tuning's question, which covers
         * this) offers it right after the answer that got here.
         */
        QTimer::singleShot(0, this, [this]() {
            if ((!m_host || !m_host->groupOffered()) && !m_watch->issues().isEmpty()) {
                explain();
            }
        });
    }
}

void UdmabufNotifier::explain()
{
    const QList<UdmabufWatch::Issue> issues = m_watch->issues();

    if (issues.isEmpty()) {
        m_button->hide();
        return;
    }
    if (m_box && m_box->isVisible()) {
        m_box->raise();
        return;
    }
    const Udmabuf::Limits now = m_watch->limitsNow();
    auto any = [&issues](bool (UdmabufWatch::Issue::*what)() const) {
        return std::any_of(issues.begin(), issues.end(),
                           [what](const UdmabufWatch::Issue &issue) { return (issue.*what)(); });
    };
    const bool refused = std::any_of(issues.begin(), issues.end(),
                                     [](const UdmabufWatch::Issue &issue) { return issue.log.any(); });
    const bool device = now.deviceErrno || any(&UdmabufWatch::Issue::device);
    auto *box = Widgets::messageBox(QMessageBox::Warning, UdmabufText::title(refused, device),
                                    explanation(issues, now), QMessageBox::Close, m_window);
    const HostSettings::Status status = m_host ? m_host->untunedStatus() : HostSettings::Status();
    QPushButton *setUp = nullptr, *turnOn = nullptr;
    /* host tuning, where raising the limits helps; not without the helper
       installed: the explanation points to Installing */
    const bool raise = any(&UdmabufWatch::Issue::limitsBlamed) && !now.deviceErrno;
    if (raise && !HostSettings::enabled() && HostSettings::helperInstalled()) {
        turnOn = box->addButton(tr("&Turn On Host Tuning"), QMessageBox::AcceptRole);
        turnOn->setToolTip(tr("Preferences > Tune the host while VMs run"));
    } else if (raise && HostSettings::enabled() && m_host && m_host->untuned() &&
               status.needsGroup()) {
        setUp = box->addButton(tr("&Set Up…"), QMessageBox::AcceptRole);
    }
    /* it may come up by itself, at a VM start: an Enter meant for the guest
       closes it, and nothing else */
    box->setDefaultButton(QMessageBox::Close);
    box->setEscapeButton(QMessageBox::Close);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box, setUp, turnOn]() {
        if (setUp && box->clickedButton() == setUp) {
            HostTuningNotifier::setUp(m_window);
        } else if (turnOn && box->clickedButton() == turnOn) {
            HostSettings::setEnabled(true);
            if (m_host) {
                m_host->preferencesChanged();
            }
        }
    });
    m_box = box;
    box->open();
}
