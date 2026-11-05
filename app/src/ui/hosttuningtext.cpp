// SPDX-License-Identifier: GPL-2.0-or-later
#include "hosttuningtext.h"

static const char kDocs[] = "https://github.com/tomtomnet/vitrine/blob/main/docs/host-tuning.md";

/* The commands that install the helper where polkit reads its files */
static QString installCommands()
{
    return "<pre>cmake -B build -DCMAKE_INSTALL_PREFIX=/usr\ncmake --build build\n"
           "sudo cmake --install build</pre>";
}

static QString docsLink()
{
    return HostTuningText::tr("See <a href=\"%1\">docs/host-tuning.md</a>.").arg(kDocs);
}

QString HostTuningText::untuned(int count, const QString &why)
{
    return count == 1 ? tr("A running VM is not tuned: %1.").arg(why)
                      : tr("%1 running VMs are not tuned: %2.").arg(QString::number(count), why);
}

QString HostTuningText::summary()
{
    return tr("Real-time QEMU threads for the VM in front, a shorter kernel fair-server period, "
              "a GPU clock floor, huge pages for VM memory, the CPU kept ready to wake up and, "
              "for native-context VMs, higher udmabuf limits while VMs run, put back after the "
              "last one.");
}

QString HostTuningText::fix(const HostSettings::Status &status)
{
    using Problem = HostSettings::Problem;

    switch (status.problem) {
    case Problem::None:
        return QString();
    case Problem::NotInstalled:
        return tr("Install it with Vitrine, from Vitrine's source folder, configured for the "
                  "prefix <nobr>/usr</nobr>:") + installCommands() +
               tr("Then, unless you are in the vitrine group, Vitrine offers to set it up "
                  "(an administrator's password, once).") + ' ' + docsLink();
    case Problem::NoPolicy:
        return tr("polkit reads actions from /usr/share/polkit-1 only: configure Vitrine for "
                  "the prefix <nobr>/usr</nobr> and install it again, from its source folder:") +
               installCommands() + docsLink();
    case Problem::NoPolkit:
        return tr("Install polkit.");
    case Problem::NoGroup:
        return tr("Members of the vitrine group may tune the host without a password. Set Up "
                  "creates the group and adds you to it: it asks for an administrator's "
                  "password once.");
    case Problem::NotMember:
        return tr("Members of the vitrine group may tune the host without a password. Set Up "
                  "adds you to it: it asks for an administrator's password once.");
    case Problem::NotLocal:
        return tr("Start Vitrine from the desktop in front of you, not over ssh or a remote "
                  "desktop. The group's rule, 49-vitrine.rules, must be in "
                  "/usr/share/polkit-1/rules.d or /etc/polkit-1/rules.d.") + ' ' + docsLink();
    case Problem::Failed:
        break;
    }
    return docsLink();
}

HostTuningText::Box HostTuningText::active()
{
    return {tr("Host Tuning Active"), tr("Vitrine may tune the host now: it tunes the running VMs.")};
}

HostTuningText::Box HostTuningText::inactive(int count, const HostSettings::Status &status)
{
    return {tr("Host Tuning Inactive"), "<p>" + untuned(count, status.why().toHtmlEscaped()) +
                                            "</p><p>" + summary() + "</p><p>" + fix(status) +
                                            "</p>"};
}

HostTuningText::Box HostTuningText::offer(const HostSettings::Status &status)
{
    const QString group =
        status.problem == HostSettings::Problem::NoGroup
            ? tr("There is no vitrine group yet: Set Up creates it and adds you to it.")
            : tr("You are not in it: Set Up adds you to it.");

    return {tr("Tune the Host for VMs?"),
            "<p>" + tr("Vitrine can tune the host while VMs run: real-time QEMU threads, a shorter "
                       "kernel fair-server period, a GPU clock floor, huge pages for VM memory, the "
                       "CPU kept ready to wake up and higher udmabuf limits, without which "
                       "native-context VMs copy the windows of Qt Widgets and GTK apps. All of it is "
                       "put back after the last VM.") + "</p><p>" +
                tr("vitrine-helper does it as root, without a password for members of the vitrine "
                   "group.") + ' ' + group + ' ' +
                tr("It asks for an administrator's password once.") + "</p><p>" +
                tr("Until then, the VMs run untuned.") + "</p>"};
}

HostTuningText::Box HostTuningText::setUpFailed(const QString &error)
{
    return {tr("Host Tuning Not Set Up"), tr("The vitrine group could not be set up: %1.").arg(error)};
}

