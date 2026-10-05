// SPDX-License-Identifier: GPL-2.0-or-later
#include "systems.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QRegularExpression>

#include <cmath>

#include "ui/icons.h"

namespace Systems {

namespace {

/* A brand: its systems (the distributions as libosinfo names them), its
   colour, the letter drawn when its logo is not bundled, and more names
   of its logo in icon themes than distributor-logo-KEY */
struct Logo {
    const char *key;
    const char *distros;
    QRgb color;
    const char *letter;
    bool bundled;
    const char *theme;
};

const Logo kLogos[] = {
    {"fedora", "fedora", 0x51a2da, "f", false, "fedora-logo-icon"},
    {"archlinux", "archlinux", 0x1793d1, "A", true, "archlinux-logo"},
    {"debian", "debian", 0xa81d33, "D", false, "debian-logo"},
    {"ubuntu", "ubuntu", 0xe95420, "U", true, "ubuntu-logo"},
    {"opensuse", "opensuse", 0x73ba25, "S", true, ""},
    {"linuxmint", "linuxmint", 0x86be43, "M", true, ""},
    {"manjaro", "manjaro", 0x35bfa4, "M", true, ""},
    {"endeavouros", "endeavouros", 0x7f7fff, "E", true, ""},
    {"cachyos", "cachyos", 0x00aa88, "C", true, ""},
    {"kde", "kdelinux,kdeneon", 0x1d99f3, "K", true, "distributor-logo-kde-neon"},
    {"popos", "popos", 0x48b9c7, "P", true, "distributor-logo-pop-os"},
    {"elementary", "elementaryos", 0x64baff, "e", true, ""},
    {"zorin", "zorin", 0x15a6f0, "Z", true, ""},
    {"kalilinux", "kalilinux", 0x557c94, "K", true, "distributor-logo-kali"},
    {"redhat", "rhel,rhl", 0xee0000, "R", true, ""},
    {"centos", "centos", 0x262577, "C", true, ""},
    {"almalinux", "almalinux", 0x000000, "A", true, "distributor-logo-alma"},
    {"rockylinux", "rocky", 0x10b981, "R", false, "distributor-logo-rocky"},
    {"suse", "sle,sled,sles,slem,caasp", 0x0c322c, "S", true, ""},
    {"alpinelinux", "alpinelinux", 0x0d597f, "A", true, "distributor-logo-alpine"},
    {"nixos", "nixos", 0x5277c3, "N", false, "distributor-logo-nixos"},
    {"gentoo", "gentoo", 0x54487a, "g", false, ""},
    {"voidlinux", "voidlinux", 0x478061, "V", true, "distributor-logo-void"},
    {"slackware", "slackware", 0x000000, "S", true, ""},
    {"freebsd", "freebsd", 0xab2b28, "F", true, ""},
    {"openbsd", "openbsd", 0xf2ca30, "O", true, ""},
    {"netbsd", "netbsd", 0xff6600, "N", true, ""},
    {"apple", "osx", 0x000000, "A", true, "distributor-logo-macos"},
    {"windows", "win,winnt", 0x0078d4, "W", true, ""},
    /* the family's, for the systems with none of their own */
    {"linux", "", 0xfcc624, "L", true, ""},
};

const Logo *logo(const QString &key)
{
    for (const Logo &l : kLogos) {
        if (key == QLatin1String(l.key)) {
            return &l;
        }
    }
    return nullptr;
}

/* As WCAG computes it: a dark logo on a light square */
bool light(const QColor &c)
{
    auto linear = [](qreal v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF()) >
           0.45;
}

/*
 * The logo, or the letter, on a square of the brand's colour with round
 * corners, drawn for each size and pixel ratio asked: on whole device
 * pixels, as Icons::badged() draws its badge
 */
class TileEngine : public QIconEngine
{
public:
    TileEngine(const QString &key, const QColor &color, const QString &letter, bool bundled)
        : m_key(key), m_color(color), m_letter(letter),
          m_logo(bundled ? QIcon(":/os/" + key + ".svg") : QIcon())
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override
    {
        const qreal ratio = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, ratio));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State,
                         qreal scale) override
    {
        const QSize device(qRound(size.width() * scale), qRound(size.height() * scale));
        const int side = qMin(device.width(), device.height());
        const QString cached = QString("vitrine-os/%1/%2x%3/%4/%5")
                                   .arg(m_key)
                                   .arg(device.width())
                                   .arg(device.height())
                                   .arg(scale)
                                   .arg(int(mode));
        QPixmap pixmap;

        if (side <= 0) {
            return {};
        }
        if (QPixmapCache::find(cached, &pixmap)) {
            return pixmap;
        }
        pixmap = QPixmap(device);
        pixmap.fill(Qt::transparent);
        {
            QPainter p(&pixmap);
            /* as designed at 32 pixels: a margin of 1, corners of 7, the logo 20 */
            const qreal unit = side / 32.0;
            const QRectF square = QRectF((device.width() - side) / 2.0,
                                         (device.height() - side) / 2.0, side, side)
                                      .adjusted(unit, unit, -unit, -unit);
            QColor color = m_color;

            if (mode == QIcon::Disabled) {
                color = QColor::fromHslF(0, 0, qMin(color.lightnessF() + 0.25, 0.8));
            }
            const QColor ink = light(color) ? QColor(0x23, 0x26, 0x29) : QColor(Qt::white);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawRoundedRect(square, 7 * unit, 7 * unit);

            const int logo = qRound(20 * unit);
            QImage drawn = m_logo.isNull() || logo <= 0
                               ? QImage()
                               : m_logo.pixmap(QSize(logo, logo), 1.0).toImage();
            if (!drawn.isNull()) {
                /* the logo's shape in the ink, on whole device pixels */
                drawn = drawn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                QPainter tint(&drawn);
                tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
                tint.fillRect(drawn.rect(), ink);
                tint.end();
                p.drawImage(QPointF(qRound(square.center().x() - drawn.width() / 2.0),
                                    qRound(square.center().y() - drawn.height() / 2.0)),
                            drawn);
            } else {
                QFont font = QGuiApplication::font();
                font.setBold(true);
                font.setPixelSize(qMax(1, qRound(22 * unit)));
                p.setFont(font);
                p.setPen(ink);
                p.drawText(square, Qt::AlignCenter, m_letter);
            }
        }
        pixmap.setDevicePixelRatio(scale);
        QPixmapCache::insert(cached, pixmap);
        return pixmap;
    }

    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override
    {
        return size;
    }

