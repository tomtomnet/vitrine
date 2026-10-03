// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemuinfo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QtConcurrent>

#include "core/paths.h"

/* The column where -help puts the descriptions */
static const int kHelpColumn = 16;

const QemuOptionDoc *QemuInfo::option(const QString &name) const
{
    for (const QemuOptionDoc &o : options) {
        if (o.name == name) {
            return &o;
        }
    }
    return nullptr;
}

const QemuDeviceDoc *QemuInfo::device(const QString &name) const
{
    for (const QemuDeviceDoc &d : devices) {
        if (d.name == name || d.aliases.contains(name)) {
            return &d;
        }
    }
    return nullptr;
}

static qsizetype indentOf(const QString &line)
{
    qsizetype n = 0;

    while (n < line.size() && line[n] == ' ') {
        n++;
    }
    return n;
}

static qsizetype indexOfOption(const QList<QemuOptionDoc> &options, const QString &name)
{
    for (qsizetype i = 0; i < options.size(); i++) {
        if (options[i].name == name) {
            return i;
        }
    }
    return -1;
}

/*
 * -help lists each option as its synopsis at column 0, continued on lines
 * starting with '[', then the description from column 16, on the same line
 * when the synopsis is short enough.  Section headers end with ':', and
 * options with several forms (-numa node..., -numa dist...) repeat.
 */
QList<QemuOptionDoc> QemuInfo::parseHelp(const QString &text)
{
    static const QRegularExpression gap(" {2,}");
    QList<QemuOptionDoc> out;
    QList<qsizetype> current;       /* the entries being read, -hda and -hdb */
    bool inSynopsis = false;
    QString section;

    auto addHelp = [&](const QString &help) {
        for (qsizetype i : std::as_const(current)) {
            out[i].help += (out[i].help.isEmpty() ? "" : "\n") + help;
        }
        inSynopsis = false;
    };

    for (QString line : text.split('\n')) {
        while (line.endsWith(' ') || line.endsWith('\r')) {
            line.chop(1);
        }
        const qsizetype indent = indentOf(line);

        if (line.isEmpty()) {
            current.clear();
        } else if (line.startsWith('-')) {
            QString synopsis = line, help;
            QRegularExpressionMatchIterator it = gap.globalMatch(line);

            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                if (m.capturedEnd() >= kHelpColumn) {
                    synopsis = line.left(m.capturedStart());
                    help = line.mid(m.capturedEnd());
                    break;
                }
            }
            QStringList words = synopsis.split(' ', Qt::SkipEmptyParts);
            /* "-kernel bzImage use 'bzImage' as kernel image" */
            if (help.isEmpty() && words.size() > 2 && words[1] != "or") {
                help = words.mid(2).join(' ');
                words = words.mid(0, 2);
                synopsis = words.join(' ');
            }

            QStringList names;
            bool takesValue;
            if (words.size() == 3 && words[1] == "or") {
                names = QStringList{words[0].mid(1), words[2].mid(1)};
                takesValue = false;
            } else {
                names = words[0].mid(1).split("/-");
                takesValue = words.size() > 1;
            }

            current.clear();
            for (const QString &name : std::as_const(names)) {
                qsizetype i = indexOfOption(out, name);
                if (i < 0) {
                    QemuOptionDoc doc;
                    doc.name = name;
                    doc.synopsis = synopsis;
                    doc.section = section;
                    doc.takesValue = takesValue;
                    i = out.size();
                    out << doc;
                } else {
                    out[i].synopsis += '\n' + synopsis;
                }
                current << i;
            }
            inSynopsis = true;
            if (!help.isEmpty()) {
                addHelp(help);
            }
        } else if (indent == 0 && line.endsWith(':')) {
            section = line.chopped(1);
            current.clear();
        } else if (current.isEmpty()) {
            /* the banner and the key bindings at the end */
        } else if (inSynopsis && indent < kHelpColumn && line[indent] == '[') {
            for (qsizetype i : std::as_const(current)) {
                out[i].synopsis += '\n' + line;
            }
        } else {
            addHelp(indent >= kHelpColumn ? line.mid(kHelpColumn) : line.trimmed());
        }
    }

    /* -M as -machine */
    for (QemuOptionDoc &o : out) {
        if (o.help.startsWith("as -")) {
            const QString target = o.help.mid(4).section(QRegularExpression("[^\\w-]"), 0, 0);
            const qsizetype i = indexOfOption(out, target);
            if (i >= 0) {
                o.takesValue = out[i].takesValue;
            }
        }
    }
    return out;
}

