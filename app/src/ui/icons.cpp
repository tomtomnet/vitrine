// SPDX-License-Identifier: GPL-2.0-or-later
#include "icons.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>

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

/* @path filled with @fill, inside an edge @edge device pixels wide of @dark */
void fillEdged(QPainter *painter, const QPainterPath &path, const QColor &fill,
               const QColor &dark, int edge)
{
    QPainterPathStroker stroker;

    stroker.setWidth(2 * edge);
    stroker.setJoinStyle(Qt::MiterJoin);
    painter->fillPath(path, dark);
    painter->fillPath(path.subtracted(stroker.createStroke(path)), fill);
}

/* A VM's power button, drawn in device pixels for each size and ratio asked */
class ControlEngine : public QIconEngine
{
public:
    explicit ControlEngine(Control kind) : m_kind(kind) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override
    {
        const qreal ratio = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, ratio));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State,
                         qreal scale) override
    {
        const int side = qRound(qMin(size.width(), size.height()) * scale);

        if (side <= 0) {
            return QPixmap();
        }
        QPixmap pixmap = QPixmap::fromImage(draw(side, scale));
        pixmap.setDevicePixelRatio(scale);
        if (mode != QIcon::Normal) {
            /* greyed, or as the style shows an icon under the pointer or selected */
            QStyleOption option(0);
            option.palette = QGuiApplication::palette();
            pixmap = QApplication::style()->generatedIconPixmap(mode, pixmap, &option);
        }
        return pixmap;
    }

    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override
    {
        const int side = qMin(size.width(), size.height());
        return QSize(side, side);
    }

    QIconEngine *clone() const override { return new ControlEngine(m_kind); }
    QString key() const override { return QStringLiteral("vitrine-control"); }

private:
    /* The icon @side device pixels square, for a pixel ratio of @ratio */
    QImage draw(int side, qreal ratio) const
    {
        /* Breeze's (KDE's) palette: the VM list's badges */
        const QColor green(0x27, 0xae, 0x60), amber(0xf6, 0x74, 0x00), red(0xda, 0x44, 0x53),
            blue(0x29, 0x80, 0xb9);
        QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        /* where the shapes go: the 18 middle pixels of 22, as large as VMware's */
        const int margin = qRound(side * 2 / 22.0);
        const QRect box(margin, margin, side - 2 * margin, side - 2 * margin);
        /* the darker edge: a device pixel, more at large ratios */
        const int edge = qMax(1, qRound(ratio * 0.9));
        /* the lines of the rings, whole device pixels wide */
        const int line = qMax(2, qRound(box.width() * 0.15));
        const qreal radius = box.width() / 2.0 - line / 2.0;
        QPainterPath path;

        p.setRenderHint(QPainter::Antialiasing);
        switch (m_kind) {
        case Control::Start: {
            /* a little narrower than it is high, a little right of the middle,
               which the eye puts at its wide side */
            const int width = qRound(box.height() * 0.875);
            const int left = box.left() + (box.width() - width) / 2 + qRound(width * 0.06);
            path.moveTo(left, box.top());
            path.lineTo(left + width, box.top() + box.height() / 2.0);
            path.lineTo(left, box.top() + box.height());
            path.closeSubpath();
            fillEdged(&p, path, green, green.darker(143), edge);
            break;
        }
        case Control::Pause: {
            const int bar = qRound(box.width() * 0.3);
            const int gap = qMax(1, qRound(box.width() * 0.2));
            const int left = box.left() + (box.width() - 2 * bar - gap) / 2;
            const int high = qRound(box.height() * 0.9);
            const int top = box.top() + (box.height() - high) / 2;
            path.addRect(left, top, bar, high);
            path.addRect(left + bar + gap, top, bar, high);
            fillEdged(&p, path, amber, amber.darker(137), edge);
            break;
        }
        case Control::ForceOff: {
            const int square = qRound(box.width() * 0.78);
            path.addRect(box.left() + (box.width() - square) / 2,
                         box.top() + (box.height() - square) / 2, square, square);
            fillEdged(&p, path, red, red.darker(132), edge);
            break;
        }
        case Control::ShutDown: {
            /* the power symbol: its stem's edges on device pixels, the middle
               half a pixel aside if need be */
            const QPointF centre(box.left() + box.width() / 2.0 +
                                     ((line + box.width()) % 2 ? 0.5 : 0.0),
                                 box.top() + box.height() / 2.0);
            const QRectF circle(centre - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius));
            path.arcMoveTo(circle, 90 + 42);
            path.arcTo(circle, 90 + 42, 360 - 2 * 42);
            p.strokePath(path, QPen(red, line, Qt::SolidLine, Qt::RoundCap));
            /* the stem from the ring's top to its middle */
            p.fillRect(QRectF(centre.x() - line / 2.0, box.top(), line,
                              std::round(centre.y() - box.top())),
                       red);
            break;
        }
        case Control::Reset: {
            /* clockwise from the upper right round to the top, where the
               arrowhead points on: the whole in the middle, lower by half
               what the arrowhead stands out above the ring, the ring a little
               smaller than Shut Down's, the rest of it in the margins */
            const qreal head = line * 1.4;
            const qreal out = head - line / 2.0;
            const QPointF centre(box.left() + box.width() / 2.0,
                                 box.top() + box.height() / 2.0 + out / 2);
            const qreal ring = radius - out / 4;
            const QRectF circle(centre - QPointF(ring, ring), QSizeF(2 * ring, 2 * ring));
            const QPointF end(centre.x(), centre.y() - ring);
            path.arcMoveTo(circle, 40);
            path.arcTo(circle, 40, -(360 - 50 - 8));
            p.strokePath(path, QPen(blue, line, Qt::SolidLine, Qt::FlatCap));
            QPainterPath arrow;
            arrow.moveTo(end + QPointF(-line * 0.5, -head));
            arrow.lineTo(end + QPointF(head * 1.2, 0));
            arrow.lineTo(end + QPointF(-line * 0.5, head));
            arrow.closeSubpath();
            p.fillPath(arrow, blue);
            break;
        }
        }
        return image;
    }

    Control m_kind;
};

}

QIcon badged(const QIcon &base, const QColor &color, Badge badge)
{
    return QIcon(new BadgeEngine(base, color, badge));
}

QIcon control(Control kind)
{
    return QIcon(new ControlEngine(kind));
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
