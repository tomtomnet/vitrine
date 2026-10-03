// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpucontexts.h"

#include <QJsonObject>

#include "core/qmpclient.h"

/* Where the devices of the command line are: with an id, and without */
static const char *const kParents[] = {"/machine/peripheral", "/machine/peripheral-anon"};
static const QStringList kProperties{"x-drm-offered", "x-drm-contexts", "x-virgl-contexts",
                                     "x-venus-contexts"};

GpuContexts::GpuContexts(QObject *parent) : QObject(parent)
{
}

QString GpuContexts::findGpu(const QJsonArray &children, const QString &parent)
{
    /* the PCI and VGA forms pass the properties of the GPU on */
    static const QStringList types{"child<virtio-vga-gl>", "child<virtio-gpu-gl-pci>",
                                   "child<virtio-gpu-gl-device>"};

    for (const QJsonValue &child : children) {
        if (types.contains(child["type"].toString())) {
            return parent + '/' + child["name"].toString();
        }
    }
    return {};
}

GpuContexts::Status GpuContexts::statusOf(bool offered, qint64 drmContexts,
                                          qint64 virglContexts)
{
    if (!offered) {
        return Status::NotOffered;
    }
    if (drmContexts > 0) {
        return Status::InUse;
    }
    return virglContexts > 0 ? Status::Virgl : Status::Waiting;
}

void GpuContexts::reset()
{
    m_path.clear();
    m_status = Status::Unknown;
    m_venusUsed = false;
    m_busy = false;
}

void GpuContexts::update(QmpClient *qmp)
{
    if (m_busy || !qmp || !qmp->isReady()) {
        return;
    }
    m_busy = true;
    m_qmp = qmp;
    if (m_path.isEmpty()) {
        findGpu(0);
    } else {
        readCounts(kProperties, {});
    }
}

void GpuContexts::findGpu(int parent)
{
    if (parent >= int(std::size(kParents))) {
        finish(Status::Unknown);       // no 3D GPU
        return;
    }
    m_qmp->execute("qom-list", {{"path", kParents[parent]}},
                   [this, parent, guard = QPointer<GpuContexts>(this)](const QJsonValue &result,
                                                                       const QString &error) {
        if (!guard) {
            return;
        }
        m_path = error.isEmpty() ? findGpu(result.toArray(), kParents[parent]) : QString();
        if (m_path.isEmpty()) {
            findGpu(parent + 1);
        } else if (m_qmp) {
            readCounts(kProperties, {});
        } else {
            finish(Status::Unknown);
        }
    });
}

/*
 * One property after the other: a QEMU without the counts has no
 * x-drm-offered, and nothing is told then
 */
void GpuContexts::readCounts(QStringList properties, QJsonObject values)
{
    const QPointer<GpuContexts> guard(this);

    if (properties.isEmpty()) {
        finish(statusOf(values["x-drm-offered"].toBool(), values["x-drm-contexts"].toInteger(),
                        values["x-virgl-contexts"].toInteger()),
               values["x-venus-contexts"].toInteger() > 0);
        return;
    }
    const QString property = properties.takeFirst();
    m_qmp->execute("qom-get", {{"path", m_path}, {"property", property}},
                   [=, this](const QJsonValue &value, const QString &error) mutable {
        if (!guard) {
            return;
        }
        if (!error.isEmpty() || !m_qmp) {
            finish(Status::Unknown);
            return;
        }
        values[property] = value;
        readCounts(properties, values);
    });
}

void GpuContexts::finish(Status status, bool venusUsed)
{
    const bool changing = status != m_status || venusUsed != m_venusUsed;

    m_busy = false;
    m_status = status;
    m_venusUsed = venusUsed;
    if (changing) {
        emit changed();
    }
}
