// SPDX-License-Identifier: GPL-2.0-or-later
#include "argseditor.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListWidget>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QTextBlock>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include "core/argsfile.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "ui/icons.h"
#include "ui/qemudocs.h"

static QString optionName(const QString &word)
{
    return word.mid(word.startsWith("--") ? 2 : 1);
}

static QString tr(const char *text)
{
    return QCoreApplication::translate("ArgsEditor", text);
}

QList<ArgsProblem> checkArgs(const QString &text, const QemuInfo *info, const QString &vmDir)
{
    const bool knowsOptions = info && !info->options.isEmpty();
    const bool knowsDevices = info && !info->devices.isEmpty();
    const QStringList lines = text.split('\n');
    QList<ArgsProblem> problems;

    for (int n = 0; n < lines.size(); n++) {
        const QString line = lines[n].trimmed();

        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith('#')) {
            if (line.startsWith("#qemu ") || line == "#qemu") {
                const QString qemu = VmConfig::qemuBinary(ArgsFile::parse(line));
                if (qemu.isEmpty()) {
                    problems << ArgsProblem{n, tr("#qemu needs the path of a QEMU binary")};
                } else if (!QFileInfo(qemu).isExecutable()) {
                    problems << ArgsProblem{n, tr("%1 is not an executable").arg(qemu)};
                }
            } else if (line.startsWith("#share ") || line == "#share") {
                const OptionValue v(line.mid(7).trimmed());
                if (v.get("tag").isEmpty() || v.get("path").isEmpty()) {
                    problems << ArgsProblem{n, tr("#share needs tag= and path=")};
                } else if (!QFileInfo(v.get("path")).isDir()) {
                    problems << ArgsProblem{
                        n, tr("Shared folder %1 does not exist").arg(v.get("path"))};
                }
            }
            continue;
        }
        if (!line.startsWith('-') || line.size() < 2) {
            problems << ArgsProblem{n, tr("Not an option: QEMU never sees this line")};
            continue;
        }

        const qsizetype space = line.indexOf(QRegularExpression("\\s"));
        const QString name = optionName(space < 0 ? line : line.left(space));
        const QString value = space < 0 ? QString() : line.mid(space + 1).trimmed();

        if (knowsOptions) {
            const QemuOptionDoc *doc = info->option(name);
            if (!doc) {
                problems << ArgsProblem{n, tr("Unknown option -%1").arg(name)};
                continue;
            }
            if (doc->takesValue && value.isEmpty()) {
                problems << ArgsProblem{n, tr("-%1 needs a value").arg(name)};
            } else if (!doc->takesValue && !value.isEmpty()) {
                problems << ArgsProblem{n, tr("-%1 takes no value").arg(name)};
            }
        }
        if (knowsDevices && name == "device") {
            const QString driver = OptionValue(value).implied();
            if (!driver.isEmpty() && driver != "help" && !info->device(driver)) {
                problems << ArgsProblem{n, tr("Unknown device %1").arg(driver)};
            }
        }
    }
    if (!vmDir.isEmpty()) {
        const QDir dir(vmDir);
        for (const VmConfig::FileRef &f : VmConfig::files(ArgsFile::parse(text))) {
            if (QFileInfo::exists(dir.absoluteFilePath(f.path))) {
                continue;
            }
            problems << ArgsProblem{
                f.line, QDir::isAbsolutePath(f.path)
                            ? tr("%1 does not exist").arg(f.path)
                            : tr("%1 is not in the VM folder: give its full path, or copy it "
                                 "there").arg(f.path)};
        }
        std::stable_sort(problems.begin(), problems.end(),
                         [](const ArgsProblem &a, const ArgsProblem &b) {
                             return a.line < b.line;
                         });
    }
    return problems;
}

/* Highlighter */

ArgsHighlighter::ArgsHighlighter(QTextDocument *document) : QSyntaxHighlighter(document)
{
    updateFormats(QApplication::palette());
}

void ArgsHighlighter::setDocs(QemuDocs *docs)
{
    m_docs = docs;
    rehighlight();
}

