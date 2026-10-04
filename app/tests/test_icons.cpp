// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
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
     * Advanced, under the settings pages, in line with them: its name where
     * a row of the list has its name, its arrow in the column of their icons
     */
    void sectionRow_data()
    {
        sizes_data();
    }

    void sectionRow()
    {
        QFETCH(QString, style);
        QApplication::setStyle(style);
        QFile args(m_tmp.filePath("section/vm.args"));
        QVERIFY(QDir().mkpath(m_tmp.filePath("section")));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-m 1G\n") > 0);
        args.close();
        Vm vm(m_tmp.filePath("section"));

        /* made once: shown, then its window destroyed, at each scale */
        struct Pane : VmPane {
            using QWidget::destroy;
        } pane;
        pane.setVm(&vm);
        pane.setTab(VmPane::Settings);
        auto *pages = pane.findChild<QListWidget *>("pages");
        auto *advanced = pane.findChild<QToolButton *>("advanced");
        QVERIFY(pages && advanced);
        /* a row of the same name in the list, to compare with */
        pages->addItem(new QListWidgetItem(computer(), advanced->text()));
        pages->setFixedHeight(pages->height() + pages->sizeHintForRow(0) * 2);
        pane.resize(900, 700);

        Scales::sweep([&](int) {
            using namespace Scales;
            QStringList wrong;
            settle(&pane);
            const qreal ratio = pane.devicePixelRatio();
            const QImage shot = grab(&pane);
            const QRect list = geometryIn(pages, &pane);
            const QRgb background = shot.pixel(qRound(list.right() * ratio) - 2,
                                               qRound(list.bottom() * ratio) - 2);

            /* the runs of columns with ink in the device rows of @r */
            auto inkRuns = [&](const QRect &r) {
                QList<std::pair<int, int>> runs;
                const int y0 = qCeil(r.top() * ratio), y1 = qFloor((r.bottom() + 1) * ratio) - 1;
                for (int x = qCeil(r.left() * ratio); x < qFloor((r.right() + 1) * ratio); x++) {
                    bool ink = false;
                    for (int y = y0; y <= y1 && !ink; y++) {
                        ink = difference(shot.pixel(x, y), background) > 90;
                    }
                    if (!ink) {
                        continue;
                    }
                    /* a gap of two columns or more parts the icon from the name */
                    if (!runs.isEmpty() && x <= runs.last().second + 2) {
                        runs.last().second = x;
                    } else {
                        runs << std::pair(x, x);
                    }
                }
                return runs;
            };
            const QRect last = pages->visualItemRect(pages->item(pages->count() - 1))
                                   .translated(geometryIn(pages->viewport(), &pane).topLeft());
            const auto row = inkRuns(last);
            const auto button = inkRuns(geometryIn(advanced, &pane));
            /* icon (or arrow), then the name's letters */
            if (row.size() < 2 || button.size() < 2) {
                wrong << QString("%1 runs of ink in the row, %2 in Advanced").arg(row.size())
                             .arg(button.size());
            } else {
                if (qAbs(row[1].first - button[1].first) > 1) {
                    wrong << QString("Advanced's name at %1, a row's at %2 (device pixels)")
                                 .arg(button[1].first).arg(row[1].first);
                }
                if (button[0].first < row[0].first || button[0].second > row[0].second) {
                    wrong << QString("Advanced's arrow at %1-%2, the icon at %3-%4")
                                 .arg(button[0].first).arg(button[0].second)
                                 .arg(row[0].first).arg(row[0].second);
                }
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
        for (const char *name : {"pages", "advancedPages"}) {
            auto *list = pane.findChild<QListWidget *>(name);
            QVERIFY(list);
            QCOMPARE(list->iconSize(), QSize(toolbar, toolbar));
        }
    }
};

QTEST_MAIN(TestIcons)
#include "test_icons.moc"
