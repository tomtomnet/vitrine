// SPDX-License-Identifier: GPL-2.0-or-later
#include "referencepanel.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "core/paths.h"
#include "core/qemuinfo.h"
#include "ui/argseditor.h"
#include "ui/icons.h"
#include "ui/qemudocs.h"
#include "ui/widgets.h"

enum { KindRole = Qt::UserRole, NameRole };

/* Lowercase, with _ and - as spaces: "drm native" finds drm_native_context */
static QString searchable(const QString &text)
{
    QString s = text.toLower();
    return s.replace('_', ' ').replace('-', ' ');
}

/* "virtio-vga-gl,drm_native_context": the device and the property */
static std::pair<QString, QString> splitProperty(const QString &name)
{
    return {name.section(',', 0, 0), name.section(',', 1)};
}

ReferencePanel::ReferencePanel(QWidget *parent)
    : QWidget(parent), m_search(new QLineEdit), m_status(new QLabel),
      m_results(new QTreeWidget), m_doc(new QTextBrowser), m_use(new QPushButton),
      m_timer(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);
    auto *splitter = new Splitter(Qt::Vertical);
    auto *buttons = new QHBoxLayout;

    layout->setContentsMargins(0, 0, 0, 0);
    m_search->setObjectName("search");
    m_search->setPlaceholderText(tr("Search options, devices and their properties, machines…"));
    m_search->setClearButtonEnabled(true);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->hide();

    m_results->setObjectName("results");
    m_results->setColumnCount(2);
    m_results->setHeaderHidden(true);
    m_results->setUniformRowHeights(true);
    m_results->setTextElideMode(Qt::ElideRight);
    m_results->header()->setStretchLastSection(true);
    m_results->header()->setSectionResizeMode(0, QHeaderView::Interactive);

    m_doc->setObjectName("doc");
    /* QTextBrowser would open file: links itself, and show nothing */
    m_doc->setOpenLinks(false);
    connect(m_doc, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        if (!url.scheme().isEmpty()) {
            QDesktopServices::openUrl(url);
        }
    });

    m_use->setEnabled(false);
    buttons->addStretch();
    buttons->addWidget(m_use);

    splitter->addWidget(m_results);
    splitter->addWidget(m_doc);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setChildrenCollapsible(false);

    layout->addWidget(m_search);
    layout->addWidget(m_status);
    layout->addWidget(splitter, 1);
    layout->addLayout(buttons);
    setFocusProxy(m_search);
    setEditor(nullptr);

    m_timer->setSingleShot(true);
    m_timer->setInterval(150);
    connect(m_timer, &QTimer::timeout, this, &ReferencePanel::refresh);
    connect(m_search, &QLineEdit::textChanged, m_timer, qOverload<>(&QTimer::start));
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        /* to the first result */
        refresh();
        for (int i = 0; i < m_results->topLevelItemCount(); i++) {
            QTreeWidgetItem *group = m_results->topLevelItem(i);
            if (group->childCount() > 0) {
                m_results->setCurrentItem(group->child(0));
                m_results->setFocus();
                break;
            }
        }
    });
    connect(m_results, &QTreeWidget::currentItemChanged, this, &ReferencePanel::showCurrent);
    connect(m_results, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item->parent()) {
            use();
        }
    });
    connect(m_use, &QPushButton::clicked, this, &ReferencePanel::use);
    setDocs(QemuDocs::preferred());
}

void ReferencePanel::setDocs(QemuDocs *docs)
{
    if (docs == m_docs) {
        return;
    }
    if (m_docs) {
        m_docs->disconnect(this);
    }
    m_docs = docs;
    connect(docs, &QemuDocs::changed, this, &ReferencePanel::refresh);
    connect(docs, &QemuDocs::propertiesLoaded, this, [this](const QString &device) {
        const QTreeWidgetItem *item = m_results->currentItem();
        if (item && item->data(0, KindRole).toInt() == Device &&
            item->data(0, NameRole).toString() == device) {
            showCurrent();
        }
    });
    refresh();
}

void ReferencePanel::setEditor(ArgsEditor *editor)
{
    m_editor = editor;
    if (editor) {
        m_use->setText(tr("&Insert"));
        m_use->setIcon(Icons::themed({"list-add"}, QStyle::SP_ArrowRight));
        m_use->setToolTip(tr("Add a line for this entry to the arguments"));
    } else {
        m_use->setText(tr("&Copy"));
        m_use->setIcon(Icons::themed({"edit-copy"}, QStyle::SP_FileIcon));
        m_use->setToolTip(tr("Copy a line for this entry to the clipboard"));
    }
}

