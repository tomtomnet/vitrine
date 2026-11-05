// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

/*
 * The rich text that the core and the texts both apps share write (Qt's
 * subset of HTML: paragraphs, lists, preformatted blocks, line breaks,
 * bold, italic, code and links, with QString::toHtmlEscaped()'s entities)
 * as Pango markup, for a GTK label, an AdwBanner's title or an alert's
 * body: paragraphs apart by an empty line, a list's items each on a line
 * of its own after a bullet, code in a monospace font, links kept (GTK's
 * labels open them).  Other tags go, their text stays.  Qt Core only.
 */
namespace Markup {

QString pango(const QString &html);
/* Plain text as Pango markup: escaped */
QString escaped(const QString &text);
/* The text alone, without its tags and entities: what a test reads */
QString plain(const QString &html);

}
