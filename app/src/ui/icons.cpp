// SPDX-License-Identifier: GPL-2.0-or-later
#include "icons.h"

#include <QApplication>

namespace Icons {

QIcon themed(const QStringList &names, QStyle::StandardPixmap fallback)
{
    for (const QString &name : names) {
        if (QIcon::hasThemeIcon(name)) {
            return QIcon::fromTheme(name);
        }
    }
    return qApp->style()->standardIcon(fallback);
}

/* Its own, not the theme's: a copy installed by an older build would win */
QIcon app()
{
    return QIcon(":/icons/qemu-gui-manager.svg");
}

}