/* The C string literals in @text, concatenated per literal */
static QStringList stringLiterals(const QString &text)
{
    QStringList literals;

    for (qsizetype i = 0; i < text.size(); i++) {
        if (text[i] != '"') {
            continue;
        }
        QString lit;
        for (i++; i < text.size() && text[i] != '"'; i++) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                QChar c = text[++i];
                lit += c == 'n' ? QChar('\n') : c == 't' ? QChar('\t') : c;
            } else {
                lit += text[i];
            }
        }
        literals << lit;
    }
    return literals;
}

/*
 * qemu-options.hx: DEF(name, HAS_ARG or 0, enum, help, arch), possibly over
 * several lines, then the reStructuredText of the DEFs above it between
 * SRST and ERST.  Only the options the binary has are documented.
 */
void QemuInfo::mergeOptionsHx(QList<QemuOptionDoc> &options, const QString &hx)
{
    QList<std::pair<QString, bool>> pending;
    QString def, rst;
    bool inDef = false, inRst = false;
    int depth = 0;

    for (const QString &line : hx.split('\n')) {
        if (inRst) {
            if (line.trimmed() != "ERST") {
                rst += line + '\n';
                continue;
            }
            inRst = false;
            for (const auto &[name, takesValue] : std::as_const(pending)) {
                const qsizetype i = indexOfOption(options, name);
                if (i >= 0) {
                    options[i].details = rst;
                    options[i].takesValue = takesValue;
                }
            }
            pending.clear();
        } else if (inDef || line.startsWith("DEF(")) {
            bool inString = false;

            if (!inDef) {
                inDef = true;
                def.clear();
                depth = 0;
            }
            def += line + '\n';
            for (qsizetype i = 0; i < line.size(); i++) {
                if (line[i] == '\\' && inString) {
                    i++;
                } else if (line[i] == '"') {
                    inString = !inString;
                } else if (!inString && line[i] == '(') {
                    depth++;
                } else if (!inString && line[i] == ')') {
                    depth--;
                }
            }
            if (depth <= 0) {
                const QStringList literals = stringLiterals(def);
                if (!literals.isEmpty()) {
                    pending.append({literals[0], def.section(',', 1, 1).trimmed() == "HAS_ARG"});
                }
                inDef = false;
            }
        } else if (line.trimmed() == "SRST") {
            inRst = true;
            rst.clear();
        }
    }

    /* -h shares the documentation of -help */
    for (QemuOptionDoc &o : options) {
        for (const QemuOptionDoc &other : std::as_const(options)) {
            if (o.details.isEmpty() && !other.details.isEmpty() &&
                other.synopsis == o.synopsis) {
                o.details = other.details;
            }
        }
    }
}

/* Split "a "x, y", b c" at ", " outside quotes */
static QStringList splitFields(const QString &line)
{
    QStringList fields;
    QString cur;
    bool quoted = false;

    for (qsizetype i = 0; i < line.size(); i++) {
        if (line[i] == '"') {
            quoted = !quoted;
        }
        if (!quoted && line.mid(i, 2) == ", ") {
            fields << cur;
            cur.clear();
            i++;
            continue;
        }
        cur += line[i];
    }
    fields << cur;
    return fields;
}

static QString unquote(const QString &text)
{
    const QString t = text.trimmed();
    return t.size() >= 2 && t.startsWith('"') && t.endsWith('"')
               ? t.mid(1, t.size() - 2) : t;
}

QList<QemuDeviceDoc> QemuInfo::parseDeviceHelp(const QString &text)
{
    QList<QemuDeviceDoc> out;
    QString category;

    for (const QString &line : text.split('\n')) {
        if (line.startsWith("name ")) {
            QemuDeviceDoc d;

            d.category = category;
            for (const QString &field : splitFields(line)) {
                const QString key = field.section(' ', 0, 0);
                const QString value = unquote(field.section(' ', 1));

                if (key == "name") {
                    d.name = value;
                } else if (key == "bus") {
                    d.bus = value;
                } else if (key == "alias") {
                    d.aliases << value;
                } else if (key == "desc") {
                    d.desc = value;
                } else if (key == "no-user") {
                    d.userCreatable = false;
                }
            }
            out << d;
        } else if (line.trimmed().endsWith(':')) {
            category = line.trimmed().chopped(1);
        }
    }
    return out;
}

