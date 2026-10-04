// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * The UI tests at every scale a Wayland desktop can have.  The compositor
 * gives the scale as n/120 (wp_fractional_scale_v1) and Qt 6 makes it the
 * windows' device pixel ratio as it is, so these are all of them from 1 to
 * 3; below 1 Qt draws at 1.  A test makes, shows and grabs its windows at
 * each ratio, set through Qt's global factor as QT_SCALE_FACTOR sets it at
 * start; with QT_SCALE_FACTOR set, at that one only (ctest runs a few that
 * way too, to check the two agree).
 *
 * VITRINE_SCALES="150 156" checks only those.
 */

#include <QApplication>
#include <QImage>
#include <QMap>
#include <QRegularExpression>
#include <QTest>
#include <QWidget>

#include <private/qhighdpiscaling_p.h>

#include <cmath>
#include <functional>

namespace Scales {

inline QList<int> list()
{
    QList<int> list;

    if (qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        return {int(std::lround(qEnvironmentVariable("QT_SCALE_FACTOR").toDouble() * 120))};
    }
    for (const QString &n : qEnvironmentVariable("VITRINE_SCALES").split(' ', Qt::SkipEmptyParts)) {
        list << n.toInt();
    }
    if (list.isEmpty()) {
        for (int n = 120; n <= 360; n++) {
            list << n;
        }
    }
    return list;
}

/* The ratio of the windows made from now on, n/120: no window may exist */
inline void set(int n)
{
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        QHighDpiScaling::setGlobalFactor(n / 120.0);
    }
}

/* @window shown, and laid out as far as its layouts go */
inline void settle(QWidget *window)
{
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    /* the layouts' requests, some of which make more */
    for (int i = 0; i < 4; i++) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    }
}

/* The window, as the screen shows it, in device pixels */
inline QImage grab(QWidget *window)
{
    return window->grab().toImage().convertToFormat(QImage::Format_RGB32);
}

inline QRect geometryIn(const QWidget *widget, const QWidget *window)
{
    return QRect(widget->mapTo(window, QPoint(0, 0)), widget->size());
}

inline int difference(QRgb a, QRgb b)
{
    return qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b));
}

/* @src (premultiplied) over @dst, as Qt's raster engine blends */
inline QRgb over(QRgb src, QRgb dst)
{
    const int a = 255 - qAlpha(src);
    auto mul = [a](int c) {
        const int t = c * a;
        return (t + (t >> 8) + 0x80) >> 8;
    };
    return qRgb(qRed(src) + mul(qRed(dst)), qGreen(src) + mul(qGreen(dst)),
                qBlue(src) + mul(qBlue(dst)));
}

/* Where @icon (premultiplied) is drawn in @shot, over @background, around
   @guess: the best place, and how far off it is there (per channel) */
struct Found {
    QPoint at;
    double off = 1e9;
};

inline Found find(const QImage &shot, const QImage &icon, QRgb background, QPoint guess,
                  int dx, int dy)
{
    Found found;

    for (int y0 = guess.y() - dy; y0 <= guess.y() + dy; y0++) {
        for (int x0 = guess.x() - dx; x0 <= guess.x() + dx; x0++) {
            if (x0 < 0 || y0 < 0 || x0 + icon.width() > shot.width() ||
                y0 + icon.height() > shot.height()) {
                continue;
            }
            qint64 sum = 0;
            for (int y = 0; y < icon.height(); y++) {
                for (int x = 0; x < icon.width(); x++) {
                    sum += difference(shot.pixel(x0 + x, y0 + y),
                                      over(icon.pixel(x, y), background));
                }
            }
            const double off = double(sum) / (icon.width() * icon.height() * 3);
            if (off < found.off) {
                found = {QPoint(x0, y0), off};
            }
        }
    }
    return found;
}

/* An icon drawn as its pixmap is but for the blending's rounding: a scaled
   one, or one between device pixels, is off by far more */
constexpr double kCrisp = 1.5;

/*
 * @check at each scale.  What fails is told by kind (its numbers left
 * out) with the scales it fails at, then the first failures in full.
 */
inline void sweep(const std::function<QStringList(int)> &check)
{
    static const QRegularExpression numbers("[-0-9.]+");
    QStringList failures;
    QMap<QString, QList<int>> kinds;
    const QList<int> scales = list();

    for (int n : scales) {
        set(n);
        for (const QString &wrong : check(n)) {
            failures << QString("%1 (n=%2): %3").arg(n / 120.0, 0, 'f', 4).arg(n).arg(wrong);
            QList<int> &at = kinds[QString(wrong).replace(numbers, "#")];
            if (at.isEmpty() || at.last() != n) {
                at << n;
            }
        }
        QVERIFY2(QApplication::topLevelWidgets().isEmpty(), "a window left over");
    }
    set(120);
    for (auto it = kinds.cbegin(); it != kinds.cend(); ++it) {
        QStringList at;
        for (int n : it.value().mid(0, 12)) {
            at << QString::number(n / 120.0, 'g', 4);
        }
        qWarning("at %lld of %lld scales (%s%s): %s", qint64(it.value().size()),
                 qint64(scales.size()), qPrintable(at.join(' ')),
                 it.value().size() > 12 ? " ..." : "", qPrintable(it.key()));
    }
    for (const QString &failure : failures.mid(0, 10)) {
        qWarning("%s", qPrintable(failure));
    }
    QVERIFY2(failures.isEmpty(), qPrintable(QString("%1 failures over %2 scales")
                                                .arg(failures.size()).arg(scales.size())));
}

inline QtMessageHandler &previousHandler()
{
    static QtMessageHandler handler = nullptr;
    return handler;
}

/* The offscreen platform tells, for each window Breeze sizes, that it cannot */
inline void quieter()
{
    previousHandler() = qInstallMessageHandler(
        [](QtMsgType type, const QMessageLogContext &context, const QString &message) {
            if (!message.contains("does not support propagateSizeHints") && previousHandler()) {
                previousHandler()(type, context, message);
            }
        });
}

}
