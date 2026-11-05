// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

namespace Gtk4 {

/*
 * The rich text of the words both apps share (ui/hosttuningtext.h and the
 * like: Qt's subset, <p>, <br>, <pre>, <code>, <b>, <i>, <a href>, <nobr>)
 * as Pango markup, for a label or a row: paragraphs apart by a blank
 * line, <pre> in monospace on lines of its own, links kept (GTK's labels
 * open them), the other tags dropped and their text kept.
 */
QString markup(const QString &html);

/* Plain text, for a label's markup: escaped */
QString escaped(const QString &text);

}