    QIconEngine *clone() const override { return new TileEngine(*this); }

    QString key() const override { return QStringLiteral("vitrine-os"); }

private:
    QString m_key;
    QColor m_color;
    QString m_letter;
    QIcon m_logo;
};

QHash<QString, QIcon> &icons()
{
    static QHash<QString, QIcon> icons;
    return icons;
}

QIcon computer()
{
    static const QIcon icon = Icons::themed({"computer"}, QStyle::SP_ComputerIcon);
    return icon;
}

}

QString key(const GuestOs::Os &os, const QString &family)
{
    const QString f = os.isNull() ? family : GuestOs::guestFamily(os);

    if (!os.distro.isEmpty()) {
        for (const Logo &l : kLogos) {
            if (QString::fromLatin1(l.distros).split(',').contains(os.distro)) {
                return QLatin1String(l.key);
            }
        }
    }
    if (f == "linux" || f == "windows") {
        return f;
    }
    return {};
}

QStringList themeNames(const QString &key)
{
    const Logo *l = logo(key);
    QStringList names;

    /* a family's logo is not a distribution's: the theme's distributor-logo
       is this computer's, as linux would be its kernel's */
    if (!l || key == "linux") {
        return names;
    }
    names << "distributor-logo-" + key;
    for (const QString &name : QString::fromLatin1(l->theme).split(',', Qt::SkipEmptyParts)) {
        names << name;
    }
    return names;
}

QIcon icon(const GuestOs::Os &os, const QString &family)
{
    const QString k = key(os, family);
    const Logo *l = logo(k);

    if (!l) {
        return computer();
    }
    /* the same icon for the same logo: the list's badges are made once per icon */
    if (const auto it = icons().constFind(k); it != icons().cend()) {
        return *it;
    }
    QIcon made;
    for (const QString &name : themeNames(k)) {
        if (QIcon::hasThemeIcon(name)) {
            made = QIcon::fromTheme(name);
            break;
        }
    }
    if (made.isNull()) {
        made = QIcon(new TileEngine(k, QColor::fromRgb(l->color), QLatin1String(l->letter),
                                    l->bundled));
    }
    icons().insert(k, made);
    return made;
}

void reloadIcons()
{
    icons().clear();
}

QIcon icon(const VmConfig::Guest &guest)
{
    return icon(GuestOs::Catalogue::instance().find(guest.id), guest.os);
}

QString osName(const GuestOs::Os &os)
{
    if (os.isGeneric()) {
        /* "Fedora", "Red Hat Enterprise Linux 9 Unknown" */
        return QCoreApplication::translate("Systems", "%1 (another release)")
            .arg(QString(os.name).remove(QRegularExpression(" [Uu]nknown$")));
    }
    return os.name;
}

QString name(const VmConfig::Guest &guest)
{
    const GuestOs::Os os = GuestOs::Catalogue::instance().find(guest.id);

    if (!os.isNull()) {
        return osName(os);
    }
    if (!guest.id.isEmpty()) {
        return guest.id;
    }
    if (guest.os == "linux") {
        return QCoreApplication::translate("Systems", "Linux");
    }
    if (guest.os == "windows") {
        return QCoreApplication::translate("Systems", "Windows");
    }
    if (guest.os == "other") {
        return QCoreApplication::translate("Systems", "Another system");
    }
    return {};
}

QString desktopName(const QString &desktop)
{
    if (desktop == "kde") {
        return QCoreApplication::translate("Systems", "KDE Plasma");
    }
    if (desktop == "gnome") {
        return QCoreApplication::translate("Systems", "GNOME");
    }
    if (desktop == "other") {
        return QCoreApplication::translate("Systems", "another desktop, or none");
    }
    return desktop;
}

}
