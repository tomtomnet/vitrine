// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QIcon>
#include <QStringList>
#include <QStyle>
#include <QWidget>

class QPainter;

namespace Icons {

/* The first of @names in the icon theme, else the style's @fallback */
QIcon themed(const QStringList &names, QStyle::StandardPixmap fallback);
QIcon app();

/* What a badge shows in its circle */
enum class Badge { Play, Pause, Busy };
/*
 * @base with a round badge of @color in its bottom right corner, drawn
 * for each size and pixel ratio it is asked at, rather than scaled from
 * pixmaps made beforehand for a few ratios
 */
QIcon badged(const QIcon &base, const QColor &color, Badge badge);

/*
 * @icon at @size (logical pixels) with its middle at @centre, on whole
 * device pixels of @painter's device: a pixmap made for that device's
 * pixel ratio, put there as it is.  At 1.25 or 1.5 (Wayland's fractional
 * scales), a pixmap of another ratio, or one put between two device
 * pixels, would come out blurred.
 */
void paint(QPainter *painter, const QIcon &icon, int size, const QPointF &centre,
           QIcon::Mode mode = QIcon::Normal, QIcon::State state = QIcon::Off);

}

/*
 * An icon on its own, where a QLabel would show a pixmap: drawn each time
 * for the pixel ratio of the screen it is on.  A QLabel keeps the pixmap
 * made for the ratio of the moment (the highest of the screens, by
 * default), which a window at another ratio shows scaled: on Wayland at
 * 1.5, a pixmap made for 2.
 */
class IconLabel : public QWidget
{
public:
    explicit IconLabel(const QIcon &icon = {}, int size = 16, QWidget *parent = nullptr);

    QIcon icon() const { return m_icon; }
    void setIcon(const QIcon &icon);
    int iconSize() const { return m_size; }
    void setIconSize(int size);
    /* Where the middle of the icon goes, from the top (a line of text it
       goes with); by default the middle of the widget */
    void setCentre(qreal y);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QIcon m_icon;
    int m_size;
    qreal m_centre = -1;
};
