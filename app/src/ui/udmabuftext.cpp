// SPDX-License-Identifier: GPL-2.0-or-later
#include "udmabuftext.h"

#include "core/hostsettings.h"

static const char kDocs[] = "https://github.com/tomtomnet/vitrine/blob/main/docs/host-tuning.md";

QString UdmabufText::persistentFix()
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

QString UdmabufText::explanation(const QList<UdmabufWatch::Issue> &issues,
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

QString UdmabufText::line(const UdmabufWatch::Issue &issue)
{
    return tr("%1: %2.").arg(issue.vmName, issue.text());
}

QString UdmabufText::label(bool refused, bool device)
{
    return refused  ? tr("Guest windows copied")
           : device ? tr("udmabuf unavailable")
                    : tr("udmabuf limits low");
}

QString UdmabufText::title(bool refused, bool device)
{
    return refused  ? tr("Guest Windows Copied")
           : device ? tr("udmabuf Not Available")
                    : tr("udmabuf Limits Too Low");
}
