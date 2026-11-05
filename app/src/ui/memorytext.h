// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QString>

#include "core/hostmemory.h"

/*
 * The words of the memory notice, for both apps (the Qt app's
 * MemoryMonitor): the running VMs could take more memory than the host has
 * free.  Rich text as Qt's labels take it.  Qt Core only.
 */
class MemoryText
{
    /* the Qt app's MemoryMonitor's words, in its translation context */
    Q_DECLARE_TR_FUNCTIONS(MemoryMonitor)

public:
    /* The notice's few words, and its explanation's title */
    static QString label();
    static QString title();
    /* The VMs have @guestMiB of RAM and hold @heldMiB of it; the @host's memory */
    static QString text(qint64 guestMiB, qint64 heldMiB, const HostMemory::Info &host);
};
