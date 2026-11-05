// SPDX-License-Identifier: GPL-2.0-or-later
#include "preferencestext.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

QString PreferencesText::autoVirtiofsd()
{
    if (QFileInfo("/usr/libexec/virtiofsd").isExecutable()) {
        return "/usr/libexec/virtiofsd";
    }
    return QStandardPaths::findExecutable("virtiofsd");
}

QString PreferencesText::customQemuTip()
{
    /* VMs with a #qemu line of their own keep it, whatever is chosen here */
    return tr("For the VMs without a QEMU of their own; Vitrine's QEMU has what "
              "its VMs need, another one may not.");
}

QString PreferencesText::vmsDirTip()
{
    return tr("A folder holding VMs, each a folder with its vm.args: Vitrine "
              "lists the VMs it finds there, and makes new ones there");
}

QString PreferencesText::updatesHint()
{
    return tr("Of Vitrine itself: one request to GitHub.");
}

QString PreferencesText::stackButton(bool running, StackBuilder::State state)
{
    return running                                    ? tr("Show…")
           : state == StackBuilder::State::Outdated  ? tr("Update…")
           : state == StackBuilder::State::UpToDate ? tr("Details…")
                                                     : tr("Build…");
}

QString PreferencesText::toolsButton(bool running, GuestToolsBuilder::State state)
{
    return running                                         ? tr("Show…")
           : state == GuestToolsBuilder::State::Outdated ? tr("Update…")
           : state == GuestToolsBuilder::State::UpToDate ? tr("Details…")
                                                         : tr("Build…");
}

QString PreferencesText::stackBuilding()
{
    return tr("Building…");
}

QString PreferencesText::toolsBuilding(const QString &progress)
{
    return tr("Building: %1.").arg(progress);
}

QString PreferencesText::qemuNone()
{
    return tr("Choose the QEMU binary, for example one you built.");
}

QString PreferencesText::notExecutable()
{
    return tr("This file is not an executable.");
}

QString PreferencesText::checking()
{
    return tr("Checking…");
}

QString PreferencesText::qemuVersion(const QString &output)
{
    const QString first = output.section('\n', 0, 0);

    return first.contains("version") ? first.trimmed().toHtmlEscaped()
                                     : tr("This does not look like QEMU.");
}

QString PreferencesText::cannotRun(const QString &error)
{
    return tr("Cannot run it: %1").arg(error);
}

QString PreferencesText::virtiofsd(const QString &binary)
{
    if (binary.isEmpty()) {
        return tr("Not found: shared folders need it. Install it with "
                  "<code>sudo dnf install virtiofsd</code>.");
    }
    if (!QFileInfo(binary).isExecutable()) {
        return tr("This file is not an executable.");
    }
    return tr("Shared folders use %1.").arg(binary.toHtmlEscaped());
}

QString PreferencesText::vmsDirState(const QString &vmsDir)
{
    const QDir dir(vmsDir);
    int count = 0;

    for (const QString &id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFileInfo::exists(dir.filePath(id) + "/vm.args")) {
            count++;
        }
    }
    if (!QFileInfo(vmsDir).isWritable() && dir.exists()) {
        return tr("%n VM(s) found there. The folder is read-only: new VMs "
                  "cannot be made there, and VMs may not start.", nullptr,
                  count);
    }
    return tr("%n VM(s) found there. The VMs of the folder used until "
              "now stay where they are; running ones stay listed until "
              "they stop.", nullptr, count);
}
