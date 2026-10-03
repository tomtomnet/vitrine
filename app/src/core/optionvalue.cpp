// SPDX-License-Identifier: GPL-2.0-or-later
#include "optionvalue.h"

#include <QStringList>

/* Split at commas, where ",," is a comma inside an item */
static QStringList splitItems(const QString &text)
{
    QStringList items;
    QString cur;

    for (qsizetype i = 0; i < text.size(); i++) {
        if (text[i] != ',') {
            cur += text[i];
        } else if (i + 1 < text.size() && text[i + 1] == ',') {
            cur += ',';
            i++;
        } else {
            items << cur;
            cur.clear();
        }
    }
    items << cur;
    return items;
}

OptionValue::OptionValue(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    const QStringList parts = splitItems(text);
    for (qsizetype i = 0; i < parts.size(); i++) {
        const QString &part = parts[i];
        qsizetype eq = part.indexOf('=');

        if (eq >= 0) {
            m_items.append({part.left(eq), part.mid(eq + 1), false});
        } else if (i == 0) {
            m_items.append({QString(), part, false});
        } else {
            m_items.append({part, QString(), true});
        }
    }
}

QString OptionValue::implied() const
{
    if (!m_items.isEmpty() && m_items[0].key.isEmpty() && !m_items[0].bare) {
        return m_items[0].value;
    }
    return {};
}

void OptionValue::setImplied(const QString &value)
{
    if (!m_items.isEmpty() && m_items[0].key.isEmpty() && !m_items[0].bare) {
        m_items[0].value = value;
    } else {
        m_items.prepend({QString(), value, false});
    }
}

bool OptionValue::has(const QString &key) const
{
    for (const Item &item : m_items) {
        if (item.key == key) {
            return true;
        }
    }
    return false;
}

QString OptionValue::get(const QString &key, const QString &fallback) const
{
    for (const Item &item : m_items) {
        if (item.key == key && !item.bare) {
            return item.value;
        }
    }
    return fallback;
}

bool OptionValue::flag(const QString &key, bool fallback) const
{
    const QString value = get(key).toLower();

    if (value == "on" || value == "yes" || value == "true") {
        return true;
    }
    if (value == "off" || value == "no" || value == "false") {
        return false;
    }
    return fallback;
}

void OptionValue::set(const QString &key, const QString &value)
{
    for (Item &item : m_items) {
        if (item.key == key) {
            item.value = value;
            item.bare = false;
            return;
        }
    }
    m_items.append({key, value, false});
}

void OptionValue::setFlag(const QString &key, bool on)
{
    set(key, on ? "on" : "off");
}

void OptionValue::remove(const QString &key)
{
    m_items.removeIf([&](const Item &item) { return item.key == key; });
}

QString OptionValue::escape(const QString &text)
{
    QString out = text;
    return out.replace(',', ",,");
}

QString OptionValue::toString() const
{
    QStringList parts;

    for (const Item &item : m_items) {
        if (item.key.isEmpty()) {
            parts << escape(item.value);
        } else if (item.bare) {
            parts << escape(item.key);
        } else {
            parts << escape(item.key) + '=' + escape(item.value);
        }
    }
    return parts.join(',');
}