HostTuningText::Box HostTuningText::setUpDone(const HostSettings::Status &now)
{
    if (now.active()) {
        return {tr("Host Tuning On"),
                tr("You are in the vitrine group: Vitrine tunes the host while VMs run, "
                   "from now on and for those running now.")};
    }
    if (now.needsGroup()) {
        /* polkit and vitrine read the user database: a cache in the
           way (nscd, sssd) lets go at the next login */
        return {tr("Log Out and Back In"),
                tr("You are in the vitrine group now, but the system does not show it yet. "
                   "Log out and back in, then start the VMs again.")};
    }
    return {tr("Host Tuning Still Inactive"),
            "<p>" + tr("You are in the vitrine group now, but host tuning is still "
                       "inactive: %1.").arg(now.why().toHtmlEscaped()) +
                "</p><p>" + fix(now) + "</p>"};
}

QString HostTuningPrefsText::active(const QString &realtime)
{
    return tr("Active: vitrine-helper may tune the host without a password.") +
           (realtime.isEmpty() ? QString() : ' ' + realtime.toHtmlEscaped());
}

QString HostTuningPrefsText::notActive(const HostSettings::Status &status)
{
    return "<b>" + tr("Not active:") + "</b> " + tr("%1.").arg(status.why().toHtmlEscaped()) + ' ' +
           HostTuningText::fix(status);
}

QString HostTuningPrefsText::cpuHint()
{
    /* the research's measures: on a Ryzen 7 260 laptop a GNOME VM's keys
       11.3-11.9 ms instead of 15.1-16.3, as on the bare desktop, +2.1 W at
       idle (10.4 W against 8.35 W, 3 x 60 s with a VM idle in front); on a
       Ryzen 7 7800X3D desktop under GNOME a nested desktop's keys 7.49 ms
       instead of 10.09 (under KDE no difference), +3.5 W */
    return tr("Its cores stay in their lightest idle state, at their "
              "performance setting, so that keys show in a VM as "
              "fast as on the bare desktop. It costs about 2 W at "
              "idle on a laptop (a Ryzen 7 260, a VM idle in front) "
              "and 3.5 W on a desktop (a Ryzen 7 7800X3D), and ends "
              "when the last VM stops.");
}

QString HostTuningPrefsText::awake(const QStringList &gpus, GpuInputAwake::Installed installed,
                                   const QString &active)
{
    using enum GpuInputAwake::Installed;
    QString state;

    switch (installed) {
    case No:
        state = tr("It is not installed.");
        break;
    case Vitrine:
        state = active == "active" ? tr("It is installed and runs.")
                : active.isEmpty()  ? tr("It is installed.")
                                    : tr("It is installed and does not run (%1): <nobr>journalctl -u "
                                         "gpu-input-awake</nobr> says why.")
                                          .arg(active.toHtmlEscaped());
        break;
    case Other:
        state = tr("It is installed, not Vitrine's version.");
        break;
    case Partial:
        state = tr("It is partly installed: one of its two files is missing.");
        break;
    }
    return tr("%1: a clock floor does nothing on a dedicated GPU. gpu-input-awake, a "
              "service that runs as root from boot, wakes it at each key or pointer "
              "event instead.")
               .arg(gpus.join(", ").toHtmlEscaped()) +
           ' ' + state;
}

HostTuningText::Box HostTuningPrefsText::awakeConfirm(bool install,
                                                      GpuInputAwake::Installed installed)
{
    using enum GpuInputAwake::Installed;
    const QString title = install ? tr("Install gpu-input-awake") : tr("Remove gpu-input-awake");
    QString text;

    if (install) {
        text = tr("gpu-input-awake runs as root from now on, and at each boot: it reads the "
                  "keyboard's and the pointer's events, without taking them, and wakes the GPU "
                  "at each one.");
        if (installed == Other) {
            text += ' ' + tr("It replaces the copy installed now, which is not Vitrine's version.");
        } else if (installed == Partial) {
            text += ' ' + tr("It replaces what is installed of it now.");
        }
    } else {
        text = tr("gpu-input-awake stops now and no longer starts at boot.");
        if (installed == Other) {
            text += ' ' + tr("This removes the copy installed now, which is not Vitrine's "
                             "version: another tool may have installed it.");
        }
    }
    text += ' ' + tr("It takes an administrator's password.");
    return {title, text};
}

QString HostTuningPrefsText::awakeBusy(bool install)
{
    return install ? tr("Installing gpu-input-awake…") : tr("Removing gpu-input-awake…");
}

QString HostTuningPrefsText::awakeFailed(const QString &error)
{
    return tr("It did not work: %1.").arg(error);
}
