// SPDX-License-Identifier: GPL-2.0-or-later
#include "widgets.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>

Form::Form()
{
    setColumnStretch(1, 1);
}

void Form::addLabel(QWidget *label, Qt::Alignment vertical)
{
    const int row = rowCount();
    const Qt::Alignment horizontal =
        Qt::Alignment(QApplication::style()->styleHint(QStyle::SH_FormLayoutLabelAlignment)) &
        Qt::AlignHorizontal_Mask;

    if (label) {
        addWidget(label, row, 0, horizontal | vertical);
    }
}

void Form::addRow(const QString &label, QWidget *field)
{
    QLabel *l = nullptr;

    if (!label.isEmpty()) {
        l = new QLabel(label);
        l->setBuddy(field);
    }
    addRow(l, field);
}

void Form::addRow(QWidget *label, QWidget *field)
{
    const int row = rowCount();
    const bool grows = field->sizePolicy().horizontalPolicy() & QSizePolicy::ExpandFlag;

    addLabel(label, Qt::AlignVCenter);
    addWidget(field, row, 1, grows ? Qt::Alignment() : Qt::AlignLeft);
}

void Form::addRow(const QString &label, QLayout *field)
{
    addRow(label.isEmpty() ? nullptr : new QLabel(label), field);
}

void Form::addRow(QWidget *label, QLayout *field)
{
    const int row = rowCount();

    /* a column of rows: the label goes with the first */
    addLabel(label, qobject_cast<QHBoxLayout *>(field) ? Qt::AlignVCenter : Qt::AlignTop);
    addLayout(field, row, 1);
}

namespace {

class GapHandle : public QSplitterHandle
{
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent *) override {}
};

}

Splitter::Splitter(Qt::Orientation orientation, QWidget *parent) : QSplitter(orientation, parent)
{
    const int spacing = style()->pixelMetric(orientation == Qt::Horizontal
                                                 ? QStyle::PM_LayoutHorizontalSpacing
                                                 : QStyle::PM_LayoutVerticalSpacing);

    setHandleWidth(spacing > 0 ? spacing : 6);
}

QSplitterHandle *Splitter::createHandle()
{
    return new GapHandle(orientation(), this);
}

void Form::addSection(const QString &title)
{
    addWidget(Widgets::heading(title), rowCount(), 0, 1, 2);
}

namespace Widgets {

int em(const QWidget *widget)
{
    return widget->fontMetrics().height();
}

QLabel *heading(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();

    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 1.1);
    label->setFont(font);
    /* apart from what is above, not from what it names */
    label->setContentsMargins(0, QFontMetrics(font).height() / 2, 0, 0);
    return label;
}

bool confirm(QWidget *parent, QMessageBox::Icon icon, const QString &title,
             const QString &text, const QString &action)
{
    QMessageBox box(icon, title, text, QMessageBox::Cancel, parent);
    QPushButton *yes = box.addButton(action, QMessageBox::DestructiveRole);

    /* the KDE dialog would make the action the default button */
    box.setOption(QMessageBox::Option::DontUseNativeDialog);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == yes;
}

QMessageBox *messageBox(QMessageBox::Icon icon, const QString &title, const QString &text,
                        QMessageBox::StandardButtons buttons, QWidget *parent)
{
    auto *box = new QMessageBox(icon, title, text, buttons, parent);

    box->setOption(QMessageBox::Option::DontUseNativeDialog);
    return box;
}

static void tell(QWidget *parent, QMessageBox::Icon icon, const QString &title,
                 const QString &text)
{
    QMessageBox box(icon, title, text, QMessageBox::Ok, parent);

    box.setOption(QMessageBox::Option::DontUseNativeDialog);
    box.exec();
}

void inform(QWidget *parent, const QString &title, const QString &text)
{
    tell(parent, QMessageBox::Information, title, text);
}

void warn(QWidget *parent, const QString &title, const QString &text)
{
    tell(parent, QMessageBox::Warning, title, text);
}

Form *form()
{
    return new Form;
}

QLabel *label(const QString &text, QWidget *buddy)
{
    auto *label = new QLabel(text);

    label->setBuddy(buddy);
    return label;
}

QLabel *note(const QString &text)
{
    auto *label = new QLabel(text);

    /* first: word wrap adds height for width to the policy */
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(true);
    return label;
}

QLabel *hint(const QString &text)
{
    QLabel *label = note(text);
    QFont font = label->font();
    QPalette palette = label->palette();

    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
    return label;
}

QWidget *browseRow(QLineEdit *edit, const QString &title, const QString &filter, bool folder)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    auto *browse = new QPushButton(QObject::tr("Browse…"));

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->setFocusProxy(edit);
    QObject::connect(browse, &QPushButton::clicked, edit, [=]() {
        const QString current = edit->text().trimmed().isEmpty() ? edit->placeholderText()
                                                                  : edit->text().trimmed();
        const QString start = QFileInfo(current).isAbsolute()
                                  ? (folder ? current : QFileInfo(current).absolutePath())
                                  : QDir::homePath();
        const QString path =
            folder ? QFileDialog::getExistingDirectory(edit->window(), title, start)
                   : QFileDialog::getOpenFileName(edit->window(), title, start, filter);
        if (!path.isEmpty()) {
            edit->setText(path);
        }
    });
    return row;
}

void link(QSlider *slider, QSpinBox *spin, int unit)
{
    QObject::connect(slider, &QSlider::valueChanged, spin, [spin, unit](int value) {
        if (spin->value() / unit != value) {
            spin->setValue(value * unit);
        }
    });
    QObject::connect(spin, &QSpinBox::valueChanged, slider, [slider, unit](int value) {
        const QSignalBlocker block(slider);
        slider->setValue(value / unit);
    });
}

qint64 hostMemoryMiB()
{
    QFile f("/proc/meminfo");

    if (f.open(QIODevice::ReadOnly)) {
        const QRegularExpressionMatch m = QRegularExpression("MemTotal:\\s+(\\d+) kB")
                                              .match(QString::fromLatin1(f.readAll()));
        if (m.hasMatch()) {
            return m.captured(1).toLongLong() / 1024;
        }
    }
    return 16384;
}

}