void ReferencePanel::setSearchText(const QString &text)
{
    m_search->setText(text);
    refresh();
}

void ReferencePanel::refresh()
{
    struct Entry {
        Kind kind;
        QString name;
        QString desc;
        QString text;       // everything else to search
        QString label = {}; // if not the name
    };
    static const char *const groups[] = {
        QT_TR_NOOP("Options"),         QT_TR_NOOP("Devices"),
        QT_TR_NOOP("Machines"),        QT_TR_NOOP("CPU models"),
        QT_TR_NOOP("Objects"),         QT_TR_NOOP("Network backends"),
        QT_TR_NOOP("Character devices"), QT_TR_NOOP("Audio backends"),
        QT_TR_NOOP("Displays"),        QT_TR_NOOP("Accelerators"),
        QT_TR_NOOP("Device properties"),
    };
    /* past that, a search for properties is too vague */
    const int maxProperties = 300;
    const QemuInfo *info = m_docs->info();
    const QStringList words = searchable(m_search->text()).split(' ', Qt::SkipEmptyParts);
    const QString query = words.join(' ');
    const QString currentName =
        m_results->currentItem() ? m_results->currentItem()->data(0, NameRole).toString()
                                 : QString();
    const int currentKind =
        m_results->currentItem() ? m_results->currentItem()->data(0, KindRole).toInt() : -1;
    QTreeWidgetItem *restore = nullptr;
    QList<Entry> entries;

    m_results->clear();
    if (!info) {
        m_status->setText(m_docs->status());
        m_status->show();
        m_doc->clear();
        return;
    }
    m_status->hide();

    for (const QemuOptionDoc &o : info->options) {
        entries << Entry{Option, o.name, o.help.section('\n', 0, 0),
                         o.synopsis + ' ' + o.help + ' ' + o.details};
    }
    for (const QemuDeviceDoc &d : info->devices) {
        entries << Entry{Device, d.name, d.desc.isEmpty() ? d.category : d.desc,
                         d.category + ' ' + d.bus + ' ' + d.aliases.join(' ')};
    }
    const std::pair<Kind, const QList<QemuNamedDoc> *> lists[] = {
        {Machine, &info->machines}, {Cpu, &info->cpus},         {Object, &info->objects},
        {Netdev, &info->netdevs},   {Chardev, &info->chardevs}, {Audiodev, &info->audiodevs},
        {Display, &info->displays}, {Accel, &info->accels},
    };
    for (const auto &[kind, list] : lists) {
        for (const QemuNamedDoc &n : *list) {
            entries << Entry{kind, n.name, n.desc, {}, {}};
        }
    }
    /* thousands of them: only when searching */
    if (!words.isEmpty()) {
        for (const QemuDeviceDoc &d : info->devices) {
            for (const QemuPropertyDoc &p : info->properties.value(d.name)) {
                QString desc = d.name + " · " + p.type;
                if (!p.defaultValue.isEmpty()) {
                    desc += ' ' + tr("(default %1)").arg(p.defaultValue);
                }
                if (!p.desc.isEmpty()) {
                    desc += " · " + p.desc;
                }
                entries << Entry{Property, d.name + ',' + p.name, desc, {}, p.name};
            }
        }
    }

    QList<std::pair<int, const Entry *>> hits[Property + 1];
    for (const Entry &e : entries) {
        const QString name = searchable(e.label.isEmpty() ? e.name : e.label);
        const QString text = searchable(e.desc + ' ' + e.text);
        int score = 0, nameHits = 0;
        bool all = true;

        for (const QString &w : words) {
            if (name == w) {
                score += 100;
            } else if (name.startsWith(w)) {
                score += 50;
            } else if (name.contains(w)) {
                score += 20;
            } else if (text.contains(w)) {
                score += 1;
                continue;
            } else {
                all = false;
                break;
            }
            nameHits++;
        }
        if (!words.isEmpty() && name == query) {
            score += 200;
        }
        /* a property is found by its name, not by its device's */
        if (all && (e.kind != Property || nameHits > 0)) {
            hits[e.kind] << std::pair(score, &e);
        }
    }

    for (int kind = Option; kind <= Property; kind++) {
        auto &list = hits[kind];
        if (list.isEmpty()) {
            continue;
        }
        std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
            if (a.first != b.first) {
                return a.first > b.first;
            }
            return a.second->name.compare(b.second->name, Qt::CaseInsensitive) < 0;
        });

        auto *group = new QTreeWidgetItem(
            m_results, {QString("%1 (%2)").arg(tr(groups[kind])).arg(list.size())});
        if (list.size() > maxProperties) {
            list.resize(maxProperties);
        }
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);
        group->setFirstColumnSpanned(true);
        group->setFlags(Qt::ItemIsEnabled);
        group->setData(0, KindRole, kind);

        for (const auto &[score, e] : list) {
            auto *item = new QTreeWidgetItem(
                group, {kind == Option ? '-' + e->name : e->label.isEmpty() ? e->name : e->label,
                        e->desc});
            item->setData(0, KindRole, kind);
            item->setData(0, NameRole, e->name);
            item->setToolTip(1, e->desc);
            if (kind == currentKind && e->name == currentName) {
                restore = item;
            }
        }
        group->setExpanded(!words.isEmpty() || kind == Option);
    }
    m_results->resizeColumnToContents(0);
    m_results->setColumnWidth(0, qMin(m_results->columnWidth(0),
                                      m_results->viewport()->width() / 2));
    if (restore) {
        m_results->setCurrentItem(restore);
        m_results->scrollToItem(restore);
    } else {
        showCurrent();
    }
}

