// SPDX-License-Identifier: GPL-2.0-or-later
#include "clipboardstate.h"

#include <QMimeData>

namespace {

/* "text/plain; charset=UTF-8" -> "text/plain;charset=utf-8" */
QString normalized(const QString &mime)
{
    QString m = mime.toLower();
    m.remove(QLatin1Char(' '));
    return m;
}

} // namespace

QStringList ClipboardMime::textMimes()
{
    return {QLatin1String(kText), QStringLiteral("text/plain"), QStringLiteral("UTF8_STRING")};
}

bool ClipboardMime::isText(const QString &mime)
{
    const QString m = normalized(mime);
    return m == QLatin1String(kText) || m == QLatin1String("text/plain") ||
           m == QLatin1String("utf8_string");
}

QString ClipboardMime::pickText(const QStringList &mimes)
{
    for (const QString &mime : mimes) {
        if (isText(mime)) {
            return mime;
        }
    }
    return QString();
}

QString ClipboardMime::textOf(const QMimeData *data)
{
    QString text;

    if (!data) {
        return text;
    }
    /* Qt's own reading first: it knows the platform's names for text */
    if (data->hasText()) {
        text = data->text();
    }
    if (text.isEmpty()) {
        const QString mime = pickText(data->formats());
        if (!mime.isEmpty()) {
            text = QString::fromUtf8(data->data(mime));
        }
    }
    /* some X11 clients count the terminating NUL */
    while (text.endsWith(QChar(0))) {
        text.chop(1);
    }
    return text;
}

void ClipboardState::reset()
{
    m_next = 0;
}

ClipboardState::HostAction ClipboardState::hostChanged(const QString &text, bool again,
                                                       uint32_t *serial)
{
    if (text.isEmpty()) {
        /* no text now, e.g. an image: what the guest was offered is gone */
        m_hostText.clear();
        if (m_owner == Owner::Host) {
            m_owner = Owner::None;
            m_offered.clear();
            return HostAction::Release;
        }
        return HostAction::None;
    }
    /*
     * The same text again: Wayland tells the clipboard each time vitrine
     * gets the keyboard, the guest's own text comes back from the host
     * clipboard once put there, and Plasma's clipboard history may put it
     * there again.  Offering it would take the clipboard from the guest.
     */
    if (!again && text == m_hostText) {
        return HostAction::None;
    }
    m_hostText = text;
    m_offered = text;
    m_owner = Owner::Host;
    *serial = m_next++;
    return HostAction::Grab;
}

void ClipboardState::hostSeen(const QString &text)
{
    m_hostText = text;
}

bool ClipboardState::guestGrab(uint32_t serial, const QStringList &mimes)
{
    /* below the next one: our grab of the same serial won the race (the
       guest drops its own then too), or one we saw already */
    if (serial < m_next) {
        return false;
    }
    m_next = serial + 1;
    m_owner = Owner::Guest;
    m_guestSerial = serial;
    m_guestText = !ClipboardMime::pickText(mimes).isEmpty();
    m_offered.clear();
    return m_guestText;
}

void ClipboardState::guestRelease()
{
    /* the host clipboard keeps the text it got */
    if (m_owner == Owner::Guest) {
        m_owner = Owner::None;
    }
}

std::optional<QString> ClipboardState::guestText(uint32_t serial, const QString &text)
{
    if (m_owner != Owner::Guest || serial != m_guestSerial || text.isEmpty() ||
        text == m_hostText) {
        return std::nullopt;
    }
    return text;
}

bool ClipboardState::request(const QStringList &mimes, QString *mime, QByteArray *data) const
{
    const QString text = ClipboardMime::pickText(mimes);

    if (text.isEmpty()) {
        return false;
    }
    *mime = text;
    *data = m_offered.toUtf8();
    return true;
}

std::optional<uint32_t> ClipboardState::guestSerial() const
{
    if (m_owner == Owner::Guest && m_guestText) {
        return m_guestSerial;
    }
    return std::nullopt;
}
