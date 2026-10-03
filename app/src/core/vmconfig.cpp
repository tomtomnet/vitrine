// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmconfig.h"

#include <QDir>
#include <QRegularExpression>

namespace VmConfig {

QString name(const ArgsFile &args)
{
    int i = args.indexOf("name");

    if (i < 0) {
        return {};
    }
    const OptionValue v = args.valueAt(i);
    return v.implied().isEmpty() ? v.get("guest") : v.implied();
}

void setName(ArgsFile &args, const QString &name)
{
    int i = args.indexOf("name");

    if (i < 0) {
        ArgsFile::Line line;
        int at = 0;

        /* first, after the comments at the top */
        while (at < args.lines.size() &&
               args.lines[at].kind != ArgsFile::Line::Option) {
            at++;
        }
        line.kind = ArgsFile::Line::Option;
        line.name = "name";
        line.value = OptionValue::escape(name);
        args.lines.insert(at, line);
        return;
    }
    OptionValue v = args.valueAt(i);
    if (v.implied().isEmpty() && v.has("guest")) {
        v.set("guest", name);
    } else {
        v.setImplied(name);
    }
    args.setValueAt(i, v);
}

QString qemuBinary(const ArgsFile &args)
{
    const int i = args.indexOf("qemu", ArgsFile::Line::Directive);
    QString path = i < 0 ? QString() : args.lines[i].value;

    if (path.startsWith("~/")) {
        path.replace(0, 1, QDir::homePath());
    }
    return path;
}

void setQemuBinary(ArgsFile &args, const QString &path)
{
    int i = args.indexOf("qemu", ArgsFile::Line::Directive);

    if (path.isEmpty()) {
        if (i >= 0) {
            args.removeAt(i);
        }
        return;
    }
    if (i < 0) {
        /* first, after the comments at the top */
        for (i = 0; i < args.lines.size() &&
                    args.lines[i].kind == ArgsFile::Line::Comment; i++) {
        }
        args.lines.insert(i, ArgsFile::Line{ArgsFile::Line::Directive, "qemu", {}, {}});
    }
    args.lines[i].value = path;
}

qint64 parseSize(const QString &text, qint64 unit)
{
    static const QRegularExpression re("^\\s*(\\d+(?:\\.\\d+)?)\\s*([kKmMgGtT]?)[bB]?\\s*$");
    const QRegularExpressionMatch m = re.match(text);
    double value;

    if (!m.hasMatch()) {
        return -1;
    }
    value = m.captured(1).toDouble();
    switch (m.captured(2).toUpper().isEmpty() ? '\0'
            : m.captured(2).toUpper()[0].toLatin1()) {
    case 'K':
        return qint64(value * (1LL << 10));
    case 'M':
        return qint64(value * (1LL << 20));
    case 'G':
        return qint64(value * (1LL << 30));
    case 'T':
        return qint64(value * (1LL << 40));
    default:
        return qint64(value * unit);
    }
}

QString formatMiB(qint64 mib)
{
    return mib % 1024 == 0 ? QString("%1G").arg(mib / 1024)
                           : QString("%1M").arg(mib);
}

/* The last -machine key=, as QEMU merges its -machine options */
static QString machineKey(const ArgsFile &args, const QString &key)
{
    QString value;

    for (int i : args.indexesOf("machine")) {
        const OptionValue v = args.valueAt(i);
        if (v.has(key)) {
            value = v.get(key);
        }
    }
    return value;
}

/* The -object line of the memory backend for guest RAM, or -1 */
static int ramBackend(const ArgsFile &args)
{
    const QString id = machineKey(args, "memory-backend");

    if (id.isEmpty()) {
        return -1;
    }
    for (int i : args.indexesOf("object")) {
        const OptionValue v = args.valueAt(i);
        if (v.implied().startsWith("memory-backend-") && v.get("id") == id) {
            return i;
        }
    }
    return -1;
}

qint64 memoryMiB(const ArgsFile &args)
{
    const int backend = ramBackend(args);
    const int m = args.indexOf("m");

    if (backend >= 0) {
        const qint64 bytes = parseSize(args.valueAt(backend).get("size"), 1);
        if (bytes > 0) {
            return bytes >> 20;
        }
    }
    if (m >= 0) {
        const OptionValue v = args.valueAt(m);
        const QString size = v.implied().isEmpty() ? v.get("size") : v.implied();
        const qint64 bytes = parseSize(size, 1LL << 20);
        return bytes > 0 ? bytes >> 20 : 0;
    }
    return 0;
}

void setMemoryMiB(ArgsFile &args, qint64 mib)
{
    const int backend = ramBackend(args);
    const int m = args.indexOf("m");

    if (backend >= 0) {
        OptionValue v = args.valueAt(backend);
        v.set("size", formatMiB(mib));
        args.setValueAt(backend, v);
    }
    if (m >= 0) {
        OptionValue v = args.valueAt(m);
        if (v.implied().isEmpty() && v.has("size")) {
            v.set("size", formatMiB(mib));
        } else {
            v.setImplied(formatMiB(mib));
        }
        args.setValueAt(m, v);
    } else if (backend < 0) {
        args.add("m", formatMiB(mib));
    }
}

Cpus cpus(const ArgsFile &args)
{
    Cpus c;
    const int smp = args.indexOf("smp");
    const int cpu = args.indexOf("cpu");

    if (smp >= 0) {
        const OptionValue v = args.valueAt(smp);
        const QString count = v.implied().isEmpty() ? v.get("cpus") : v.implied();

        c.sockets = v.get("sockets").toInt();
        c.cores = v.get("cores").toInt();
        c.threads = v.get("threads").toInt();
        c.count = count.toInt();
        if (c.count <= 0) {
            c.count = qMax(c.sockets, 1) * qMax(c.cores, 1) * qMax(c.threads, 1);
        }
    }
    if (cpu >= 0) {
        c.model = args.valueAt(cpu).implied();
    }
    return c;
}

static void setOrRemove(OptionValue &v, const QString &key, int value)
{
    if (value > 0) {
        v.set(key, QString::number(value));
    } else {
        v.remove(key);
    }
}

void setCpus(ArgsFile &args, const Cpus &c)
{
    int smp = args.indexOf("smp");
    int cpu = args.indexOf("cpu");
    OptionValue v;

    if (smp < 0) {
        smp = args.add("smp");
    }
    v = args.valueAt(smp);
    if (v.implied().isEmpty() && v.has("cpus")) {
        v.set("cpus", QString::number(c.count));
    } else {
        v.setImplied(QString::number(c.count));
    }
    setOrRemove(v, "sockets", c.sockets);
    setOrRemove(v, "cores", c.cores);
    setOrRemove(v, "threads", c.threads);
    args.setValueAt(smp, v);

    if (c.model.isEmpty()) {
        args.removeAll("cpu");
    } else if (cpu < 0) {
        args.add("cpu", c.model);
    } else {
        OptionValue m = args.valueAt(cpu);
        m.setImplied(c.model);
        args.setValueAt(cpu, m);
    }
}

QList<Share> shares(const ArgsFile &args)
{
    QList<Share> list;

    for (int i : args.indexesOf("share", ArgsFile::Line::Directive)) {
        const OptionValue v = args.valueAt(i);
        Share s;

        s.tag = v.get("tag");
        s.path = v.get("path");
        s.cache = v.get("cache", "auto");
        s.readonly = v.flag("readonly");
        s.mount = v.get("mount");
        list << s;
    }
    return list;
}

void setShares(ArgsFile &args, const QList<Share> &shares)
{
    const QList<int> old = args.indexesOf("share", ArgsFile::Line::Directive);
    qsizetype i;

    /* existing directives keep their place and any other keys */
    for (i = 0; i < shares.size(); i++) {
        const Share &s = shares[i];
        OptionValue v = i < old.size() ? args.valueAt(old[i]) : OptionValue();

        v.set("tag", s.tag);
        v.set("path", s.path);
        if (s.cache == "auto") {
            v.remove("cache");
        } else {
            v.set("cache", s.cache);
        }
        if (s.readonly) {
            v.setFlag("readonly", true);
        } else {
            v.remove("readonly");
        }
        if (s.mount.isEmpty()) {
            v.remove("mount");
        } else {
            v.set("mount", s.mount);
        }
        if (i < old.size()) {
            args.setValueAt(old[i], v);
        } else {
            args.add("share", v.toString(), ArgsFile::Line::Directive);
        }
    }
    for (i = old.size() - 1; i >= shares.size(); i--) {
        args.removeAt(old[i]);
    }
}

void enableCpuFeature(ArgsFile &args, const QString &feature)
{
    const int i = args.indexOf("cpu");
    OptionValue v;

    if (i < 0) {
        return;
    }
    v = args.valueAt(i);
    /* topoext, +topoext, -topoext or topoext=... */
    for (const OptionValue::Item &item : v.items()) {
        const QString key = item.key.isEmpty() ? item.value : item.key;
        if (key == feature || key.mid(1) == feature) {
            return;
        }
    }
    v.set(feature, "on");
    args.setValueAt(i, v);
}

bool hasSharedMemory(const ArgsFile &args)
{
    const int backend = ramBackend(args);

    if (backend < 0) {
        return false;
    }
    const OptionValue v = args.valueAt(backend);
    if (v.implied() == "memory-backend-memfd") {
        return v.flag("share", true);
    }
    if (v.implied() == "memory-backend-file") {
        return v.flag("share", false);
    }
    return false;
}

void useSharedMemory(ArgsFile &args)
{
    const qint64 mib = memoryMiB(args) > 0 ? memoryMiB(args) : 1024;
    const int backend = ramBackend(args);

    if (backend >= 0) {
        OptionValue v = args.valueAt(backend);
        if (v.implied() == "memory-backend-ram") {
            v.setImplied("memory-backend-memfd");
        }
        v.set("share", "on");
        args.setValueAt(backend, v);
        return;
    }

    /* a memfd backend, named after the first free "mem" id */
    QString id = "mem";
    for (int n = 1;; n++) {
        bool used = false;
        for (int i : args.indexesOf("object")) {
            used |= args.valueAt(i).get("id") == id;
        }
        if (!used) {
            break;
        }
        id = QString("mem%1").arg(n);
    }
    args.add("object", QString("memory-backend-memfd,id=%1,size=%2,share=on")
                           .arg(id, formatMiB(mib)));

    const QList<int> machines = args.indexesOf("machine");
    if (machines.isEmpty()) {
        args.add("machine", "memory-backend=" + id);
    } else {
        OptionValue v = args.valueAt(machines.last());
        v.set("memory-backend", id);
        args.setValueAt(machines.last(), v);
    }
    if (args.indexOf("m") >= 0) {
        setMemoryMiB(args, mib);
    }
}

static QString fullPciAddress(const QString &address)
{
    /* 03:00.0 -> 0000:03:00.0 */
    return address.count(':') == 1 ? "0000:" + address : address;
}

/* Options whose value is a path, keys whose value is one */
static const QStringList kFileOptions = {
    "hda", "hdb", "hdc", "hdd", "cdrom", "fda", "fdb", "bios", "kernel",
    "initrd", "dtb", "pflash", "mtdblock", "sd", "L", "readconfig",
};
static const QStringList kFileKeys = {"file", "filename", "path", "mem-path", "script"};
/* Options whose paths are sockets, or files QEMU writes */
static const QStringList kNoFiles = {
    "chardev", "qmp", "monitor", "mon", "serial", "parallel", "debugcon", "audiodev",
    "trace", "D", "pidfile",
};

QList<FileRef> files(const ArgsFile &args)
{
    QList<FileRef> out;

    for (int i = 0; i < args.lines.size(); i++) {
        const ArgsFile::Line &line = args.lines[i];

        if (line.kind != ArgsFile::Line::Option || line.value.isEmpty() ||
            kNoFiles.contains(line.name)) {
            continue;
        }
        if (kFileOptions.contains(line.name)) {
            out << FileRef{i, {}, line.value};
            continue;
        }
        const OptionValue v(line.value);
        for (const QString &key : kFileKeys) {
            const QString path = v.get(key);
            /* -object filter-dump,file= writes; mem-path= reads */
            if (path.isEmpty() || path == "no" || path.contains("://") ||
                path.startsWith("json:") || (line.name == "object" && key != "mem-path")) {
                continue;
            }
            out << FileRef{i, key, path};
        }
    }
    return out;
}

void setFile(ArgsFile &args, const FileRef &file, const QString &path)
{
    if (file.key.isEmpty()) {
        args.setValueAt(file.line, path);
        return;
    }
    OptionValue v = args.valueAt(file.line);
    v.set(file.key, path);
    args.setValueAt(file.line, v);
}

QStringList pciPassthrough(const ArgsFile &args)
{
    QStringList list;

    for (int i : args.indexesOf("device")) {
        const OptionValue v = args.valueAt(i);
        if (v.implied() == "vfio-pci" && v.has("host")) {
            list << fullPciAddress(v.get("host"));
        }
    }
    return list;
}

void setPciPassthrough(ArgsFile &args, const QStringList &addresses)
{
    QStringList missing = addresses;
    const QList<int> devices = args.indexesOf("device");

    for (qsizetype n = devices.size() - 1; n >= 0; n--) {
        const OptionValue v = args.valueAt(devices[n]);
        if (v.implied() != "vfio-pci" || !v.has("host")) {
            continue;
        }
        const QString host = fullPciAddress(v.get("host"));
        if (addresses.contains(host)) {
            missing.removeAll(host);
        } else {
            args.removeAt(devices[n]);
        }
    }
    for (const QString &host : missing) {
        args.add("device", "vfio-pci,host=" + host);
    }
}

static quint16 parseId(const QString &text)
{
    bool ok;
    uint value = text.startsWith("0x", Qt::CaseInsensitive)
                     ? text.mid(2).toUInt(&ok, 16) : text.toUInt(&ok, 0);
    return ok ? quint16(value) : 0;
}

QList<UsbId> usbPassthrough(const ArgsFile &args)
{
    QList<UsbId> list;

    for (int i : args.indexesOf("device")) {
        const OptionValue v = args.valueAt(i);
        if (v.implied() == "usb-host" && v.has("vendorid") &&
            v.has("productid")) {
            list << UsbId{parseId(v.get("vendorid")), parseId(v.get("productid"))};
        }
    }
    return list;
}

void setUsbPassthrough(ArgsFile &args, const QList<UsbId> &ids)
{
    QList<UsbId> missing = ids;
    const QList<int> devices = args.indexesOf("device");

    for (qsizetype n = devices.size() - 1; n >= 0; n--) {
        const OptionValue v = args.valueAt(devices[n]);
        if (v.implied() != "usb-host" || !v.has("vendorid") ||
            !v.has("productid")) {
            continue;
        }
        const UsbId id{parseId(v.get("vendorid")), parseId(v.get("productid"))};
        if (ids.contains(id)) {
            missing.removeAll(id);
        } else {
            args.removeAt(devices[n]);
        }
    }
    for (const UsbId &id : missing) {
        args.add("device", QString("usb-host,vendorid=0x%1,productid=0x%2")
                               .arg(id.vendor, 4, 16, QChar('0'))
                               .arg(id.product, 4, 16, QChar('0')));
    }
}

bool hasUsbController(const ArgsFile &args)
{
    static const QStringList controllers = {
        "qemu-xhci", "nec-usb-xhci", "usb-ehci", "ich9-usb-ehci1",
        "ich9-usb-ehci2", "ich9-usb-uhci1", "piix3-usb-uhci",
        "piix4-usb-uhci", "pci-ohci",
    };

    if (args.indexOf("usb") >= 0 || machineKey(args, "usb") == "on") {
        return true;
    }
    return args.indexOfDevice([](const QString &driver) {
        return controllers.contains(driver);
    }) >= 0;
}

}
