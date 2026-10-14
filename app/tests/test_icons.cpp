// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QToolButton>

#include <cmath>

#include "core/vmstore.h"
#include "scales.h"
#include "ui/icons.h"
#include "ui/memorymonitor.h"
#include "ui/vmpane.h"
#include "ui/widgets.h"

/*
 * Icons drawn for the pixel ratio of where they show, at every scale
 * (scales.h): an IconLabel's icon is its pixmap for the window's ratio,
 * one for one, on whole device pixels, in the middle of where it goes,
 * and for another ratio when rendered for one (a window moved to another
 * screen).  The VM list's icons with their badge are drawn at the ratio
 * asked, not scaled from pixmaps made for 1, 2 and 3.  The VM's power
 * buttons drawn for the ratio asked: crisp, in the middle, legible on a
 * light and a dark window, disabled as the theme's icons.  The icons of a role have
 * the style's size for it.
 */
class TestIcons : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

    static QImage image(const QPixmap &pixmap)
    {
        return pixmap.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    static QIcon computer() { return Icons::themed({"computer"}, QStyle::SP_ComputerIcon); }

    /* WCAG's contrast ratio of two colours, 1 to 21 */
    static qreal contrast(QRgb a, QRgb b)
    {
        auto luminance = [](QRgb c) {
            auto linear = [](int v) {
                const qreal x = v / 255.0;
                return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * linear(qRed(c)) + 0.7152 * linear(qGreen(c)) +
                   0.0722 * linear(qBlue(c));
        };
        const qreal la = luminance(a), lb = luminance(b);
        return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
    }

private slots:
    /*
     * The Builds window's status icons keep their colours on a selected row
     * (KDE's theme turned its green check all white there): keepColours()
     * gives the Normal pixmap where Selected or Active is asked, and the
     * up-to-date check is the same in both, green with its white check
     */
    void keepsColours()
    {
        QPixmap red(16, 16), white(16, 16);
        red.fill(Qt::red);
        white.fill(Qt::white);
        QIcon theme;
        theme.addPixmap(red, QIcon::Normal);
        theme.addPixmap(white, QIcon::Selected);
        theme.addPixmap(white, QIcon::Active);
        const QIcon kept = Icons::keepColours(theme);
        for (QIcon::Mode mode : {QIcon::Selected, QIcon::Active}) {
            const QImage image = kept.pixmap(QSize(16, 16), 1.0, mode).toImage();
            QCOMPARE(image.pixelColor(8, 8), QColor(Qt::red));
        }

        const QIcon check = Icons::upToDate();
        for (qreal ratio : {1.0, 1.5, 2.0}) {
            const QImage normal = check.pixmap(QSize(16, 16), ratio, QIcon::Normal).toImage();
            const QImage selected = check.pixmap(QSize(16, 16), ratio, QIcon::Selected).toImage();
            QCOMPARE(normal.size(), QSize(qRound(16 * ratio), qRound(16 * ratio)));
            QCOMPARE(selected, normal);
            /* green near a corner, inside the rounded edge; the check's white somewhere */
            const QColor green = normal.pixelColor(qRound(3 * ratio), qRound(13 * ratio));
            QVERIFY2(green.green() > 150 && green.red() < 80, qPrintable(green.name()));
            int whitest = 0;
            for (int y = 0; y < normal.height(); y++) {
                for (int x = 0; x < normal.width(); x++) {
                    whitest = qMax(whitest, normal.pixelColor(x, y).lightness());
                }
            }
            QVERIFY2(whitest > 240, qPrintable(QString::number(whitest)));
        }
    }

    void initTestCase()
    {
        Scales::quieter();
        Scales::bigScreen();
        QVERIFY(m_tmp.isValid());
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << "/usr/share/icons");
        if (QDir("/usr/share/icons/breeze").exists()) {
            QIcon::setThemeName("breeze");
        }
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
    }

    void iconLabel()
    {
        Scales::sweep([this](int n) {
            using namespace Scales;
            QStringList wrong;
            QWidget window;
            auto *layout = new QHBoxLayout(&window);
            QList<IconLabel *> labels;

            /* other offsets at each scale: other parts of a device pixel */
            layout->setContentsMargins(3 + n % 7, 2 + n % 5, 3, 3);
            for (int size : {16, 22, 32, 48}) {
                labels << new IconLabel(computer(), size);
                layout->addWidget(labels.last());
            }
            /* off its middle: the middle of a line of text it goes with */
            labels.last()->setFixedHeight(48 + 7);
            labels.last()->setCentre(48 / 2.0 + 2.3);
            settle(&window);
            const qreal ratio = window.devicePixelRatio();
            const QImage shot = grab(&window);
            const QRgb background = shot.pixel(0, 0);

            for (IconLabel *label : std::as_const(labels)) {
                const QImage icon = image(computer().pixmap(QSize(label->iconSize(),
                                                                  label->iconSize()), ratio));
                const QRect r = geometryIn(label, &window);
                const qreal centreY = label->height() == label->iconSize()
                                          ? r.top() + label->height() / 2.0
                                          : r.top() + 48 / 2.0 + 2.3;
                const QPointF middle((r.left() + r.width() / 2.0) * ratio, centreY * ratio);
                const QPoint guess(qRound(middle.x() - icon.width() / 2.0),
                                   qRound(middle.y() - icon.height() / 2.0));
                const Found found = find(shot, icon, background, guess, 2, 2);
                if (found.off > kCrisp) {
                    wrong << QString("%1: not its %2x%3 pixmap (off by %4)")
                                 .arg(label->iconSize()).arg(icon.width()).arg(icon.height())
                                 .arg(found.off, 0, 'f', 1);
                    continue;
                }
                const QPointF centre = QPointF(found.at) + QPointF(icon.width(), icon.height()) / 2;
                if (qAbs(centre.x() - middle.x()) > 0.5 + 1e-6 ||
                    qAbs(centre.y() - middle.y()) > 0.5 + 1e-6) {
                    wrong << QString("%1: middle at %2,%3, should be %4,%5")
                                 .arg(label->iconSize()).arg(centre.x()).arg(centre.y())
                                 .arg(middle.x()).arg(middle.y());
                }
            }

            /* rendered for another ratio, as on another screen: drawn for that one */
            IconLabel *label = labels.first();
            const qreal other = qFuzzyCompare(ratio, 2.0) ? 1.5 : 2.0;
            QImage target(QSize(label->width(), label->height()) * other,
                          QImage::Format_ARGB32_Premultiplied);
            target.setDevicePixelRatio(other);
            target.fill(Qt::white);
            /* on the white: without the window's background */
            label->render(&target, QPoint(), QRegion(), QWidget::RenderFlags());
            const QImage icon = image(computer().pixmap(QSize(16, 16), other));
            const Found found = find(target.convertToFormat(QImage::Format_RGB32), icon,
                                     qRgb(255, 255, 255),
                                     QPoint(qRound((label->width() * other - icon.width()) / 2),
                                            qRound((label->height() * other - icon.height()) / 2)),
                                     2, 2);
            if (found.off > kCrisp) {
                wrong << QString("rendered for %1: not its pixmap for it (off by %2)")
                             .arg(other).arg(found.off, 0, 'f', 1);
            }
            return wrong;
        });
    }

    /* The VM list's icons, at the size and ratio a view asks */
    void badges()
    {
        Scales::sweep([](int n) {
            QStringList wrong;
            const qreal ratio = n / 120.0;
            const QIcon base = computer();
            const QIcon badged = Icons::badged(base, Qt::green, Icons::Badge::Play);

            for (int size : {32, 48}) {
                const QImage plain = image(base.pixmap(QSize(size, size), ratio));
                const QImage with = image(badged.pixmap(QSize(size, size), ratio));
                if (with.size() != plain.size()) {
                    wrong << QString("%1: %2x%3, the icon is %4x%5").arg(size)
                                 .arg(with.width()).arg(with.height())
                                 .arg(plain.width()).arg(plain.height());
                    continue;
                }
                /* the icon as it is above the badge, which starts under 16.5/32 */
                int changed = 0;
                for (int y = 0; y < with.height() * 15 / 32; y++) {
                    for (int x = 0; x < with.width(); x++) {
                        changed += with.pixel(x, y) != plain.pixel(x, y);
                    }
                }
                if (changed > 0) {
                    wrong << QString("%1: %2 pixels of the icon above the badge changed").arg(size)
                                 .arg(changed);
                }
                /* and the badge there */
                const QPoint middle = QPointF(with.width() * (1 - 8.25 / 32),
                                              with.height() * (1 - 8.25 / 32)).toPoint();
                const QRgb green = with.pixel(middle + QPoint(qRound(-5 * ratio * size / 32), 0));
                if (qGreen(green) < 200 || qRed(green) > 80 || qBlue(green) > 80) {
                    wrong << QString("%1: no badge").arg(size);
                }
            }
            return wrong;
        });
    }

    /*
     * The VM's power buttons at the sizes of a menu's icons and of KDE's and
     * Fusion's toolbars, at each scale: the pixmap of the ratio's size;
     * Pause's bars and Force Off's square whole device pixels, Start's
     * straight edge too; the ink in the middle; colours that stand out at
     * 3:1 on Breeze's light and dark windows, the shape's edge on the light
     * one (the darker shade of the filled shapes); disabled, the colour of
     * disabled text, half transparent
     */
    void controls()
    {
        Scales::sweep([](int n) {
            const qreal ratio = n / 120.0;
            const QRgb light = qRgb(239, 240, 241), dark = qRgb(32, 35, 38);
            const QList<std::pair<Icons::Control, QString>> controls = {
                {Icons::Control::Start, "Start"}, {Icons::Control::Pause, "Pause"},
                {Icons::Control::ShutDown, "Shut Down"}, {Icons::Control::Reset, "Reset"},
                {Icons::Control::ForceOff, "Force Off"}};
            QStringList wrong;

            for (const auto &[kind, name] : controls) {
                const QIcon icon = Icons::control(kind);
                for (int size : {16, 22, 24}) {
                    const QString what = QString("%1 at %2").arg(name).arg(size);
                    const QImage pixels = image(icon.pixmap(QSize(size, size), ratio));
                    const int side = qRound(size * ratio);
                    if (pixels.size() != QSize(side, side)) {
                        wrong << QString("%1: %2x%3, not %4").arg(what).arg(pixels.width())
                                     .arg(pixels.height()).arg(side);
                        continue;
                    }
                    QRect ink;
                    bool partial = false;
                    qreal onLight = 0, onDark = 0;
                    for (int y = 0; y < side; y++) {
                        for (int x = 0; x < side; x++) {
                            const QRgb p = pixels.pixel(x, y);
                            if (qAlpha(p) > 0) {
                                ink |= QRect(x, y, 1, 1);
                            }
                            partial = partial || (qAlpha(p) > 0 && qAlpha(p) < 255);
                            if (qAlpha(p) == 255) {
                                onLight = qMax(onLight, contrast(p, light));
                                onDark = qMax(onDark, contrast(p, dark));
                            }
                        }
                    }
                    if (partial && (kind == Icons::Control::Pause ||
                                    kind == Icons::Control::ForceOff)) {
                        wrong << what + ": edges between device pixels";
                    }
                    /* Start: its straight edge, the first column of ink, opaque in its middle */
                    if (kind == Icons::Control::Start) {
                        for (int y = ink.top() + ink.height() / 4; y < ink.bottom() - ink.height() / 4;
                             y++) {
                            if (qAlpha(pixels.pixel(ink.left(), y)) != 255) {
                                wrong << what + ": its straight edge between device pixels";
                                break;
                            }
                        }
                    }
                    /* in the middle; Start a little right, where the eye puts it */
                    const qreal dx = (ink.left() + ink.right() + 1) / 2.0 - side / 2.0;
                    const qreal dy = (ink.top() + ink.bottom() + 1) / 2.0 - side / 2.0;
                    const qreal right = kind == Icons::Control::Start ? 0.08 * side : 0;
                    if (dx < -1 || dx > 1 + right || qAbs(dy) > 1) {
                        wrong << QString("%1: its ink %2,%3 from the middle").arg(what).arg(dx)
                                     .arg(dy);
                    }
                    if (ink.width() < side / 2 || ink.height() < side / 2) {
                        wrong << QString("%1: its ink %2x%3 only").arg(what).arg(ink.width())
                                     .arg(ink.height());
                    }
                    if (onLight < 3 || onDark < 3) {
                        wrong << QString("%1: %2:1 on light, %3:1 on dark").arg(what)
                                     .arg(onLight, 0, 'f', 1).arg(onDark, 0, 'f', 1);
                    }
                    /* the shape's edge on light: the first opaque pixel of the middle row */
                    for (int x = 0; x < side; x++) {
                        const QRgb p = pixels.pixel(x, side / 2);
                        if (qAlpha(p) == 255) {
                            if (contrast(p, light) < 3) {
                                wrong << QString("%1: its edge %2:1 on light").arg(what)
                                             .arg(contrast(p, light), 0, 'f', 1);
                            }
                            break;
                        }
                    }
                    /* disabled: the colour of disabled text, half transparent */
                    const QRgb text = QGuiApplication::palette()
                                          .color(QPalette::Disabled, QPalette::ButtonText).rgb();
                    const QImage off = image(icon.pixmap(QSize(size, size), ratio, QIcon::Disabled));
                    int most = 0;
                    for (int y = 0; y < off.height(); y++) {
                        for (int x = 0; x < off.width(); x++) {
                            const QRgb p = off.pixel(x, y);
                            most = qMax(most, qAlpha(p));
                            /* the whole pixels: an edge's, halved, is rounded too much */
                            if (qAlpha(p) >= 126 && Scales::difference(qUnpremultiply(p), text) > 6) {
                                wrong << what + ": not the colour of disabled text, disabled";
                                y = off.height();
                                break;
                            }
                        }
                    }
                    if (qAbs(most - 128) > 1) {
                        wrong << QString("%1: %2 opaque at most, disabled").arg(what).arg(most);
                    }
                }
            }
            return wrong;
        });
    }

    /*
     * The list of the settings pages, all of them in it: as wide as their
     * longest name and as high as its rows at every scale, in Breeze as in
     * Fusion - no row cut or out of sight, none wider than the list
     */
    void pageList_data()
    {
        sizes_data();
    }

    void pageList()
    {
        QFETCH(QString, style);
        QApplication::setStyle(style);
        QFile args(m_tmp.filePath("pagelist/vm.args"));
        QVERIFY(QDir().mkpath(m_tmp.filePath("pagelist")));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-m 1G\n") > 0);
        args.close();
        Vm vm(m_tmp.filePath("pagelist"));

        /* made once: shown, then its window destroyed, at each scale */
        struct Pane : VmPane {
            using QWidget::destroy;
        } pane;
        pane.setVm(&vm);
        pane.setTab(VmPane::Settings);
        auto *pages = pane.findChild<QListWidget *>("pages");
        QVERIFY(pages);
        QCOMPARE(pages->count(), int(VmPane::Arguments) + 1);
        pane.resize(900, 700);

        Scales::sweep([&](int) {
            using namespace Scales;
            QStringList wrong;
            settle(&pane);
            const QRect viewport = pages->viewport()->rect();
            const int widest = pages->sizeHintForColumn(0);

            if (widest > viewport.width()) {
                wrong << QString("rows %1 wide, the list %2").arg(widest).arg(viewport.width());
            }
            for (int i = 0; i < pages->count(); i++) {
                const QRect row = pages->visualItemRect(pages->item(i));
                if (row.isEmpty() || !viewport.contains(row)) {
                    wrong << QString("%1 at %2,%3 %4x%5, out of the list's %6x%7")
                                 .arg(pages->item(i)->text()).arg(row.x()).arg(row.y())
                                 .arg(row.width()).arg(row.height()).arg(viewport.width())
                                 .arg(viewport.height());
                }
            }
            if (pages->verticalScrollBar()->maximum() > 0) {
                wrong << QString("the list scrolls by %1").arg(pages->verticalScrollBar()->maximum());
            }
            pane.hide();
            pane.destroy();
            return wrong;
        });
    }

    /* The buttons of a row or a column, the settings pages' and the tabs': icons on all or none */
    void buttonGroups()
    {
        QFile args(m_tmp.filePath("groups/vm.args"));
        QVERIFY(QDir().mkpath(m_tmp.filePath("groups")));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-m 1G\n") > 0);
        args.close();
        Vm vm(m_tmp.filePath("groups"));
        VmPane pane;
        QStringList wrong;

        pane.setVm(&vm);
        for (QLayout *layout : pane.findChildren<QLayout *>()) {
            QStringList with, without;
            for (int i = 0; i < layout->count(); i++) {
                if (auto *button = qobject_cast<QPushButton *>(layout->itemAt(i)->widget())) {
                    (button->icon().isNull() ? without : with) << button->text().remove('&');
                }
            }
            if (!with.isEmpty() && !without.isEmpty()) {
                wrong << QString("icons on %1, not on %2").arg(with.join(", "), without.join(", "));
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join("; ")));
    }

    /* Each role's icons at the style's size for it, in Breeze as in Fusion */
    void sizes_data()
    {
        QTest::addColumn<QString>("style");
        QTest::newRow("fusion") << "Fusion";
        if (QStyleFactory::keys().contains("Breeze", Qt::CaseInsensitive)) {
            QTest::newRow("breeze") << "Breeze";
        }
    }

    void sizes()
    {
        QFETCH(QString, style);
        QApplication::setStyle(style);
        const int small = QApplication::style()->pixelMetric(QStyle::PM_SmallIconSize);

        /* the notices of the status bar, the memory warning among them */
        VmStore store(m_tmp.filePath("vms"));
        QWidget window;
        MemoryMonitor memory(&store, &window);
        QToolButton *notice = Widgets::statusButton("notice", computer(), "Notice");
        for (QToolButton *button : {memory.button(), notice}) {
            QCOMPARE(button->iconSize(), QSize(small, small));
            QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
            QVERIFY(button->autoRaise());
        }
        delete notice;

        /* the list of the settings pages */
        VmPane pane;
        const int toolbar = QApplication::style()->pixelMetric(QStyle::PM_ToolBarIconSize);
        auto *pages = pane.findChild<QListWidget *>("pages");
        QVERIFY(pages);
        QCOMPARE(pages->iconSize(), QSize(toolbar, toolbar));
    }
};

QTEST_MAIN(TestIcons)
#include "test_icons.moc"
