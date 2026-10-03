// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QIcon>
#include <QStringList>
#include <QStyle>

namespace Icons {

/* The first of @names in the icon theme, else the style's @fallback */
QIcon themed(const QStringList &names, QStyle::StandardPixmap fallback);
QIcon app();

}