QList<QemuPropertyDoc> QemuInfo::parsePropertyHelp(const QString &text)
{
    static const QRegularExpression def("\\s*\\(default: (.*)\\)\\s*$");
    QList<QemuPropertyDoc> out;

    for (const QString &line : text.split('\n')) {
        if (!line.startsWith("  ")) {
            continue;
        }
        const QString t = line.trimmed();
        const qsizetype eq = t.indexOf("=<");
        if (eq <= 0) {
            continue;
        }

        QemuPropertyDoc p;
        qsizetype i = eq + 2;
        int depth = 1;

        p.name = t.left(eq);
        for (; i < t.size() && depth > 0; i++) {
            if (t[i] == '<') {
                depth++;
            } else if (t[i] == '>' && --depth == 0) {
                break;
            }
            p.type += t[i];
        }
        QString rest = t.mid(i + 1).trimmed();
        if (rest.startsWith('-')) {
            rest = rest.mid(1).trimmed();
        }
        const QRegularExpressionMatch m = def.match(rest);
        if (m.hasMatch()) {
            p.defaultValue = m.captured(1);
            rest = rest.left(m.capturedStart()).trimmed();
        }
        /* bools all say on/off */
        p.desc = p.type == "bool" && rest == "on/off" ? QString() : rest;
        out << p;
    }
    return out;
}

QList<QemuNamedDoc> QemuInfo::parseListHelp(const QString &text)
{
    QList<QemuNamedDoc> out;
    const QStringList lines = text.split('\n');

    /* after the header, until the first blank line */
    for (qsizetype i = 1; i < lines.size(); i++) {
        const QString t = lines[i].trimmed();
        if (t.isEmpty()) {
            break;
        }
        out << QemuNamedDoc{t.section(' ', 0, 0),
                            t.section(' ', 1).trimmed()};
    }
    return out;
}

/*
 * Inline markup: ``literal``, **strong**, *emphasis*, `interpreted`,
 * :role:`text <target>`, `link <url>`_, |substitution| and \ escapes
 */
static QString inlineRst(const QString &text)
{
    static const QRegularExpression literal("``(.+?)``");
    static const QRegularExpression strong("\\*\\*(.+?)\\*\\*");
    static const QRegularExpression emphasis("\\*([^*\\s](?:[^*]*[^*\\s])?)\\*");
    static const QRegularExpression role(":[\\w-]+:`([^`]*)`");
    static const QRegularExpression reference("`([^`]+)`(_{0,2})");
    /* those QEMU's documentation defines */
    static const QRegularExpression substitution("\\|qemu_system(?:_x86)?\\|");
    const auto anchored = QRegularExpression::AnchorAtOffsetMatchOption;
    QString out;

    /* "text <target>" */
    auto label = [](const QString &ref) {
        const qsizetype lt = ref.lastIndexOf('<');
        const QString text = lt > 0 && ref.endsWith('>') ? ref.left(lt) : ref;
        return text.trimmed().replace("_005f", "_").toHtmlEscaped();
    };
    auto at = [&](const QRegularExpression &re, qsizetype i, QRegularExpressionMatch *m) {
        *m = re.match(text, i, QRegularExpression::NormalMatch, anchored);
        return m->hasMatch();
    };

    for (qsizetype i = 0; i < text.size();) {
        const QChar c = text[i];
        const bool wordBefore = i > 0 && text[i - 1].isLetterOrNumber();
        QRegularExpressionMatch m;

        if (c == '`' && at(literal, i, &m)) {
            out += "<code>" + m.captured(1).toHtmlEscaped() + "</code>";
        } else if (c == ':' && at(role, i, &m)) {
            out += label(m.captured(1));
        } else if (c == '`' && at(reference, i, &m)) {
            const QString ref = m.captured(1);
            const qsizetype lt = ref.lastIndexOf('<');
            if (!m.captured(2).isEmpty() && lt > 0 && ref.endsWith('>')) {
                out += QString("<a href=\"%1\">%2</a>")
                           .arg(ref.mid(lt + 1).chopped(1).toHtmlEscaped(), label(ref));
            } else {
                out += "<i>" + label(ref) + "</i>";
            }
        } else if (c == '*' && at(strong, i, &m)) {
            out += "<b>" + m.captured(1).toHtmlEscaped() + "</b>";
        } else if (c == '*' && !wordBefore && at(emphasis, i, &m)) {
            out += "<i>" + m.captured(1).toHtmlEscaped() + "</i>";
        } else if (c == '|' && at(substitution, i, &m)) {
            out += "qemu-system-x86_64";
        } else if (c == '\\' && i + 1 < text.size()) {
            /* "\ " joins, "\x" is x */
            if (text[i + 1] != ' ') {
                out += QString(text[i + 1]).toHtmlEscaped();
            }
            i += 2;
            continue;
        } else {
            out += QString(c).toHtmlEscaped();
            i++;
            continue;
        }
        i = m.capturedEnd();
    }
    return out;
}

