// SPDX-License-Identifier: GPL-2.0-or-later
#include "diskinfo.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <sys/stat.h>

namespace DiskInfo {

Usage usage(const QString &path)
{
    struct stat st;
    Usage u;
    QFile f(path);

    if (path.isEmpty() || ::stat(QFile::encodeName(path).constData(), &st) != 0) {
        return u;
    }
    /* a device, not a file */
    if (!S_ISREG(st.st_mode)) {
        return u;
    }
    /* the blocks it has: holes take none */
    u.used = qint64(st.st_blocks) * 512;

    /* qcow2: "QFI\xfb", its version, then at 24 the size of the disk, big-endian */
    QByteArray header;
    if (f.open(QIODevice::ReadOnly)) {
        header = f.read(32);
    }
    if (header.size() >= 32 && header.startsWith("QFI\xfb")) {
        u.format = "qcow2";
        u.capacity = qint64(qFromBigEndian<quint64>(header.constData() + 24));
        return u;
    }
    /* anything else counts as raw only by its name: VDI, VMDK... have headers of their own */
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "img" || suffix == "raw" || suffix == "iso" || suffix.isEmpty()) {
        u.format = "raw";
        u.capacity = st.st_size;
    }
    return u;
}

}
