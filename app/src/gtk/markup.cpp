// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/markup.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace Markup {

namespace {

/* What is being written: the markup, and the breaks the next text owes */
struct Writer {
    QString out;
    int pre = 0;            // inside <pre>: the text as it is
    int lists = 0;          // the lists it is in
    int owed = 0;           // the line breaks owed before what comes next
    bool cellBefore = false;

    /* @lines line breaks before what comes next, once there is something before it */
    void block(int lines)
    {
        owed = std::max(owed, lines);
    }
    /* What is owed, before text or an inline tag */
    void settle()
    {
        if (owed == 0 || out.isEmpty()) {
            owed = 0;
            return;
        }
        while (out.endsWith(' ')) {
            out.chop(1);
        }
        int have = 0;
        for (qsizetype i = out.size() - 1; i >= 0 && out[i] == '\n'; i--) {
            have++;
        }
        out += QString(std::max(0, owed - have), '\n');
        owed = 0;
    }
    void text(QString text)
    {
        static const QRegularExpression spaces("\\s+");
        static const QRegularExpression ampersand("&(?!#?\\w+;)");

        /* Qt's entities that XML has not; a lone & (none of toHtmlEscaped()'s) escaped */
        text.replace("&nbsp;", "&#160;");
        text.replace(ampersand, "&amp;");
        if (pre == 0) {
            text.replace(spaces, " ");
            if (text == " " && (out.isEmpty() || out.endsWith('\n') || owed > 0)) {
                return;
            }
        }
        if (text.isEmpty()) {
            return;
        }
        settle();
        if (pre == 0 && (out.isEmpty() || out.endsWith('\n')) && text.startsWith(' ')) {
            text.remove(0, 1);
        }
        out += text;
    }
    void inlineTag(const QString &tag)
    {
        settle();
        out += tag;
    }
};

}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

QString pango(const QString &html)
{
    static const QRegularExpression tagPattern("<(/?)([A-Za-z][A-Za-z0-9]*)((?:\\s[^>]*?)?)\\s*/?>");
    static const QRegularExpression href("href\\s*=\\s*(\"[^\"]*\"|'[^']*')");
    Writer w;
    qsizetype from = 0;

    for (auto it = tagPattern.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const bool closing = !m.captured(1).isEmpty();
        const QString name = m.captured(2).toLower();

        w.text(html.mid(from, m.capturedStart() - from));
        from = m.capturedEnd();

        if (name == "p" || name == "div") {
            w.block(name == "p" ? 2 : 1);
        } else if (name == "ul" || name == "ol") {
            /* a list in an item: on the next line, not after an empty one */
            w.lists = std::max(0, w.lists + (closing ? -1 : 1));
            w.block((closing ? w.lists > 0 : w.lists > 1) ? 1 : 2);
        } else if (name == "li") {
            w.block(1);
            if (!closing) {
                w.settle();
                w.out += QString(4 * std::max(0, w.lists - 1), ' ') + QStringLiteral("• ");
            }
        } else if (name == "pre") {
            w.block(2);
            if (closing) {
                w.out += "</tt>";
                w.pre = std::max(0, w.pre - 1);
            } else {
                w.settle();
                w.out += "<tt>";
                w.pre++;
            }
        } else if (name == "br") {
            w.out += '\n';
        } else if (QStringList{"b", "i", "u", "s", "tt", "sub", "sup", "small", "big"}.contains(name)) {
            w.inlineTag('<' + QString(closing ? "/" : "") + name + '>');
        } else if (name == "strong" || name == "em") {
            w.inlineTag('<' + QString(closing ? "/" : "") + (name == "strong" ? "b" : "i") + '>');
        } else if (name == "code" || name == "kbd" || name == "samp") {
            w.inlineTag(closing ? "</tt>" : "<tt>");
        } else if (name == "a") {
            if (closing) {
                w.inlineTag("</a>");
            } else if (const QRegularExpressionMatch link = href.match(m.captured(3)); link.hasMatch()) {
                w.inlineTag("<a href=" + link.captured(1) + '>');
            } else {
                w.inlineTag("<a href=\"\">");
            }
        } else if (name.size() == 2 && name[0] == 'h' && name[1].isDigit()) {
            w.block(2);
            w.inlineTag(closing ? "</b>" : "<b>");
        } else if (name == "table") {
            w.block(2);
        } else if (name == "tr") {
            w.block(1);
            w.cellBefore = false;
        } else if (name == "td" || name == "th") {
            if (!closing) {
                if (w.cellBefore) {
                    w.inlineTag("  ");
                }
                w.cellBefore = true;
            }
        }
        /* any other tag goes, its text stays */
    }
    w.text(html.mid(from));
    while (w.out.endsWith('\n') || w.out.endsWith(' ')) {
        w.out.chop(1);
    }
    return w.out;
}

QString plain(const QString &html)
{
    static const QRegularExpression tags("<[^>]*>");
    QString text = pango(html);

    text.remove(tags);
    text.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&#39;", "'")
        .replace("&apos;", "'").replace("&#160;", QChar(0xa0)).replace("&amp;", "&");
    return text;
}

}
