// SPDX-License-Identifier: GPL-2.0-or-later
#include "icons.h"

#include <QApplication>
#include <QPainter>

#include <cmath>

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
    return QIcon(":/icons/vitrine.svg");
}

void paint(QPainter *painter, const QIcon &icon, int size, const QPointF &centre,
           QIcon::Mode mode, QIcon::State state)
{
    const QTransform toDevice = painter->deviceTransform();

    if (icon.isNull() || size <= 0) {
        return;
    }
    if (toDevice.type() > QTransform::TxScale) {
        /* turned or sheared: no whole pixels to keep to */
        const QRectF box(centre - QPointF(size, size) / 2, QSizeF(size, size));
        icon.paint(painter, box.toRect(), Qt::AlignCenter, mode, state);
        return;
    }
    /* the device pixels to a logical one, here: the screen's ratio, or
       that of the image a widget is rendered into */
    const qreal ratio = qAbs(toDevice.m22());
    const QPixmap pixmap = icon.pixmap(QSize(size, size), ratio, mode, state);
    if (pixmap.isNull()) {
        return;
    }
    const QPointF middle = toDevice.map(centre);
    const QPointF corner(std::round(middle.x() - pixmap.width() / 2.0),
                         std::round(middle.y() - pixmap.height() / 2.0));

    painter->save();
    /* in device pixels: the pixmap's own, one for one */
    painter->setWorldTransform(toDevice.inverted() * painter->worldTransform());
    painter->drawPixmap(QRectF(corner, QSizeF(pixmap.size())), pixmap, QRectF(pixmap.rect()));
    painter->restore();
}

}

IconLabel::IconLabel(const QIcon &icon, int size, QWidget *parent)
    : QWidget(parent), m_icon(icon), m_size(size)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void IconLabel::setIcon(const QIcon &icon)
{
    m_icon = icon;
    update();
}

void IconLabel::setIconSize(int size)
{
    if (size != m_size) {
        m_size = size;
        updateGeometry();
        update();
    }
}

void IconLabel::setCentre(qreal y)
{
    if (y != m_centre) {
        m_centre = y;
        update();
    }
}

QSize IconLabel::sizeHint() const
{
    return QSize(m_size, m_size);
}

QSize IconLabel::minimumSizeHint() const
{
    return sizeHint();
}

void IconLabel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const qreal y = m_centre >= 0 ? m_centre : height() / 2.0;

    Icons::paint(&painter, m_icon, m_size, QPointF(width() / 2.0, y),
                 isEnabled() ? QIcon::Normal : QIcon::Disabled);
}
