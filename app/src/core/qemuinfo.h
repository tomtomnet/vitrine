// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

/*
 * What a QEMU binary offers, for the reference search and for checking
 * arguments: its options and their documentation, its devices and their
 * properties, machines, CPU models, objects and backends.
 *
 * All of it comes from the binary's own help output, so it always matches
 * the build that runs the VM.  The full option descriptions come from
 * qemu-options.hx when the binary sits in a build tree next to it.
 */
struct QemuOptionDoc {
    QString name;           // without the dash
    QString synopsis;       // e.g. "-smp [[cpus=]n][,maxcpus=maxcpus]..."
    QString help;           // the -help text
    QString details;        // reStructuredText from qemu-options.hx
    QString section;        // e.g. "Standard options"
    bool takesValue = false;
};

struct QemuDeviceDoc {
    QString name;
    QString bus;
    QString desc;
    QString category;       // e.g. "Display devices"
    QStringList aliases;
    bool userCreatable = true;
};

struct QemuPropertyDoc {
    QString name;
    QString type;           // e.g. bool, uint32, str
    QString desc;
    QString defaultValue;
};

struct QemuNamedDoc {
    QString name;
    QString desc;
};

struct QemuInfo {
    QString version;        // e.g. 11.1.50
    QList<QemuOptionDoc> options;
    QList<QemuDeviceDoc> devices;
    QList<QemuNamedDoc> machines, cpus, objects, netdevs, chardevs,
        audiodevs, displays, accels;
    QHash<QString, QList<QemuPropertyDoc>> properties;   // per device, lazily

    const QemuOptionDoc *option(const QString &name) const;
    const QemuDeviceDoc *device(const QString &name) const;

    static QList<QemuOptionDoc> parseHelp(const QString &text);
    /* Merge the full descriptions and the argument flags of the options */
    static void mergeOptionsHx(QList<QemuOptionDoc> &options,
                               const QString &hx);
    static QList<QemuDeviceDoc> parseDeviceHelp(const QString &text);
    static QList<QemuPropertyDoc> parsePropertyHelp(const QString &text);
    /* A header line, then one "name description" per line */
    static QList<QemuNamedDoc> parseListHelp(const QString &text);
    static QString rstToHtml(const QString &rst);
};

/*
 * Loads the QemuInfo of a binary in the background, runs of the binary
 * being quick but many, and caches it per binary and modification time.
 */
class QemuInfoLoader : public QObject
{
    Q_OBJECT

public:
    explicit QemuInfoLoader(const QString &binary, QObject *parent = nullptr);

    /* Emits loaded() or failed(), once for calls made while loading */
    void load();
    bool isLoaded() const { return m_loaded; }
    const QemuInfo &info() const { return m_info; }
    QString binary() const { return m_binary; }
    /* qemu-options.hx of the source tree the binary was built in, if any */
    static QString findOptionsHx(const QString &binary);
    /* Emits propertiesLoaded() once known */
    void loadProperties(const QString &device);

signals:
    void loaded();
    void failed(const QString &error);
    void propertiesLoaded(const QString &device);

private:
    QString run(const QStringList &args, QString *error = nullptr) const;
    QString cachePath() const;
    bool loadCache();
    void saveCache() const;

    QString m_binary;
    QemuInfo m_info;
    bool m_loaded = false;
    bool m_loading = false;
};
