// SPDX-License-Identifier: GPL-2.0-or-later
#include "banner.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>

#include "ui/icons.h"

Banner::Banner(Type type, QWidget *parent)
    : QWidget(parent), m_type(type), m_icon(new QLabel), m_text(new QLabel),
      m_button(new QPushButton)
{
    auto *layout = new QHBoxLayout(this);
    const QIcon icon = type == Warning
                           ? Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning)
                           : Icons::themed({"dialog-information"},
                                           QStyle::SP_MessageBoxInformation);
    const int size = style()->pixelMetric(QStyle::PM_SmallIconSize);

    layout->setContentsMargins(10, 8, 10, 8);
    m_icon->setPixmap(icon.pixmap(size, size));
    m_icon->setAlignment(Qt::AlignTop);
    m_text->setWordWrap(true);
    /* always rich: guessing would show the entities of an escaped line */
    m_text->setTextFormat(Qt::RichText);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_text->setOpenExternalLinks(true);
    m_button->hide();
    layout->addWidget(m_icon);
    layout->addWidget(m_text, 1);
    layout->addWidget(m_button, 0, Qt::AlignVCenter);
}

void Banner::setText(const QString &text)
{
    m_text->setText(text);
}

void Banner::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor accent = m_type == Warning ? QColor(0xf6, 0x74, 0x00)
                                            : palette().color(QPalette::Highlight);
    QColor fill = accent;
    QColor border = accent;

    fill.setAlphaF(0.12);
    border.setAlphaF(0.6);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(border, 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
}
