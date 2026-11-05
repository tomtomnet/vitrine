// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QList>
#include <QString>

#include "core/udmabuf.h"

/*
 * The words of the udmabuf notice, for both apps (the Qt app's
 * UdmabufNotifier): what the issues of the running VMs are, and how to fix
 * them.  Rich text as Qt's labels take it: the GTK app makes Pango markup
 * of it.  Qt Core only.
 */
class UdmabufText
{
    /* the Qt app's UdmabufNotifier's words, in its translation context */
    Q_DECLARE_TR_FUNCTIONS(UdmabufNotifier)

public:
    /* What the issues are, and how to fix them, rich text; @now: the limits now */
    static QString explanation(const QList<UdmabufWatch::Issue> &issues, const Udmabuf::Limits &now);
    /* The persistent ways to raise the limits, rich text */
    static QString persistentFix();
    /* "Fedora: its guest windows are copied.": one issue, plain */
    static QString line(const UdmabufWatch::Issue &issue);
    /* The notice's few words, and its explanation's title: buffers @refused, or the @device */
    static QString label(bool refused, bool device);
    static QString title(bool refused, bool device);
};