static bool isBullet(const QString &trimmed)
{
    static const QRegularExpression enumerated("^\\d+\\. ");
    return trimmed.startsWith("- ") || trimmed.startsWith("* ") ||
           enumerated.match(trimmed).hasMatch();
}

static bool isTableBorder(const QString &trimmed)
{
    static const QRegularExpression border("^=+( +=+)*$");
    return border.match(trimmed).hasMatch();
}

/*
 * What QTextBrowser shows of the reStructuredText of qemu-options.hx:
 * paragraphs, definition lists (a line followed by deeper ones), bullets,
 * literal blocks and simple tables, indented as in the source.
 */
QString QemuInfo::rstToHtml(const QString &rst)
{
    const QStringList lines = rst.split('\n');
    const qsizetype n = lines.size();
    QString html, prefix;
    qsizetype i = 0;

    auto blank = [&](qsizetype k) { return lines[k].trimmed().isEmpty(); };
    auto block = [&](const QString &tag, qsizetype indent, const QString &content) {
        html += QString("<%1 style=\"margin-left:%2px\">%3</%1>")
                    .arg(tag, QString::number(indent * 5), content);
    };
    /* the lines deeper than @indent, blank ones included */
    auto literal = [&](qsizetype indent, bool parsed) {
        QStringList body;
        qsizetype strip = -1;
        QString text;

        while (i < n && (blank(i) || indentOf(lines[i]) > indent)) {
            body << lines[i++];
        }
        while (!body.isEmpty() && body.first().trimmed().isEmpty()) {
            body.removeFirst();
        }
        while (!body.isEmpty() && body.last().trimmed().isEmpty()) {
            body.removeLast();
        }
        for (const QString &l : std::as_const(body)) {
            if (!l.trimmed().isEmpty() && (strip < 0 || indentOf(l) < strip)) {
                strip = indentOf(l);
            }
        }
        for (const QString &l : std::as_const(body)) {
            const QString line = l.mid(strip);
            text += (parsed ? inlineRst(line) : line.toHtmlEscaped()) + '\n';
        }
        if (strip >= 0) {
            text.chop(1);
            block("pre", strip, text);
        }
    };

    while (i < n) {
        if (blank(i)) {
            i++;
            continue;
        }

        const QString t = lines[i].trimmed();
        const qsizetype indent = indentOf(lines[i]);

        if (t == "\\") {
            /* between the terms of one definition */
            i++;
        } else if (t.startsWith(".. ")) {
            const QString directive = t.mid(3).section("::", 0, 0).trimmed();
            const QString argument = t.section("::", 1).trimmed();

            i++;
            if (directive == "parsed-literal" || directive.startsWith("code")) {
                literal(indent, directive == "parsed-literal");
            } else if (directive == "warning" || directive == "note") {
                prefix = QString("<b>%1</b> ").arg(directive == "note" ? QObject::tr("Note:")
                                                                        : QObject::tr("Warning:"));
                if (!argument.isEmpty()) {
                    block("p", indent, prefix + inlineRst(argument));
                    prefix.clear();
                }
            } else {
                /* include, link targets... */
                while (i < n && (blank(i) || indentOf(lines[i]) > indent)) {
                    i++;
                }
            }
        } else if (isTableBorder(t)) {
            QString text;
            QString last;

            while (i < n && !(blank(i) && isTableBorder(last) && text.count('\n') > 1)) {
                if (!blank(i)) {
                    last = lines[i].trimmed();
                }
                text += lines[i++].mid(indent).toHtmlEscaped() + '\n';
            }
            text.chop(1);
            block("pre", indent, text);
        } else if (isBullet(t)) {
            const bool numbered = t[0].isDigit();
            QString item = numbered ? t : t.mid(2);

            for (i++; i < n && !blank(i) && indentOf(lines[i]) > indent &&
                      !isBullet(lines[i].trimmed()); i++) {
                item += ' ' + lines[i].trimmed();
            }
            block("p", indent, prefix + (numbered ? "" : "• ") + inlineRst(item));
            prefix.clear();
        } else if (i + 1 < n && !blank(i + 1) && indentOf(lines[i + 1]) > indent) {
            block("p", indent, "<b>" + inlineRst(t) + "</b>");
            i++;
        } else {
            QStringList para{t};
            QString term;

            for (i++; i < n && !blank(i) && indentOf(lines[i]) == indent &&
                      !isBullet(lines[i].trimmed()); i++) {
                para << lines[i].trimmed();
            }
            /* a term right after the paragraph */
            if (para.size() > 1 && i < n && !blank(i) && indentOf(lines[i]) > indent) {
                term = para.takeLast();
            }

            QString text = para.join(' ');
            const bool literalNext = text.endsWith("::");
            if (literalNext) {
                text.chop(text == "::" ? 2 : text.endsWith(" ::") ? 3 : 1);
            }
            if (!text.isEmpty()) {
                block("p", indent, prefix + inlineRst(text));
                prefix.clear();
            }
            if (!term.isEmpty()) {
                block("p", indent, "<b>" + inlineRst(term) + "</b>");
            }
            if (literalNext) {
                literal(indent, false);
            }
        }
    }
    return html;
}

