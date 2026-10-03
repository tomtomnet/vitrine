// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

/* A note or warning across the top of a page */
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

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Type m_type;
    QLabel *m_icon;
    QLabel *m_text;
    QPushButton *m_button;
};