static QColor mix(const QColor &a, const QColor &b, double ratio)
{
    return QColor::fromRgbF(a.redF() * (1 - ratio) + b.redF() * ratio,
                            a.greenF() * (1 - ratio) + b.greenF() * ratio,
                            a.blueF() * (1 - ratio) + b.blueF() * ratio);
}

void ArgsHighlighter::updateFormats(const QPalette &palette)
{
    const QColor text = palette.color(QPalette::Text);
    const QColor base = palette.color(QPalette::Base);
    const bool dark = base.lightnessF() < 0.5;

    m_option = QTextCharFormat();
    m_option.setForeground(palette.color(QPalette::Link));
    m_option.setFontWeight(QFont::Bold);

    m_unknown = m_option;
    m_unknown.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    m_unknown.setUnderlineColor(dark ? QColor(0xff, 0x6b, 0x6b) : QColor(0xda, 0x44, 0x53));

    m_key = QTextCharFormat();
    m_key.setForeground(mix(text, base, 0.4));

    m_comment = QTextCharFormat();
    m_comment.setForeground(mix(text, base, 0.5));
    m_comment.setFontItalic(true);

    m_directive = QTextCharFormat();
    m_directive.setForeground(dark ? QColor(0x4d, 0xd0, 0xb0) : QColor(0x00, 0x7a, 0x5e));
    m_directive.setFontWeight(QFont::Bold);

    m_invalid = QTextCharFormat();
    m_invalid.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    m_invalid.setUnderlineColor(m_unknown.underlineColor());
    rehighlight();
}

void ArgsHighlighter::highlightKeys(const QString &text, int from)
{
    static const QRegularExpression key("(?:^|,)([A-Za-z0-9_.-]+=)");
    QRegularExpressionMatchIterator it = key.globalMatch(text, from);

    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        setFormat(int(m.capturedStart(1)), int(m.capturedLength(1)), m_key);
    }
}

void ArgsHighlighter::highlightBlock(const QString &text)
{
    const QemuInfo *info = m_docs ? m_docs->info() : nullptr;
    int start = 0;

    while (start < text.size() && text[start].isSpace()) {
        start++;
    }
    if (start == text.size()) {
        return;
    }
    if (text[start] == '#') {
        const QStringView rest = QStringView(text).mid(start);
        auto isDirective = [&](QStringView word) {
            return rest.startsWith(word) &&
                   (rest.size() == word.size() || rest[word.size()].isSpace());
        };
        if (isDirective(u"#share")) {
            setFormat(start, 6, m_directive);
            highlightKeys(text, start + 7);
        } else if (isDirective(u"#qemu")) {
            setFormat(start, 5, m_directive);
        } else {
            setFormat(start, int(text.size()) - start, m_comment);
        }
        return;
    }
    if (text[start] != '-') {
        setFormat(start, int(text.size()) - start, m_invalid);
        return;
    }

    int end = start;
    while (end < text.size() && !text[end].isSpace()) {
        end++;
    }
    const QString name = optionName(text.mid(start, end - start));
    const bool unknown = info && !info->options.isEmpty() && !info->option(name);

    setFormat(start, end - start, unknown ? m_unknown : m_option);
    highlightKeys(text, end);

    if (name == "device" && info && !info->devices.isEmpty()) {
        /* the driver */
        int from = end;
        while (from < text.size() && text[from].isSpace()) {
            from++;
        }
        int to = from;
        while (to < text.size() && text[to] != ',' && !text[to].isSpace()) {
            to++;
        }
        const QString driver = text.mid(from, to - from);
        if (!driver.isEmpty() && !driver.contains('=') && driver != "help" &&
            !info->device(driver)) {
            setFormat(from, to - from, m_invalid);
        }
    }
}

/* Editor */

