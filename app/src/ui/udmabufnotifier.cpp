// SPDX-License-Identifier: GPL-2.0-or-later
#include "udmabufnotifier.h"

#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

#include "ui/hosttuningnotifier.h"
#include "ui/icons.h"
#include "ui/widgets.h"

static const char kDocs[] = "https://github.com/tomtomnet/vitrine/blob/main/docs/host-tuning.md";

UdmabufNotifier::UdmabufNotifier(UdmabufWatch *watch, HostSettings *host, QWidget *window)
    : QObject(window), m_watch(watch), m_host(host), m_window(window), m_button(new QToolButton)
{
    m_button->setObjectName("udmabuf");
    m_button->setAutoRaise(true);
    m_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_button->setIcon(Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning));
    m_button->hide();
    connect(m_button, &QToolButton::clicked, this, &UdmabufNotifier::explain);
    connect(watch, &UdmabufWatch::changed, this, &UdmabufNotifier::update);
    update();
}

QString UdmabufNotifier::persistentFix()
{
    return "<p>" + tr("Or raise them for good, at each boot: on Fedora, with this command, then "
                      "restart the host:") +
           "</p><pre>" + Udmabuf::grubbyCommand().toHtmlEscaped() + "</pre><p>" +
           tr("or with a file %1 that holds:").arg(Udmabuf::tmpfilesPath()) + "</p><pre>" +
           Udmabuf::tmpfilesContent().trimmed().toHtmlEscaped() + "</pre><p>" +
           tr("(read at each boot; now too with <nobr>sudo systemd-tmpfiles --create "
              "%1</nobr>). See <a href=\"%2\">docs/host-tuning.md</a>.")
               .arg(Udmabuf::tmpfilesPath(), kDocs) +
           "</p>";
}

QString UdmabufNotifier::explanation(const QList<UdmabufWatch::Issue> &issues,
                                     const Udmabuf::Limits &now)
{
    QString text;
    QString why;
    bool blamed = false;

    for (const UdmabufWatch::Issue &issue : issues) {
        text += "<p>" + tr("%1: %2.").arg(issue.vmName, issue.text()).toHtmlEscaped() + "</p>";
        if (why.isEmpty()) {
            why = issue.notRaised;
        }
        blamed |= issue.limitsBlamed();
    }
    if (now.deviceErrno) {
        /* neither the limits nor host tuning can help there */
        text += "<p>" + tr("QEMU hands the host GPU each window the guest draws with the CPU (Qt "
                           "Widgets and GTK apps, cursors) as a udmabuf, made through "
                           "/dev/udmabuf: without it, the guest copies those windows at each "
                           "change.") +
                "</p>";
        const QString problem = Udmabuf::problem(now).toHtmlEscaped();
        if (!text.contains(problem)) {
            text += "<p>" + problem + ".</p>";
        }
        return text + "<p>" + tr("See <a href=\"%1\">docs/host-tuning.md</a>.").arg(kDocs) +
               "</p>";
    }
    if (!blamed) {
        /* refused for another reason: nothing to raise */
        text += "<p>" + tr("QEMU hands the host GPU each window the guest draws with the CPU (Qt "
                           "Widgets and GTK apps, cursors) as a udmabuf, and refuses the window "
                           "when it cannot: the guest then copies it at each change.");
        if (!now.low()) {
            text += ' ' + tr("The udmabuf limits, %1 entries and %2 MB, are not the reason: the "
                             "VM's qemu.log says what failed.")
                              .arg(now.listLimit)
                              .arg(now.sizeLimitMb);
        }
        return text + "</p>";
    }
    text += "<p>" + tr("QEMU hands the host GPU each window the guest draws with the CPU (Qt "
                       "Widgets and GTK apps, cursors) as a udmabuf, which the kernel refuses "
                       "beyond its limits: the guest then copies that window at each change.") +
            "</p>";
    if (!now.low()) {
        /* raised since, by hand or by another VM's host tuning */
        return text + "<p>" + tr("They are %1 entries and %2 MB now, which is enough: the windows "
                                 "the guest makes from now on are not copied.")
                                  .arg(now.listLimit)
                                  .arg(now.sizeLimitMb) +
               ' ' + tr("See <a href=\"%1\">docs/host-tuning.md</a> to have them so at each boot.")
                         .arg(kDocs) +
               "</p>";
    }
    if (now.known()) {
        text += "<p>" + tr("They are %1 entries and %2 MB now.")
                            .arg(now.listLimit)
                            .arg(now.sizeLimitMb) + ' ';
    } else {
        text += "<p>";
    }
    text += tr("Host tuning raises them to %1 entries and %2 MB while native-context VMs run")
                .arg(Udmabuf::kListLimit)
                .arg(Udmabuf::kSizeLimitMb);
    if (!HostSettings::enabled()) {
        text += HostSettings::helperInstalled()
                    ? tr(": it is off.")
                    : tr(": it is off, and vitrine-helper is not installed (see Installing in "
                         "<a href=\"%1\">docs/host-tuning.md</a>).")
                          .arg(QString(kDocs) + "#installing");
    } else if (!why.isEmpty()) {
        text += tr(", but not here: %1.").arg(why.toHtmlEscaped());
    } else {
        text += '.';
    }
    return text + "</p>" + persistentFix();
}

void UdmabufNotifier::update()
{
    const QList<UdmabufWatch::Issue> issues = m_watch->issues();
    QStringList lines;
    bool refused = false, device = false, blamed = false;

    for (const UdmabufWatch::Issue &issue : issues) {
        lines << tr("%1: %2.").arg(issue.vmName, issue.text());
        refused |= issue.log.any();
        device |= issue.device();
        blamed |= issue.limitsBlamed();
    }
    if (!issues.isEmpty()) {
        m_button->setText(refused  ? tr("Guest windows copied")
                          : device ? tr("udmabuf unavailable")
                                   : tr("udmabuf limits low"));
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
    auto *box = Widgets::messageBox(QMessageBox::Warning,
                                    refused  ? tr("Guest Windows Copied")
                                    : device ? tr("udmabuf Not Available")
                                             : tr("udmabuf Limits Too Low"),
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
