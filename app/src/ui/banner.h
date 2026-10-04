// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QWidget>

class IconLabel;
class QLabel;
class QPushButton;

/*
 * A note or warning across the top of a page: its icon, its text and
 * buttons on the right.  The icon and the buttons are centred on the first
 * line of the text, however many lines it wraps to, and the banner is as
 * high as its content: a layout with room to spare does not stretch it.
 */
class Banner : public QWidget
{
    Q_OBJECT

public:
    enum Type { Information, Warning };

    explicit Banner(Type type, QWidget *parent = nullptr);

    /* Rich text, whose links open in the browser: escape plain parts with toHtmlEscaped() */
    void setText(const QString &text);
    /* A button on the right, hidden until given a text */
    QPushButton *button() const { return m_button; }
    /* Another button, right of the others, shown */
    QPushButton *addButton(const QString &text);

    /*
     * As wide as it reads well, and as high as it needs at the width it
     * has: with its vertical policy, Maximum, also as high as layouts let
     * it be, which heightForWidth() alone does not stop them stretching
     */
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    /* Where the parts go in a banner @width wide */
    struct Geometry {
        QRect text;
        QList<QRect> buttons;   // the buttons shown, in their order
        qreal line = 0;         // the middle of the text's first line
        int height = 0;
    };
    Geometry arrange(int width) const;
    void place();
    /* The content changed: a new height for the width, a new size hint */
    void contentChanged();

    int m_height = -1;  // for the width it has, once laid out

    Type m_type;
    IconLabel *m_icon;
    QLabel *m_text;
    QPushButton *m_button;
    QList<QPushButton *> m_buttons;
};
