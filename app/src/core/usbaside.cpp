// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbaside.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>

namespace UsbAside {

static QString s_path = QStringLiteral("/etc/udev/rules.d/70-vitrine-usb-aside.rules");

QString path()
{
    return s_path;
}

void setPath(const QString &path)
{
    s_path = path;
}

QList<Ids> read(const QString &file)
{
    /* the helper's lines: ATTR{idVendor}=="vvvv", ATTR{idProduct}=="pppp" */
    static const QRegularExpression line(
        "^SUBSYSTEM==\"usb\".*ATTR\\{idVendor\\}==\"([0-9a-f]{4})\".*ATTR\\{idProduct\\}==\"([0-9a-f]{4})\"");
    QFile f(file);
    QList<Ids> ids;

    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QRegularExpressionMatch m = line.match(QString::fromLatin1(raw));
        if (m.hasMatch()) {
            const Ids id{quint16(m.captured(1).toUInt(nullptr, 16)),
                         quint16(m.captured(2).toUInt(nullptr, 16))};
            if (!ids.contains(id)) {
                ids << id;
            }
        }
    }
    return ids;
}

QString text(const QList<Ids> &ids)
{
    QStringList parts;

    for (const Ids &id : ids) {
        parts << QString("%1:%2").arg(id.first, 4, 16, QChar('0')).arg(id.second, 4, 16, QChar('0'));
    }
    return parts.join(',');
}

QList<Ids> missing(const QList<Ids> &ids, const QList<Ids> &aside)
{
    QList<Ids> out;

    for (const Ids &id : ids) {
        if (!aside.contains(id) && !out.contains(id)) {
            out << id;
        }
    }
    return out;
}

}
