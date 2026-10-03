// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

/*
 * The value of a QEMU option in QemuOpts syntax: comma-separated items,
 * "key=value" or a bare word, with ",," standing for a literal comma.  A
 * bare first item is the value of the option's implied key: the driver of
 * -device, the size of -m, the model of -cpu, the type of -machine.
 *
 * Editing keeps the items in order and writes back everything that was not
 * touched as it was, so hand-written options survive the settings pages.
 */
class OptionValue
{
public:
    struct Item {
        QString key;        // empty for the implied first value
        QString value;
        bool bare = false;  // a bare word after the first one, e.g. +avx
    };

    OptionValue() = default;
    explicit OptionValue(const QString &text);

    QString implied() const;
    void setImplied(const QString &value);

    bool has(const QString &key) const;
    QString get(const QString &key, const QString &fallback = {}) const;
    /* on/off, yes/no, true/false; @fallback when absent or unknown */
    bool flag(const QString &key, bool fallback = false) const;
    void set(const QString &key, const QString &value);
    void setFlag(const QString &key, bool on);
    void remove(const QString &key);

    QString toString() const;
    const QList<Item> &items() const { return m_items; }
    bool isEmpty() const { return m_items.isEmpty(); }

    static QString escape(const QString &text);

private:
    QList<Item> m_items;
};