ArgsEditor::ArgsEditor(QWidget *parent)
    : QPlainTextEdit(parent), m_highlighter(new ArgsHighlighter(document())),
      m_completer(new QCompleter(this)), m_model(new QStandardItemModel(this))
{
    auto *popup = new QTreeView;

    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabChangesFocus(true);
    setPlaceholderText(tr("-option value, one per line"));

    popup->setHeaderHidden(true);
    popup->setRootIsDecorated(false);
    popup->setUniformRowHeights(true);
    popup->setEditTriggers(QAbstractItemView::NoEditTriggers);
    popup->setSelectionBehavior(QAbstractItemView::SelectRows);
    popup->header()->setStretchLastSection(true);
    popup->setTextElideMode(Qt::ElideRight);

    m_completer->setModel(m_model);
    m_completer->setPopup(popup);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setModelSorting(QCompleter::CaseInsensitivelySortedModel);
    m_completer->setMaxVisibleItems(12);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this,
            &ArgsEditor::insertCompletion);

    setDocs(QemuDocs::preferred());
}

void ArgsEditor::setDocs(QemuDocs *docs)
{
    if (docs == m_docs) {
        return;
    }
    if (m_docs) {
        m_docs->disconnect(this);
    }
    m_docs = docs;
    m_context = Context::None;
    m_highlighter->setDocs(docs);
    connect(docs, &QemuDocs::changed, this, [this]() {
        m_context = Context::None;
        m_highlighter->rehighlight();
    });
}

void ArgsEditor::fillModel(Context context)
{
    const QemuInfo *info = m_docs->info();
    QList<std::pair<QString, QString>> rows;

    if (context == m_context) {
        return;
    }
    m_context = context;
    m_model->clear();
    if (!info) {
        return;
    }
    if (context == Context::Option) {
        for (const QemuOptionDoc &o : info->options) {
            rows << std::pair(o.name, o.help.section('\n', 0, 0));
        }
    } else if (context == Context::Device) {
        for (const QemuDeviceDoc &d : info->devices) {
            if (d.userCreatable) {
                rows << std::pair(d.name, d.desc.isEmpty() ? d.category : d.desc);
                for (const QString &alias : d.aliases) {
                    rows << std::pair(alias, tr("alias of %1").arg(d.name));
                }
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    QString last;
    for (const auto &[name, desc] : rows) {
        if (name == last) {
            continue;
        }
        last = name;
        m_model->appendRow({new QStandardItem(name), new QStandardItem(desc)});
    }
}

void ArgsEditor::complete(bool force)
{
    static const QRegularExpression deviceRe("^\\s*-{1,2}device\\s+([\\w.-]*)$");
    static const QRegularExpression optionRe("^\\s*-{1,2}([\\w.-]*)$");
    const QTextCursor cursor = textCursor();
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    QString prefix;
    QRegularExpressionMatch m;

    if (!m_docs->info()) {
        m_completer->popup()->hide();
        return;
    }
    if ((m = deviceRe.match(before)).hasMatch()) {
        fillModel(Context::Device);
    } else if ((m = optionRe.match(before)).hasMatch()) {
        fillModel(Context::Option);
    } else {
        m_completer->popup()->hide();
        return;
    }
    prefix = m.captured(1);
    m_completer->setCompletionPrefix(prefix);
    if (m_completer->completionCount() == 0 ||
        (m_completer->completionCount() == 1 && m_completer->currentCompletion() == prefix &&
         !force)) {
        m_completer->popup()->hide();
        return;
    }
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));

    QRect rect = cursorRect();
    QAbstractItemView *popup = m_completer->popup();
    const int nameWidth = popup->sizeHintForColumn(0) + 24;

    static_cast<QTreeView *>(popup)->setColumnWidth(0, nameWidth);
    rect.setWidth(qMin(nameWidth + fontMetrics().averageCharWidth() * 50, width()));
    m_completer->complete(rect);
}

void ArgsEditor::insertCompletion(const QString &completion)
{
    QTextCursor cursor = textCursor();
    const int prefix = int(m_completer->completionPrefix().size());
    const QemuInfo *info = m_docs->info();

    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, prefix);
    cursor.insertText(completion);
    if (m_context == Context::Option && info) {
        const QemuOptionDoc *doc = info->option(completion);
        if (doc && doc->takesValue && cursor.atBlockEnd()) {
            cursor.insertText(" ");
        }
    }
    setTextCursor(cursor);
    if (m_context == Context::Option && completion == "device") {
        complete(true);
    }
}