/* Loader */

QemuInfoLoader::QemuInfoLoader(const QString &binary, QObject *parent)
    : QObject(parent), m_binary(binary)
{
}

QString QemuInfoLoader::findOptionsHx(const QString &binary)
{
    /* build/qemu-system-x86_64 -> qemu-options.hx of the source tree */
    const QDir dir = QFileInfo(binary).absoluteDir();
    for (const QString &candidate : {dir.filePath("../qemu-options.hx"),
                                     dir.filePath("qemu-options.hx")}) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).canonicalFilePath();
        }
    }
    return {};
}

QString QemuInfoLoader::run(const QStringList &args, QString *error) const
{
    QProcess p;

    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(m_binary, args);
    if (!p.waitForFinished(15000)) {
        if (error) {
            *error = p.errorString();
        }
        p.kill();
        return {};
    }
    return QString::fromUtf8(p.readAll());
}

/*
 * The properties of all @devices at once, from one QEMU asked over QMP on
 * stdio: running it with -device X,help for each would take seconds
 */
static QHash<QString, QList<QemuPropertyDoc>> deviceProperties(
    const QString &binary, const QList<QemuDeviceDoc> &devices)
{
    QHash<QString, QList<QemuPropertyDoc>> out;
    QByteArray input = "{\"execute\":\"qmp_capabilities\"}\n";
    QByteArray output;
    QElapsedTimer clock;
    QProcess p;

    for (qsizetype i = 0; i < devices.size(); i++) {
        const QJsonObject command{{"execute", "device-list-properties"},
                                  {"arguments", QJsonObject{{"typename", devices[i].name}}},
                                  {"id", int(i)}};
        input += QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    }
    input += "{\"execute\":\"quit\"}\n";

    p.start(binary, {"-machine", "none", "-nodefaults", "-display", "none", "-qmp", "stdio"});
    if (!p.waitForStarted(5000)) {
        return out;
    }
    p.write(input);
    clock.start();
    while (p.state() != QProcess::NotRunning && clock.elapsed() < 30000) {
        p.waitForReadyRead(1000);
        output += p.readAllStandardOutput();
    }
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(1000);
    }
    output += p.readAllStandardOutput();

    for (const QByteArray &line : output.split('\n')) {
        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        const int i = reply["id"].toInt(-1);
        QList<QemuPropertyDoc> props;

        if (i < 0 || i >= devices.size() || !reply["return"].isArray()) {
            continue;
        }
        for (const QJsonValue &v : reply["return"].toArray()) {
            const QJsonValue def = v["default-value"];
            QemuPropertyDoc prop;

            prop.name = v["name"].toString();
            prop.type = v["type"].toString();
            prop.desc = v["description"].toString();
            /* as -device X,help shows them */
            if (prop.type == "bool" && prop.desc == "on/off") {
                prop.desc.clear();
            }
            if (def.isBool()) {
                prop.defaultValue = def.toBool() ? "on" : "off";
            } else if (def.isDouble()) {
                prop.defaultValue = QString::number(def.toDouble(), 'g', 17);
            } else if (def.isString()) {
                prop.defaultValue = def.toString();
            }
            props << prop;
        }
        std::sort(props.begin(), props.end(),
                  [](const QemuPropertyDoc &a, const QemuPropertyDoc &b) {
                      return a.name < b.name;
                  });
        out[devices[i].name] = props;
    }
    return out;
}