void ReferencePanel::showCurrent()
{
    const QTreeWidgetItem *item = m_results->currentItem();
    const QemuInfo *info = m_docs->info();
    QString html;

    m_use->setEnabled(item && item->parent() && info);
    if (!info) {
        return;
    }
    if (!item || !item->parent()) {
        m_doc->setHtml(tr("<p>The documentation of <b>%1</b>, QEMU %2.</p>"
                          "<p>Search it, pick an entry and see its description here.</p>")
                           .arg(m_docs->binary().toHtmlEscaped(),
                                info->version.toHtmlEscaped()));
        return;
    }

    const int kind = item->data(0, KindRole).toInt();
    const QString name = item->data(0, NameRole).toString();
    auto row = [](const QString &key, const QString &value) {
        return QString("<tr><td><b>%1</b>&nbsp;&nbsp;</td><td>%2</td></tr>")
            .arg(key.toHtmlEscaped(), value.toHtmlEscaped());
    };

    if (kind == Property) {
        const auto [device, property] = splitProperty(name);
        const QemuDeviceDoc *d = info->device(device);

        for (const QemuPropertyDoc &p : info->properties.value(device)) {
            if (p.name != property) {
                continue;
            }
            html = QString("<h2>%1</h2>").arg(p.name.toHtmlEscaped());
            html += tr("<p>A property of the <b>%1</b> device%2.</p>")
                        .arg(device.toHtmlEscaped(),
                             d && !d->desc.isEmpty() ? ": " + d->desc.toHtmlEscaped()
                                                     : QString());
            html += "<table>" + row(tr("Type"), p.type);
            if (!p.defaultValue.isEmpty()) {
                html += row(tr("Default"), p.defaultValue);
            }
            if (!p.desc.isEmpty()) {
                html += row(tr("Description"), p.desc);
            }
            html += "</table>";
            html += tr("<p>Use it with:</p>") + "<pre>" + lineFor(item).toHtmlEscaped() + "</pre>";
        }
    } else if (kind == Option) {
        const QemuOptionDoc *o = info->option(name);
        if (!o) {
            return;
        }
        html = QString("<h2>-%1</h2>").arg(name.toHtmlEscaped());
        if (!o->section.isEmpty()) {
            html += QString("<p><i>%1</i></p>").arg(o->section.toHtmlEscaped());
        }
        html += "<pre>" + o->synopsis.toHtmlEscaped() + "</pre>";
        if (!o->details.isEmpty()) {
            html += QemuInfo::rstToHtml(o->details);
        } else if (!o->help.isEmpty()) {
            html += "<pre>" + o->help.toHtmlEscaped() + "</pre>";
        }
    } else if (kind == Device) {
        const QemuDeviceDoc *d = info->device(name);
        if (!d) {
            return;
        }
        html = QString("<h2>%1</h2>").arg(name.toHtmlEscaped());
        if (!d->desc.isEmpty()) {
            html += QString("<p>%1</p>").arg(d->desc.toHtmlEscaped());
        }
        html += "<table>" + row(tr("Category"), d->category);
        if (!d->bus.isEmpty()) {
            html += row(tr("Bus"), d->bus);
        }
        if (!d->aliases.isEmpty()) {
            html += row(tr("Also called"), d->aliases.join(", "));
        }
        if (!d->userCreatable) {
            html += row(tr("Note"), tr("Built into machines, cannot be added with -device"));
        }
        html += "</table>";
        html += QString("<h3>%1</h3>").arg(tr("Properties"));
        if (info->properties.contains(name)) {
            const QList<QemuPropertyDoc> props = info->properties.value(name);
            if (props.isEmpty()) {
                html += tr("<p>None.</p>");
            } else {
                html += QString("<table cellspacing=\"0\" cellpadding=\"3\">"
                                "<tr><th align=\"left\">%1</th><th align=\"left\">%2</th>"
                                "<th align=\"left\">%3</th><th align=\"left\">%4</th></tr>")
                            .arg(tr("Property"), tr("Type"), tr("Default"), tr("Description"));
                for (const QemuPropertyDoc &p : props) {
                    html += QString("<tr><td><code>%1</code></td><td>%2</td><td>%3</td>"
                                    "<td>%4</td></tr>")
                                .arg(p.name.toHtmlEscaped(), p.type.toHtmlEscaped(),
                                     p.defaultValue.toHtmlEscaped(), p.desc.toHtmlEscaped());
                }
                html += "</table>";
            }
        } else {
            html += tr("<p>Loading the properties…</p>");
            m_docs->loadProperties(name);
        }
    } else {
        const QList<QemuNamedDoc> *list = nullptr;
        switch (kind) {
        case Machine:
            list = &info->machines;
            break;
        case Cpu:
            list = &info->cpus;
            break;
        case Object:
            list = &info->objects;
            break;
        case Netdev:
            list = &info->netdevs;
            break;
        case Chardev:
            list = &info->chardevs;
            break;
        case Audiodev:
            list = &info->audiodevs;
            break;
        case Display:
            list = &info->displays;
            break;
        default:
            list = &info->accels;
            break;
        }
        html = QString("<h2>%1</h2>").arg(name.toHtmlEscaped());
        for (const QemuNamedDoc &n : *list) {
            if (n.name == name && !n.desc.isEmpty()) {
                html += QString("<p>%1</p>").arg(n.desc.toHtmlEscaped());
            }
        }
        html += tr("<p>Use it with:</p>") + "<pre>" + lineFor(item).toHtmlEscaped() + "</pre>";
        const QString option = lineFor(item).section(' ', 0, 0).mid(1);
        const QemuOptionDoc *o = info->option(option);
        if (o) {
            html += tr("<p>See the <b>-%1</b> option for its settings.</p>")
                        .arg(option.toHtmlEscaped());
        }
    }
    m_doc->setHtml(html);
}

