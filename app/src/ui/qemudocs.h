// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

#include "core/argsfile.h"
#include "core/qemuinfo.h"

/*
 * The documentation of a QEMU binary, shared by the argument editors and
 * the reference windows, loaded in the background.  Each binary has one
 * QemuDocs; the preferred one follows the QEMU of the preferences.
 */
class QemuDocs : public QObject
{
    Q_OBJECT

public:
    /* The QEMU of the preferences */
    static QemuDocs *preferred();
    /* Of @binary, or preferred() if @binary is empty */
    static QemuDocs *of(const QString &binary);
    /* Of the QEMU a VM with @args runs with: its #qemu, else the preferred */
    static QemuDocs *forArgs(const ArgsFile &args);
    /* After the preferences changed the QEMU */
    static void reloadPreferred();

    /* Null until loaded */
    const QemuInfo *info() const;
    /* Why info() is null: loading, no binary, or an error */
    QString status() const { return m_status; }
    QString binary() const { return m_binary; }
    /* Emits propertiesLoaded() once info() has them */
    void loadProperties(const QString &device);

signals:
    void changed();
    void propertiesLoaded(const QString &device);

private:
    explicit QemuDocs(bool preferred);
    void setBinary(const QString &binary);

    const bool m_preferred;
    QemuInfoLoader *m_loader = nullptr;
    QString m_binary;
    QString m_status;
};
