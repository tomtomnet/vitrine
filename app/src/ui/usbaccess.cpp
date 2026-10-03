// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbaccess.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include <memory>

#include <unistd.h>

#include "core/hostdevices.h"
#include "core/vmconfig.h"
#include "core/vmstore.h"

namespace UsbAccess {

static QString tr(const char *text)
{
    return QCoreApplication::translate("UsbAccess", text);
}

static QString warning(const QStringList &names, const QString &why)
{
    return tr("USB passthrough: %1 cannot be passed through: %2. Allow the access when "
              "asked, or give it for good with a udev rule, as the README explains.")
        .arg(names.join(", "), why);
}

void request(Vm *vm, QObject *context, const std::function<void(const QString &warning)> &done,
             const QString &sysfs, const QString &dev)
{
    QList<std::pair<quint16, quint16>> ids;
    QStringList nodes, names;

    for (const VmConfig::UsbId &id : VmConfig::usbPassthrough(vm->args())) {
        ids << std::pair(id.vendor, id.product);
    }
    const QList<UsbDevice> devices = HostDevices::usbWithoutAccess(ids, sysfs, dev);
    if (devices.isEmpty()) {
        done({});
        return;
    }
    for (const UsbDevice &d : devices) {
        /* /dev/bus/usb/001/004 */
        nodes << dev + d.devNode().mid(4);
        names << d.displayName();
    }

    const QString pkexec = QStandardPaths::findExecutable("pkexec");
    const QString setfacl = QStandardPaths::findExecutable("setfacl");
    if (pkexec.isEmpty() || setfacl.isEmpty()) {
        done(warning(names, tr("asking for access needs pkexec (polkit) and setfacl (acl)")));
        return;
    }

    auto *process = new QProcess(context);
    /* errorOccurred and finished could both come */
    auto reported = std::make_shared<bool>(false);
    auto report = [=](const QString &why) {
        if (*reported) {
            return;
        }
        *reported = true;
        process->deleteLater();
        QStringList still;
        for (qsizetype i = 0; i < devices.size(); i++) {
            if (::access(QFile::encodeName(nodes[i]).constData(), R_OK | W_OK) != 0) {
                still << names[i];
            }
        }
        done(still.isEmpty() ? QString() : warning(still, why));
    };
    QObject::connect(process, &QProcess::finished, context,
                     [=](int code, QProcess::ExitStatus status) {
        /* pkexec: 126 for a dismissed dialog, 127 for not authorized */
        report(status != QProcess::NormalExit ? tr("the access request failed")
               : code == 126                  ? tr("the access was not given")
               : code == 127                  ? tr("you may not give access to it")
                                              : tr("this user has no access to it"));
    });
    QObject::connect(process, &QProcess::errorOccurred, context,
                     [=](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            report(tr("pkexec could not run"));
        }
    });
    process->start(pkexec,
                   QStringList{setfacl, "-m", QString("u:%1:rw").arg(getuid())} + nodes);
}

}
