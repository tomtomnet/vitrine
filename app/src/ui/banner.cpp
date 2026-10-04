// SPDX-License-Identifier: GPL-2.0-or-later
#include "banner.h"

#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QtMath>

#include <cmath>

#include "ui/icons.h"

/* A width at which any text is on as few lines as it can be */
static constexpr int kWide = 100000;

/* Between the icon, the text and the buttons: the style's, as in a layout */
static int spacing(const QWidget *widget)
{
    const QStyle *style = widget->style();
    const int spacing = style->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, nullptr, widget);

    return spacing >= 0 ? spacing
                        : qMax(0, style->layoutSpacing(QSizePolicy::Label, QSizePolicy::PushButton,
                                                       Qt::Horizontal, nullptr, widget));
}

static QMargins margins(const QWidget *widget)
{
    const QStyle *style = widget->style();

    return QMargins(style->pixelMetric(QStyle::PM_LayoutLeftMargin, nullptr, widget),
                    style->pixelMetric(QStyle::PM_LayoutTopMargin, nullptr, widget),
                    style->pixelMetric(QStyle::PM_LayoutRightMargin, nullptr, widget),
                    style->pixelMetric(QStyle::PM_LayoutBottomMargin, nullptr, widget));
}

Banner::Banner(Type type, QWidget *parent)
    : QWidget(parent), m_type(type), m_icon(new IconLabel({}, 16, this)), m_text(new QLabel(this)),
      m_button(new QPushButton(this))
{
    /* as high as its content at its width (sizeHint()), never more; a
       line high at least (minimumSizeHint()), for the window's minimum */
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    m_icon->setIcon(type == Warning ? Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning)
                                    : Icons::themed({"dialog-information"},
                                                    QStyle::SP_MessageBoxInformation));
    m_icon->setIconSize(style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this));
    m_text->setWordWrap(true);
    /* always rich: guessing would show the entities of an escaped line */
    m_text->setTextFormat(Qt::RichText);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_text->setOpenExternalLinks(true);
    /* its first line where arrange() puts it, whatever height it is given */
    m_text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_button->hide();
    m_buttons << m_button;
}

void Banner::setText(const QString &text)
{
    m_text->setText(text);
    contentChanged();
}

QPushButton *Banner::addButton(const QString &text)
{
    auto *button = new QPushButton(text, this);

    m_buttons << button;
    button->show();
    contentChanged();
    return button;
}

/*
 * The parts for @width.  The icon and the buttons go on the middle of the
 * text's first line, which is in the middle of the first row: as high as
 * the tallest of the line, the icon and the buttons.  Lines that follow
 * go under, beside the empty space under the icon and the buttons.
 */
Banner::Geometry Banner::arrange(int width) const
{
    const QMargins m = margins(this);
    const int gap = spacing(this);
    const int icon = m_icon->iconSize();
    /* the first line of the text, from its top to its bottom */
    const qreal line = QFontMetricsF(m_text->font()).height();
    QList<QSize> sizes;
    int buttonsWidth = 0;
    qreal row = qMax(line, qreal(icon));
    Geometry g;

    for (const QPushButton *button : m_buttons) {
        if (!button->isHidden()) {
            const QSize size = button->sizeHint().expandedTo(button->minimumSize());
            sizes << size;
            buttonsWidth += gap + size.width();
            row = qMax(row, qreal(size.height()));
        }
    }
    const int textLeft = m.left() + icon + gap;
    const int textWidth = qMax(1, width - m.right() - buttonsWidth - textLeft);
    const int textHeight = qMax(qCeil(line), m_text->heightForWidth(textWidth));
    /* the text on a whole pixel, its line's middle wherever that puts it */
    int textTop = qRound((row - line) / 2);
    QList<int> tops;
    int first = textTop;

    for (const QSize &size : std::as_const(sizes)) {
        tops << qRound(textTop + line / 2 - size.height() / 2.0);
        first = qMin(first, tops.last());
    }
    first = qMin(first, qFloor(textTop + (line - icon) / 2));
    /* nothing above the margin */
    textTop -= first;
    int bottom = textTop + textHeight;
    for (int &top : tops) {
        top -= first;
    }
    g.line = m.top() + textTop + line / 2;
    bottom = qMax(bottom, qCeil(textTop + (line + icon) / 2));
    g.text = QRect(textLeft, m.top() + textTop, textWidth, textHeight);
    /* right to left from the margin, in their order */
    int x = width - m.right();
    for (qsizetype i = sizes.size() - 1; i >= 0; i--) {
        x -= sizes[i].width();
        g.buttons.prepend(QRect(QPoint(x, m.top() + tops[i]), sizes[i]));
        bottom = qMax(bottom, tops[i] + sizes[i].height());
        x -= gap;
    }
    g.height = m.top() + bottom + m.bottom();
    return g;
}

