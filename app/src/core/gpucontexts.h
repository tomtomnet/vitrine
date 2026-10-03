// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

class QmpClient;

/*
 * Whether the guest of a running VM draws through DRM native context, as
 * asked, or fell back to virgl: most distributions build Mesa without
 * native context.  The qemu-gui build of QEMU counts the contexts the guest
 * creates on its virtio-gpu-gl device, and says whether it offered native
 * context (qom-get of x-drm-offered, x-drm-contexts, x-virgl-contexts).
 */
class GpuContexts : public QObject
{
    Q_OBJECT

public:
    enum class Status {
        Unknown,        // not asked yet, no 3D GPU, or a QEMU without the counts
        NotOffered,     // this computer offers none: virglrenderer has no renderer for its GPU
        Waiting,        // offered; the guest has not drawn in 3D yet since it booted
        InUse,          // the guest created native contexts
        Virgl,          // offered, but the guest only created virgl contexts
    };

    explicit GpuContexts(QObject *parent = nullptr);

    /* Asks QEMU; changed() once the status is known again */
    void update(QmpClient *qmp);
    /* Another run of QEMU, or another VM: the GPU is looked for again */
    void reset();
    Status status() const { return m_status; }
    /* The guest created Venus contexts since it booted */
    bool venusUsed() const { return m_venusUsed; }

    /* The QOM path of the 3D GPU among the children of @parent, from qom-list */
    static QString findGpu(const QJsonArray &children, const QString &parent);
    static Status statusOf(bool offered, qint64 drmContexts, qint64 virglContexts);

signals:
    void changed();

private:
    void findGpu(int parent);
    void readCounts(QStringList properties, QJsonObject values);
    void finish(Status status, bool venusUsed = false);

    QPointer<QmpClient> m_qmp;
    QString m_path;
    Status m_status = Status::Unknown;
    bool m_venusUsed = false;
    bool m_busy = false;
};
