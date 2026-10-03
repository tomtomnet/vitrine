// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemudocs.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include "core/paths.h"
#include "core/vmconfig.h"

/* One loader per binary, whichever QemuDocs use it */
static QemuInfoLoader *loaderOf(const QString &binary)
{
    static QHash<QString, QemuInfoLoader *> loaders;
    QemuInfoLoader *&loader = loaders[binary];

    if (!loader) {
        loader = new QemuInfoLoader(binary, QCoreApplication::instance());
    }
    return loader;
}

/* Loads once, as a second load() would run QEMU all over again */
static void startLoading(QemuInfoLoader *loader)
{
    static QSet<QemuInfoLoader *> loading;

    if (loader->isLoaded() || loading.contains(loader)) {
        return;
    }
    loading.insert(loader);
    QObject::connect(loader, &QemuInfoLoader::failed, loader,
                     [loader]() { loading.remove(loader); });
    QObject::connect(loader, &QemuInfoLoader::loaded, loader,
                     [loader]() { loading.remove(loader); });
    /* emits loaded() at once when cached */
    loader->load();
}

QemuDocs *QemuDocs::preferred()
{
    static QemuDocs *docs = nullptr;

    if (!docs) {
        docs = new QemuDocs(true);
        docs->setBinary(Paths::qemuBinary());
    }
    return docs;
}

QemuDocs *QemuDocs::of(const QString &binary)
{
    static QHash<QString, QemuDocs *> all;

    if (binary.isEmpty()) {
        return preferred();
    }
    QemuDocs *&docs = all[binary];
    if (!docs) {
        docs = new QemuDocs(false);
        docs->setBinary(binary);
    }
    return docs;
}

QemuDocs *QemuDocs::forArgs(const ArgsFile &args)
{
    return of(VmConfig::qemuBinary(args));
}

void QemuDocs::reloadPreferred()
{
    preferred()->setBinary(Paths::qemuBinary());
}

QemuDocs::QemuDocs(bool preferred)
    : QObject(QCoreApplication::instance()), m_preferred(preferred)
{
}

const QemuInfo *QemuDocs::info() const
{
    return m_loader && m_loader->isLoaded() ? &m_loader->info() : nullptr;
}

void QemuDocs::setBinary(const QString &binary)
{
    if (m_loader && binary == m_binary) {
        return;
    }
    if (m_loader) {
        m_loader->disconnect(this);
        m_loader = nullptr;
    }
    m_binary = binary;
    if (binary.isEmpty()) {
        m_status = tr("QEMU was not found. Set its path in the preferences.");
        emit changed();
        return;
    }
    if (!QFileInfo(binary).isExecutable()) {
        m_status = m_preferred
                       ? tr("%1 is not an executable. Check the QEMU in the preferences.")
                             .arg(binary)
                       : tr("%1 is not an executable.").arg(binary);
        emit changed();
        return;
    }

    m_loader = loaderOf(binary);
    connect(m_loader, &QemuInfoLoader::loaded, this, [this]() {
        m_status.clear();
        emit changed();
    });
    connect(m_loader, &QemuInfoLoader::failed, this, [this](const QString &error) {
        m_status = tr("Cannot read the QEMU documentation: %1").arg(error);
        emit changed();
    });
    connect(m_loader, &QemuInfoLoader::propertiesLoaded, this, &QemuDocs::propertiesLoaded);
    if (m_loader->isLoaded()) {
        m_status.clear();
        emit changed();
        return;
    }
    m_status = tr("Loading the QEMU documentation…");
    emit changed();
    startLoading(m_loader);
}

void QemuDocs::loadProperties(const QString &device)
{
    if (m_loader && m_loader->isLoaded()) {
        m_loader->loadProperties(device);
    }
}