QString ReferencePanel::lineFor(const QTreeWidgetItem *item) const
{
    const QemuInfo *info = m_docs->info();
    const QString name = item->data(0, NameRole).toString();

    switch (item->data(0, KindRole).toInt()) {
    case Option: {
        const QemuOptionDoc *o = info ? info->option(name) : nullptr;
        return '-' + name + (o && o->takesValue ? " " : "");
    }
    case Device:
        return "-device " + name;
    case Machine:
        return "-machine " + name;
    case Cpu:
        return "-cpu " + name;
    case Object:
        return "-object " + name + ",id=";
    case Netdev:
        return "-netdev " + name + ",id=";
    case Chardev:
        return "-chardev " + name + ",id=";
    case Audiodev:
        return "-audiodev " + name + ",id=";
    case Display:
        return "-display " + name;
    case Property: {
        const auto [device, property] = splitProperty(name);
        QString value;
        for (const QemuPropertyDoc &p : info ? info->properties.value(device)
                                             : QList<QemuPropertyDoc>()) {
            if (p.name == property && p.type == "bool") {
                value = "on";
            }
        }
        return "-device " + device + ',' + property + '=' + value;
    }
    default:
        return "-accel " + name;
    }
}

void ReferencePanel::use()
{
    const QTreeWidgetItem *item = m_results->currentItem();

    if (!item || !item->parent()) {
        return;
    }
    if (m_editor) {
        m_editor->insertLine(lineFor(item));
    } else {
        QApplication::clipboard()->setText(lineFor(item).trimmed());
    }
}

/* Window */

static QPointer<ReferenceWindow> s_window;

ReferenceWindow::ReferenceWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    auto *layout = new QVBoxLayout(this);
    const QSettings settings(Paths::settingsPath(), QSettings::IniFormat);

    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("QEMU Reference"));
    layout->addWidget(new ReferencePanel);
    if (!restoreGeometry(settings.value("reference/geometry").toByteArray())) {
        resize(760, 640);
    }
}

void ReferenceWindow::open(QWidget *parent)
{
    if (!s_window) {
        s_window = new ReferenceWindow(parent);
    }
    s_window->show();
    s_window->raise();
    s_window->activateWindow();
}

void ReferenceWindow::closeEvent(QCloseEvent *event)
{
    QSettings(Paths::settingsPath(), QSettings::IniFormat)
        .setValue("reference/geometry", saveGeometry());
    QWidget::closeEvent(event);
}
