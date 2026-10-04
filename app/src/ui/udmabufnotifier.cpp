// SPDX-License-Identifier: GPL-2.0-or-later
#include "udmabufnotifier.h"

#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

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

    for (const UdmabufWatch::Issue &issue : issues) {
        text += "<p>" + tr("%1: %2.").arg(issue.vmName, issue.text()).toHtmlEscaped() + "</p>";
        if (why.isEmpty()) {
            why = issue.notRaised;
        }
    }
    text += "<p>" + tr("QEMU hands the host GPU each window the guest draws with the CPU (Qt "
                       "Widgets and GTK apps, cursors) as a udmabuf, which the kernel refuses "
                       "beyond its limits: the guest then copies that window at each change.") +
            "</p>";
    if (now.deviceErrno) {
        /* host tuning cannot help there */
        return text + "<p>" + Udmabuf::problem(now).toHtmlEscaped() + ".</p><p>" +
               tr("See <a href=\"%1\">docs/host-tuning.md</a>.").arg(kDocs) + "</p>";
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
        text += tr(": it is off.");
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

    for (const UdmabufWatch::Issue &issue : issues) {
        lines << tr("%1: %2.").arg(issue.vmName, issue.text());
    }
    if (!issues.isEmpty()) {
        m_button->setText(tr("udmabuf limits low"));
        m_button->setToolTip(lines.join('\n') + '\n' + tr("Click for what to do."));
    }
    m_button->setVisible(!issues.isEmpty());
    if (!issues.isEmpty() && !m_explained) {
        m_explained = true;
        /*
         * Once per run, by itself.  After the turn of the event loop: a VM
         * start that also offers to set up the vitrine group (host tuning's
         * question, which covers this) offers it right after the answer that
         * got here.
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
    if (m_box) {
        m_box->raise();
        return;
    }
    auto *box = Widgets::messageBox(QMessageBox::Warning, tr("udmabuf Limits Too Low"),
                                    explanation(issues, Udmabuf::read()), QMessageBox::Close,
                                    m_window);
    const HostSettings::Status status = m_host ? m_host->untunedStatus() : HostSettings::Status();
    QPushButton *setUp = nullptr, *turnOn = nullptr;
    if (!HostSettings::enabled()) {
        turnOn = box->addButton(tr("&Turn On Host Tuning"), QMessageBox::AcceptRole);
        turnOn->setToolTip(tr("Preferences > Tune the host while VMs run"));
    } else if (m_host && m_host->untuned() && status.needsGroup()) {
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
