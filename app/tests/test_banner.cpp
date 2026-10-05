// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include "core/vmstore.h"
#include "scales.h"
#include "ui/banner.h"
#include "ui/guesttoolsdialog.h"
#include "ui/icons.h"

/*
 * Banners, at every scale (scales.h): the icon and the buttons on the
 * middle of the text's first line, the icon drawn crisp, the text not cut,
 * the banner as high as its content and its margins wherever it is, its
 * border on whole device pixels.  The dialog of the guest tools, whose
 * banner a layout with room to spare made three times too high, as high
 * as its content.
 */

namespace {

/* The parts of a banner, whatever its code: the icon is the child that is
   neither the text nor a button */
struct Parts {
    QWidget *icon = nullptr;
    QLabel *text = nullptr;
    QList<QPushButton *> buttons;
};

Parts partsOf(Banner *banner)
{
    Parts p;

    for (QWidget *child : banner->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
        if (auto *button = qobject_cast<QPushButton *>(child)) {
            if (!button->isHidden()) {
                p.buttons << button;
            }
        } else if (auto *label = qobject_cast<QLabel *>(child); label && !label->text().isEmpty()) {
            p.text = label;
        } else {
            p.icon = child;
        }
    }
    return p;
}

QIcon bannerIcon(bool warning)
{
    return warning ? Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning)
                   : Icons::themed({"dialog-information"}, QStyle::SP_MessageBoxInformation);
}

/* What is wrong with @banner, shown in @window (see the top) */
QStringList checkBanner(QWidget *window, Banner *banner, bool warning)
{
    using namespace Scales;
    QStringList wrong;
    const qreal ratio = window->devicePixelRatio();
    const QImage shot = grab(window);
    const Parts parts = partsOf(banner);
    const QStyle *style = banner->style();

    if (!parts.icon || !parts.text) {
        return {"no icon or no text"};
    }
    const QRect box = geometryIn(banner, window);
    const QRect text = geometryIn(parts.text, window);
    const QRect column = geometryIn(parts.icon, window);
    /* the first line, from its top to its bottom, and its middle */
    const qreal line = QFontMetricsF(parts.text->font()).height();
    const qreal middle = text.top() + line / 2;
    /* the banner's fill, in the gap between the icon and the text */
    const int gap = qFloor((column.right() + 1 + text.left()) / 2.0 * ratio);

    /* the text: all of it, its first line where the geometry says */
    const int need = parts.text->heightForWidth(parts.text->width());
    if (parts.text->height() < need) {
        wrong << QString("text cut: %1 high for %2").arg(parts.text->height()).arg(need);
    }
    {
        const int x0 = qCeil(text.left() * ratio), x1 = qFloor((text.right() + 1) * ratio) - 1;
        const int y0 = qCeil(text.top() * ratio);
        const int y1 = qMin(qFloor((text.bottom() + 1) * ratio), shot.height()) - 1;
        int first = -1, last = -1;
        for (int y = y0; y <= y1 && last < 0; y++) {
            const QRgb fill = shot.pixel(gap, y);
            bool ink = false;
            for (int x = x0; x <= x1 && !ink; x++) {
                ink = difference(shot.pixel(x, y), fill) > 60;
            }
            if (ink && first < 0) {
                first = y;
            } else if (!ink && first >= 0) {
                last = y - 1;
            }
        }
        if (first < 0) {
            wrong << "no text to be seen";
        } else if (first < text.top() * ratio - 1 ||
                   (last >= 0 && last > (text.top() + line) * ratio + 1)) {
            wrong << QString("first line drawn at %1-%2, its box is %3-%4 (device pixels)")
                         .arg(first).arg(last).arg(text.top() * ratio)
                         .arg((text.top() + line) * ratio);
        }
    }

    /* the icon: where it was drawn, and drawn as its pixmap for the ratio */
    const int size = style->pixelMetric(QStyle::PM_SmallIconSize, nullptr, banner);
    const QImage icon = bannerIcon(warning).pixmap(QSize(size, size), ratio).toImage()
                            .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QPoint guess(qRound((column.left() + column.width() / 2.0) * ratio - icon.width() / 2.0),
                       qRound(middle * ratio - icon.height() / 2.0));
    const Found found = find(shot, icon, shot.pixel(gap, qRound(middle * ratio)), guess, 2,
                             qCeil(12 * ratio));
    if (found.off > kCrisp) {
        wrong << QString("icon not its %1x%2 pixmap for %3 (off by %4 per channel at best)")
                     .arg(icon.width()).arg(icon.height()).arg(ratio).arg(found.off, 0, 'f', 1);
    }
    /* a blurred one is found too, nearly where it is */
    const qreal centre = found.at.y() + icon.height() / 2.0;
    if (qAbs(centre - middle * ratio) > 1.0 + 1e-6) {
        wrong << QString("icon's middle %1, the first line's %2 (device pixels)")
                     .arg(centre).arg(middle * ratio);
    }

    /* the buttons: on whole logical pixels, so half a logical pixel at most off */
    for (const QPushButton *button : parts.buttons) {
        const QRect b = geometryIn(button, window);
        if (qAbs(b.top() + b.height() / 2.0 - middle) > 0.5 + 1e-6) {
            wrong << QString("button %1's middle %2, the first line's %3")
                         .arg(button->text()).arg(b.top() + b.height() / 2.0).arg(middle);
        }
    }

    /* as high as its content and margins: nothing stretched */
    {
        qreal top = qMin(qreal(text.top()), found.at.y() / ratio);
        qreal bottom = qMax(qreal(text.top() + need), (found.at.y() + icon.height()) / ratio);
        for (const QPushButton *button : parts.buttons) {
            const QRect b = geometryIn(button, window);
            top = qMin(top, qreal(b.top()));
            bottom = qMax(bottom, qreal(b.top() + b.height()));
        }
        const int above = style->pixelMetric(QStyle::PM_LayoutTopMargin, nullptr, banner);
        const int below = style->pixelMetric(QStyle::PM_LayoutBottomMargin, nullptr, banner);
        if (box.height() > qCeil(bottom - top) + above + below + 1 ||
            top < box.top() + above - 1) {
            wrong << QString("banner %1 high for content %2-%3 and margins %4+%5")
                         .arg(box.height()).arg(top - box.top()).arg(bottom - box.top())
                         .arg(above).arg(below);
        }
    }

    /* its border: whole device pixels, one per whole step of the scale */
    {
        const int pen = qMax(1, qRound(ratio));
        const int y = qRound((box.top() + box.height() / 2.0) * ratio);
        const int x0 = qRound(box.left() * ratio);
        const QRgb outside = shot.pixel(x0 - 1, y);
        const QRgb inside = shot.pixel(x0 + pen, y);
        bool crisp = difference(shot.pixel(x0 - 2, y), outside) <= 3 &&
                     difference(shot.pixel(x0 + pen + 1, y), inside) <= 3 &&
                     difference(outside, inside) > 3;
        for (int x = x0; x < x0 + pen && crisp; x++) {
            crisp = difference(shot.pixel(x, y), shot.pixel(x0, y)) <= 3 &&
                    difference(shot.pixel(x, y), outside) > 3 &&
                    difference(shot.pixel(x, y), inside) > 3;
        }
        if (!crisp) {
            wrong << QString("border not %1 whole device pixels at x=%2").arg(pen).arg(x0);
        }
    }
    return wrong;
}