/* Bump when the parsers change what they produce */
static const int kCacheFormat = 2;

QString QemuInfoLoader::cachePath() const
{
    QString key = QString::number(kCacheFormat);

    for (const QString &path : {m_binary, findOptionsHx(m_binary)}) {
        const QFileInfo fi(path);
        key += QString("|%1|%2|%3").arg(fi.canonicalFilePath(),
                                        QString::number(fi.lastModified().toMSecsSinceEpoch()),
                                        QString::number(fi.size()));
    }
    return Paths::cacheDir() + "/qemu-info-" +
           QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex() + ".json";
}

static QJsonArray namedToJson(const QList<QemuNamedDoc> &list)
{
    QJsonArray a;
    for (const QemuNamedDoc &n : list) {
        a.append(QJsonArray{n.name, n.desc});
    }
    return a;
}

static QList<QemuNamedDoc> namedFromJson(const QJsonValue &v)
{
    QList<QemuNamedDoc> list;
    for (const QJsonValue &e : v.toArray()) {
        list << QemuNamedDoc{e[0].toString(), e[1].toString()};
    }
    return list;
}

void QemuInfoLoader::saveCache() const
{
    QJsonObject root;
    QJsonArray options, devices;
    QJsonObject properties;

    for (const QemuOptionDoc &o : m_info.options) {
        options.append(QJsonObject{{"name", o.name}, {"synopsis", o.synopsis},
                                   {"help", o.help}, {"details", o.details},
                                   {"section", o.section},
                                   {"value", o.takesValue}});
    }
    for (const QemuDeviceDoc &d : m_info.devices) {
        devices.append(QJsonObject{{"name", d.name}, {"bus", d.bus},
                                   {"desc", d.desc}, {"category", d.category},
                                   {"aliases", QJsonArray::fromStringList(d.aliases)},
                                   {"user", d.userCreatable}});
    }
    for (auto it = m_info.properties.begin(); it != m_info.properties.end(); ++it) {
        QJsonArray props;
        for (const QemuPropertyDoc &p : it.value()) {
            props.append(QJsonArray{p.name, p.type, p.desc, p.defaultValue});
        }
        properties[it.key()] = props;
    }
    root["version"] = m_info.version;
    root["options"] = options;
    root["devices"] = devices;
    root["properties"] = properties;
    root["machines"] = namedToJson(m_info.machines);
    root["cpus"] = namedToJson(m_info.cpus);
    root["objects"] = namedToJson(m_info.objects);
    root["netdevs"] = namedToJson(m_info.netdevs);
    root["chardevs"] = namedToJson(m_info.chardevs);
    root["audiodevs"] = namedToJson(m_info.audiodevs);
    root["displays"] = namedToJson(m_info.displays);
    root["accels"] = namedToJson(m_info.accels);

    QDir().mkpath(QFileInfo(cachePath()).absolutePath());
    QFile f(cachePath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
}

bool QemuInfoLoader::loadCache()
{
    QFile f(cachePath());
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.isEmpty()) {
        return false;
    }

    QemuInfo info;
    info.version = root["version"].toString();
    for (const QJsonValue &v : root["options"].toArray()) {
        info.options << QemuOptionDoc{v["name"].toString(), v["synopsis"].toString(),
                                      v["help"].toString(), v["details"].toString(),
                                      v["section"].toString(), v["value"].toBool()};
    }
    for (const QJsonValue &v : root["devices"].toArray()) {
        QemuDeviceDoc d;
        d.name = v["name"].toString();
        d.bus = v["bus"].toString();
        d.desc = v["desc"].toString();
        d.category = v["category"].toString();
        for (const QJsonValue &a : v["aliases"].toArray()) {
            d.aliases << a.toString();
        }
        d.userCreatable = v["user"].toBool(true);
        info.devices << d;
    }
    const QJsonObject props = root["properties"].toObject();
    for (auto it = props.begin(); it != props.end(); ++it) {
        QList<QemuPropertyDoc> list;
        for (const QJsonValue &p : it.value().toArray()) {
            list << QemuPropertyDoc{p[0].toString(), p[1].toString(),
                                    p[2].toString(), p[3].toString()};
        }
        info.properties[it.key()] = list;
    }
    info.machines = namedFromJson(root["machines"]);
    info.cpus = namedFromJson(root["cpus"]);
    info.objects = namedFromJson(root["objects"]);
    info.netdevs = namedFromJson(root["netdevs"]);
    info.chardevs = namedFromJson(root["chardevs"]);
    info.audiodevs = namedFromJson(root["audiodevs"]);
    info.displays = namedFromJson(root["displays"]);
    info.accels = namedFromJson(root["accels"]);
    m_info = info;
    return true;
}