void ArgsEditor::keyPressEvent(QKeyEvent *event)
{
    const bool popup = m_completer->popup()->isVisible();
    const bool force = event->key() == Qt::Key_Space &&
                       (event->modifiers() & Qt::ControlModifier);

    if (popup) {
        /* the popup gets them first, unless it cannot grab the keyboard */
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab: {
            const QModelIndex index = m_completer->popup()->currentIndex();
            m_completer->popup()->hide();
            if (index.isValid()) {
                insertCompletion(index.siblingAtColumn(0).data().toString());
            }
            return;
        }
        case Qt::Key_Escape:
        case Qt::Key_Backtab:
            m_completer->popup()->hide();
            return;
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QApplication::sendEvent(m_completer->popup(), event);
            return;
        default:
            break;
        }
    }
    if (!force) {
        QPlainTextEdit::keyPressEvent(event);
    }
    if (force) {
        complete(true);
    } else if (!event->text().isEmpty() && event->text()[0].isPrint() &&
               !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        complete(false);
    } else if (popup && (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete)) {
        complete(false);
    }
}

void ArgsEditor::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange) {
        m_highlighter->updateFormats(palette());
    }
    QPlainTextEdit::changeEvent(event);
}

void ArgsEditor::insertLine(const QString &line)
{
    QTextCursor cursor = textCursor();

    cursor.beginEditBlock();
    if (cursor.block().text().trimmed().isEmpty()) {
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    } else {
        cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertText("\n");
    }
    cursor.insertText(line);
    cursor.endEditBlock();
    setTextCursor(cursor);
    ensureCursorVisible();
    setFocus();
}

void ArgsEditor::goToLine(int line)
{
    const QTextBlock block = document()->findBlockByNumber(line);

    if (block.isValid()) {
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::EndOfBlock);
        setTextCursor(cursor);
        centerCursor();
        setFocus();
    }
}

/* Pane */

ArgsEditorPane::ArgsEditorPane(QWidget *parent)
    : QWidget(parent), m_editor(new ArgsEditor), m_problems(new QListWidget),
      m_timer(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_problems);
    m_problems->setObjectName("problems");
    m_problems->hide();
    m_problems->setFocusPolicy(Qt::TabFocus);
    m_timer->setSingleShot(true);
    m_timer->setInterval(300);

    connect(m_timer, &QTimer::timeout, this, &ArgsEditorPane::check);
    connect(m_editor, &QPlainTextEdit::textChanged, m_timer, qOverload<>(&QTimer::start));
    setDocs(QemuDocs::preferred());
    connect(m_problems, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        m_editor->goToLine(item->data(Qt::UserRole).toInt());
    });
    connect(m_problems, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        m_editor->goToLine(item->data(Qt::UserRole).toInt());
    });
}

void ArgsEditorPane::setDocs(QemuDocs *docs)
{
    if (docs == m_docs) {
        return;
    }
    if (m_docs) {
        m_docs->disconnect(this);
    }
    m_docs = docs;
    m_editor->setDocs(docs);
    connect(docs, &QemuDocs::changed, this, &ArgsEditorPane::check);
    emit docsChanged(docs);
}

void ArgsEditorPane::check()
{
    const QString text = m_editor->toPlainText();

    m_timer->stop();
    /* the QEMU of the #qemu line, else the preferred one */
    setDocs(QemuDocs::of(VmConfig::qemuBinary(ArgsFile::parse(text))));

    const QList<ArgsProblem> problems = checkArgs(text, m_docs->info(), m_vmDir);
    const QIcon icon = Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning);

    m_problems->clear();
    for (const ArgsProblem &p : problems) {
        auto *item = new QListWidgetItem(icon, tr("Line %1: %2").arg(p.line + 1).arg(p.message));
        item->setData(Qt::UserRole, p.line);
        m_problems->addItem(item);
    }
    m_problems->setVisible(!problems.isEmpty());
    if (!problems.isEmpty()) {
        const int rows = int(qMin<qsizetype>(problems.size(), 4));
        m_problems->setFixedHeight(m_problems->sizeHintForRow(0) * rows +
                                   2 * m_problems->frameWidth() + 2);
    }
}
