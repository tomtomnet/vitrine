// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPainter>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QToolButton>

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
 * asked, not scaled from pixmaps made for 1, 2 and 3.  The icons of a
 * role have the style's size for it.
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

private slots:
    void initTestCase()
    {
        Scales::quieter();
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
        for (const char *name : {"pages", "advancedPages"}) {
            auto *list = pane.findChild<QListWidget *>(name);
            QVERIFY(list);
            QCOMPARE(list->iconSize(), QSize(toolbar, toolbar));
        }
    }
};

QTEST_MAIN(TestIcons)
#include "test_icons.moc"