void QemuInfoLoader::load()
{
    if (m_loaded) {
        emit loaded();
        return;
    }
    if (m_loading) {
        return;
    }
    if (loadCache()) {
        m_loaded = true;
        emit loaded();
        return;
    }

    m_loading = true;
    auto *watcher = new QFutureWatcher<QemuInfo>(this);
    connect(watcher, &QFutureWatcher<QemuInfo>::finished, this, [this, watcher]() {
        const QemuInfo info = watcher->result();
        m_loading = false;
        watcher->deleteLater();
        if (info.options.isEmpty()) {
            emit failed(tr("%1 printed no options").arg(m_binary));
            return;
        }
        m_info = info;
        m_loaded = true;
        saveCache();
        emit loaded();
    });
    watcher->setFuture(QtConcurrent::run([this]() {
        QemuInfo info;
        const QString hx = findOptionsHx(m_binary);

        info.version = run({"-version"}).section('\n', 0, 0)
                           .section("version ", 1).section(' ', 0, 0).trimmed();
        info.options = QemuInfo::parseHelp(run({"-help"}));
        if (!hx.isEmpty()) {
            QFile f(hx);
            if (f.open(QIODevice::ReadOnly)) {
                QemuInfo::mergeOptionsHx(info.options, QString::fromUtf8(f.readAll()));
            }
        }
        info.devices = QemuInfo::parseDeviceHelp(run({"-device", "help"}));
        info.properties = deviceProperties(m_binary, info.devices);
        info.machines = QemuInfo::parseListHelp(run({"-machine", "help"}));
        info.cpus = QemuInfo::parseListHelp(run({"-cpu", "help"}));
        info.objects = QemuInfo::parseListHelp(run({"-object", "help"}));
        info.netdevs = QemuInfo::parseListHelp(run({"-netdev", "help"}));
        info.chardevs = QemuInfo::parseListHelp(run({"-chardev", "help"}));
        info.audiodevs = QemuInfo::parseListHelp(run({"-audiodev", "help"}));
        info.displays = QemuInfo::parseListHelp(run({"-display", "help"}));
        info.accels = QemuInfo::parseListHelp(run({"-accel", "help"}));
        return info;
    }));
}

void QemuInfoLoader::loadProperties(const QString &device)
{
    if (m_info.properties.contains(device)) {
        emit propertiesLoaded(device);
        return;
    }

    auto *watcher = new QFutureWatcher<QList<QemuPropertyDoc>>(this);
    connect(watcher, &QFutureWatcher<QList<QemuPropertyDoc>>::finished, this,
            [this, watcher, device]() {
        m_info.properties[device] = watcher->result();
        watcher->deleteLater();
        saveCache();
        emit propertiesLoaded(device);
    });
    watcher->setFuture(QtConcurrent::run([this, device]() {
        return QemuInfo::parsePropertyHelp(run({"-device", device + ",help"}));
    }));
}
