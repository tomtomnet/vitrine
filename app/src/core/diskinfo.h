// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

/*
 * What a disk image takes on this computer, and how big the guest sees
 * it: a qcow2 or a sparse raw file only takes the blocks written to.
 */
namespace DiskInfo {

struct Usage {
    qint64 used = -1;       // bytes of the file system the file takes
    qint64 capacity = -1;   // the size of the disk in the guest; -1 when unknown
    QString format;         // qcow2 or raw, from its first bytes; empty when unknown
};

/* Quick: a stat and the header, no qemu-img.  -1 for a file that is not there,
   or a device */
Usage usage(const QString &path);

}
