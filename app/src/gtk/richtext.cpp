// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/richtext.h"

#include <QRegularExpression>

namespace Gtk4 {

/* Text between tags, as Pango's markup takes it: a < or an & of plain words escaped */
static QString sanitized(QString text)
{
    static const QRegularExpression bareAmp("&(?!(?:#[0-9]+|#x[0-9a-fA-F]+|[a-zA-Z]+);)");

    text.replace(bareAmp, "&amp;");
    text.replace('<', "&lt;");
    text.replace('>', "&gt;");
    return text;
}

/* A block starts: the text before it ends its line, a paragraph after a blank one */
static void breakFor(QString &out, bool paragraph)
{
    while (out.endsWith(' ')) {
        out.chop(1);
    }
    if (out.isEmpty()) {
        return;
    }
    if (!out.endsWith('\n')) {
        out += '\n';
    }
    if (paragraph && !out.endsWith("\n\n")) {
        out += '\n';
    }
}

QString markup(const QString &html)
{
    static const QRegularExpression tag("<(/?)(\\w+)((?:\\s[^>]*)?)>");
    static const QRegularExpression blank("\n{3,}");
    QString out;
    qsizetype from = 0;
    /* in <pre>: its lines kept as they are */
    bool pre = false;

    for (auto it = tag.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const bool closing = !m.captured(1).isEmpty();
        const QString name = m.captured(2).toLower();
        QString text = sanitized(html.mid(from, m.capturedStart() - from));

        from = m.capturedEnd();
        if (!pre) {
            /* rich text's own line breaks are spaces */
            text.replace('\n', ' ');
            if (out.endsWith('\n')) {
                while (text.startsWith(' ')) {
                    text.remove(0, 1);
                }
            }
        }
        out += text;
        if (name == "p") {
            breakFor(out, true);
        } else if (name == "pre") {
            pre = !closing;
            if (closing) {
                out += "</tt>\n";
            } else {
                breakFor(out, false);
                out += "<tt>";
            }
        } else if (name == "br") {
            out += '\n';
        } else if (name == "b" || name == "i") {
            out += '<' + m.captured(1) + name + '>';
        } else if (name == "code") {
            out += closing ? "</tt>" : "<tt>";
        } else if (name == "a") {
            /* the link's target kept: GTK's labels open it */
            out += closing ? QString("</a>") : "<a" + m.captured(3) + '>';
        }
        /* any other tag goes, its text stays (nobr, span, div) */
    }
    QString rest = sanitized(html.mid(from));
    if (!pre) {
        rest.replace('\n', ' ');
    }
    out += rest;
    /* the entities Qt's rich text has that Pango's markup lacks */
    out.replace("&nbsp;", "&#160;");
    out.replace(blank, "\n\n");
    return out.trimmed();
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

}