struct Case {
    const char *name;
    bool warning;
    const char *text;
    QStringList buttons;
    int width;
    /* as in the dialog of the guest tools: a note above, room to spare */
    bool dialog = false;
};

void useStyle(const QString &style, int points)
{
    QFont font = QApplication::font();

    QApplication::setStyle(style);
    font.setPointSize(points);
    QApplication::setFont(font);
}

}

class TestBanner : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        Scales::quieter();
        Scales::bigScreen();
        QVERIFY(m_tmp.isValid());
        /* the theme's icons where there is one, as on the desktop */
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << "/usr/share/icons");
        if (QDir("/usr/share/icons/breeze").exists()) {
            QIcon::setThemeName("breeze");
        }
        /* settings and data in here: no guest tools built */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
    }

    /* Qt's own style, and KDE's with its font where it is installed */
    void banner_data()
    {
        QTest::addColumn<QString>("style");
        QTest::addColumn<int>("points");
        QTest::newRow("fusion") << "Fusion" << 9;
        if (QStyleFactory::keys().contains("Breeze", Qt::CaseInsensitive)) {
            QTest::newRow("breeze") << "Breeze" << 10;
        }
    }

    void banner()
    {
        QFETCH(QString, style);
        QFETCH(int, points);
        const Case cases[] = {
            {"one line, a button", true,
             "<b>Vitrine's QEMU is not built yet.</b> VMs need it for 3D acceleration.",
             {"Build…"}, 760},
            {"one line", false, "Installing the guest tools in the guest…", {}, 520},
            {"two lines, two buttons", false,
             "This VM's 3D card lacks Vitrine's settings for native context and smooth "
             "frames.",
             {"Update…", "Don't Ask Again"}, 560},
            {"in a dialog", true,
             "The guest tools are not built yet. Build them in vitrine's sources with "
             "guest/build-rpms.sh, then guest/build-medium.sh (about 35 minutes).",
             {}, 520, true},
        };

        useStyle(style, points);
        Scales::sweep([&cases](int n) {
            QStringList wrong;
            for (const Case &c : cases) {
                auto *window = new QWidget;
                auto *layout = new QVBoxLayout(window);
                auto *banner = new Banner(c.warning ? Banner::Warning : Banner::Information);

                /* a few pixels in, other ones at each scale: its part of a device pixel */
                layout->setContentsMargins(3 + n % 5, 3 + n % 7, 3 + n % 5, 3);
                if (c.dialog) {
                    auto *note = new QLabel("The guest tools make a Fedora guest run well in "
                                            "vitrine. The VM starts with the tools medium; the "
                                            "guest installs them before its desktop starts.");
                    note->setWordWrap(true);
                    layout->addWidget(note);
                }
                banner->setText(QString(c.text));
                for (qsizetype i = 0; i < c.buttons.size(); i++) {
                    if (i == 0) {
                        banner->button()->setText(c.buttons[i]);
                        banner->button()->show();
                    } else {
                        banner->addButton(c.buttons[i]);
                    }
                }
                layout->addWidget(banner);
                if (c.dialog) {
                    layout->addWidget(new QCheckBox("Take a snapshot of the disks first"));
                    layout->addWidget(new QDialogButtonBox(QDialogButtonBox::Cancel));
                    /* taller than it needs: the room to spare goes somewhere */
                    window->resize(c.width, window->heightForWidth(c.width) + 120);
                } else {
                    window->resize(c.width, window->heightForWidth(c.width));
                }
                Scales::settle(window);
                for (const QString &w : checkBanner(window, banner, c.warning)) {
                    wrong << QString("%1: %2").arg(c.name, w);
                }
                delete window;
            }
            return wrong;
        });
    }

    void guestToolsDialog_data()
    {
        banner_data();
    }

    /* Its warning (no guest tools built) and its notes as high as their text */
    void guestToolsDialog()
    {
        QFETCH(QString, style);
        QFETCH(int, points);
        const QString dir = m_tmp.filePath("vms/linux");
        QFile args(dir + "/vm.args");

        QVERIFY(QDir().mkpath(dir));
        QVERIFY(args.open(QIODevice::WriteOnly));
        args.write("-name Linux\n#guest linux,desktop=kde\n-machine q35\n-m 4G\n"
                   "-device virtio-gpu-gl-pci\n-display dbus,p2p=yes,gl=on\n");
        args.close();
        VmStore store(m_tmp.filePath("vms"));
        QCOMPARE(store.vms().size(), 1);
        Vm *vm = store.vms().first();

        useStyle(style, points);
        Scales::sweep([vm](int) {
            QStringList wrong;
            GuestToolsDialog::run(nullptr, vm);
            QWidget *dialog = nullptr;
            for (QWidget *top : QApplication::topLevelWidgets()) {
                if (top->inherits("GuestToolsDialog")) {
                    dialog = top;
                }
            }
            if (!dialog) {
                return QStringList{"no dialog"};
            }
            Scales::settle(dialog);
            /* the build banner's: not built yet, with Build… */
            Banner *banner = nullptr;
            for (Banner *b : dialog->findChildren<Banner *>()) {
                if (b->isVisible()) {
                    banner = b;
                }
            }
            if (!banner) {
                wrong << "no warning";
            } else {
                for (const QString &w : checkBanner(dialog, banner, true)) {
                    wrong << "warning: " + w;
                }
            }
            /* no room to spare at first, and none in the notes */
            const int need = dialog->heightForWidth(dialog->width());
            if (dialog->height() > need + 1) {
                wrong << QString("dialog %1 high for its content's %2").arg(dialog->height())
                             .arg(need);
            }
            for (QLabel *label : dialog->findChildren<QLabel *>()) {
                if (label->wordWrap() && label->isVisible() && !label->text().isEmpty() &&
                    label->height() > label->heightForWidth(label->width()) + 1) {
                    wrong << QString("note %1 high for its text's %2").arg(label->height())
                                 .arg(label->heightForWidth(label->width()));
                }
            }
            delete dialog;
            return wrong;
        });
    }
};

QTEST_MAIN(TestBanner)
#include "test_banner.moc"