/* Where arrange() says, mirrored for right-to-left */
void Banner::place()
{
    const Geometry g = arrange(width());
    const QRect all = rect();
    const Qt::LayoutDirection direction = layoutDirection();
    qsizetype i = 0;

    /* the icon's column, as high as the banner: the icon goes on the line */
    m_icon->setGeometry(QStyle::visualRect(
        direction, all, QRect(margins(this).left(), 0, m_icon->iconSize(), height())));
    m_icon->setCentre(g.line);
    m_text->setGeometry(QStyle::visualRect(direction, all, g.text));
    for (QPushButton *button : std::as_const(m_buttons)) {
        if (!button->isHidden() && i < g.buttons.size()) {
            button->setGeometry(QStyle::visualRect(direction, all, g.buttons[i++]));
        }
    }
}

QSize Banner::sizeHint() const
{
    ensurePolished();
    /* the text as wide as a label would have it, the rest as it is */
    const int width = kWide - arrange(kWide).text.width() + m_text->sizeHint().width();

    return QSize(width, m_height >= 0 ? m_height : heightForWidth(width));
}

QSize Banner::minimumSizeHint() const
{
    ensurePolished();
    const Geometry wide = arrange(kWide);

    return QSize(kWide - wide.text.width() + m_text->minimumSizeHint().width(), wide.height);
}

bool Banner::hasHeightForWidth() const
{
    return true;
}

int Banner::heightForWidth(int width) const
{
    ensurePolished();
    return arrange(width).height;
}

void Banner::contentChanged()
{
    m_height = testAttribute(Qt::WA_Resized) ? heightForWidth(width()) : -1;
    updateGeometry();
    place();
}

bool Banner::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::StyleChange:
        m_icon->setIconSize(style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this));
        [[fallthrough]];
    /* no layout of its own: a button shown, hidden or given a text, or the
       text changed, comes here */
    case QEvent::LayoutRequest:
    case QEvent::FontChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::Show:
        contentChanged();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void Banner::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    /* another width, maybe another height: the layout asks again (the
       height it gave was the most sizeHint() allowed) */
    const int height = heightForWidth(width());
    if (height != m_height) {
        m_height = height;
        updateGeometry();
    }
    place();
}

void Banner::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor accent = m_type == Warning ? QColor(0xf6, 0x74, 0x00)
                                            : palette().color(QPalette::Highlight);
    QColor fill = accent;
    QColor border = accent;
    /*
     * The box on whole device pixels, its border one of them for each
     * whole step of the scale: a line of one logical pixel would be 1.25
     * device pixels at 1.25, blurred over two
     */
    const QTransform toDevice = painter.deviceTransform();
    const qreal ratio = qAbs(toDevice.m22());
    const QRectF device = toDevice.mapRect(QRectF(rect()));
    const QRectF box(QPointF(std::round(device.left()), std::round(device.top())),
                     QPointF(std::round(device.right()), std::round(device.bottom())));
    const qreal pen = qMax(1.0, std::round(ratio));

    const qreal radius = 4 * ratio;

    fill.setAlphaF(0.12);
    border.setAlphaF(0.6);
    painter.setWorldTransform(toDevice.inverted() * painter.worldTransform());
    painter.setRenderHint(QPainter::Antialiasing);
    /* the fill inside the border, not under it: a border of one colour */
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(box.adjusted(pen, pen, -pen, -pen), radius - pen, radius - pen);
    painter.setPen(QPen(border, pen));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(box.adjusted(pen / 2, pen / 2, -pen / 2, -pen / 2), radius - pen / 2,
                            radius - pen / 2);
}
