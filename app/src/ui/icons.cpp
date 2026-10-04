// SPDX-License-Identifier: GPL-2.0-or-later
#include "icons.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>

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

namespace {

/* The base icon with its badge, at whatever size and ratio is asked */
class BadgeEngine : public QIconEngine
{
public:
    BadgeEngine(const QIcon &base, const QColor &color, Badge badge)
        : m_base(base), m_color(color), m_badge(badge)
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override
    {
        /* the ratio of the device, as Qt's own engines take it */
        const qreal ratio = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, ratio));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    /* @size in logical pixels (Qt 6.8 on), the pixmap @scale times as large */
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override
    {
        QPixmap pixmap = m_base.pixmap(size, scale, mode, state);

        if (pixmap.isNull()) {
            return pixmap;
        }
        /* drawn in the icon's logical pixels, whatever the ratio of the pixmap */
        const QSizeF logical = pixmap.deviceIndependentSize();
        /* as designed at 32 pixels: in the corner, its outline inside the icon too */
        const qreal unit = qMin(logical.width(), logical.height()) / 32.0;
        const qreal outline = 1.5 * unit;
        const qreal diameter = 15 * unit;
        const QRectF badge(logical.width() - diameter - outline / 2,
                           logical.height() - diameter - outline / 2, diameter, diameter);
        const QPointF c = badge.center();
        QPainter p(&pixmap);

        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::white, outline));
        p.setBrush(m_color);
        p.drawEllipse(badge);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        switch (m_badge) {
        case Badge::Pause:
            p.drawRect(QRectF(c.x() - 3.0 * unit, c.y() - 3.5 * unit, 2.2 * unit, 7 * unit));
            p.drawRect(QRectF(c.x() + 0.8 * unit, c.y() - 3.5 * unit, 2.2 * unit, 7 * unit));
            break;
        case Badge::Play: {
            QPainterPath play;
            play.moveTo(c + QPointF(-2.5, -4) * unit);
            play.lineTo(c + QPointF(4, 0) * unit);
            play.lineTo(c + QPointF(-2.5, 4) * unit);
            play.closeSubpath();
            p.drawPath(play);
            break;
        }
        case Badge::Busy:
            for (int i = -1; i <= 1; i++) {
                p.drawEllipse(c + QPointF(i * 3.7, 0) * unit, 1.1 * unit, 1.1 * unit);
            }
            break;
        }
        return pixmap;
    }

    QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return m_base.actualSize(size, mode, state);
    }

    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override
    {
        return m_base.availableSizes(mode, state);
    }

    QIconEngine *clone() const override { return new BadgeEngine(m_base, m_color, m_badge); }
    QString key() const override { return QStringLiteral("vitrine-badge"); }

private:
    QIcon m_base;
    QColor m_color;
    Badge m_badge;
};

}

QIcon badged(const QIcon &base, const QColor &color, Badge badge)
{
    return QIcon(new BadgeEngine(base, color, badge));
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
