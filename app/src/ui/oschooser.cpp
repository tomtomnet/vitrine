// SPDX-License-Identifier: GPL-2.0-or-later
#include "oschooser.h"

#include <QCheckBox>
#include <QFrame>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMap>
#include <QPainter>
#include <QScreen>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "ui/systems.h"
#include "ui/widgets.h"

/* The rows the search and the releases no longer supported leave */
class SystemFilter : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void set(const QStringList &words, bool old, const QString &current)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        m_words = words;
        m_old = old;
        m_current = current;
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        m_words = words;
        m_old = old;
        m_current = current;
        invalidateFilter();
#endif
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const QModelIndex i = sourceModel()->index(row, 0, parent);
        const QString text = i.data(OsChooser::SearchRole).toString();

        /* a group shows for its rows (recursive filtering) */
        if (sourceModel()->hasChildren(i)) {
            return false;
        }
        if (!m_old && i.data(OsChooser::EolRole).toBool() &&
            i.data(OsChooser::IdRole).toString() != m_current) {
            return false;
        }
        return std::all_of(m_words.cbegin(), m_words.cend(), [&text](const QString &word) {
            return text.contains(word, Qt::CaseInsensitive);
        });
    }

private:
    QStringList m_words;
    bool m_old = false;
    QString m_current;
};

namespace {

/* The list's frame: a line around it, which a popup of the style's own
   (a combo box's, a menu's) gets from the style, and a QFrame does not */
class PopupFrame : public QFrame
{
public:
    using QFrame::QFrame;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QFrame::paintEvent(event);
        QPainter p(this);
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

/* The families of the list, in its order */
enum Family { Linux, Windows, Bsd, MacOs, Others, Families };

Family familyOf(const QString &family)
{
    if (family == "linux") {
        return Linux;
    }
    if (family == "winnt" || family == "win9x" || family == "win16") {
        return Windows;
    }
    if (family.endsWith("bsd")) {
        return Bsd;
    }
    return family == "darwin" ? MacOs : Others;
}

QString familyName(Family f)
{
    switch (f) {
    case Linux:
        return OsChooser::tr("Linux");
    case Windows:
        return OsChooser::tr("Windows");
    case Bsd:
        return OsChooser::tr("BSD");
    case MacOs:
        return OsChooser::tr("macOS");
    default:
        return OsChooser::tr("Other systems");
    }
}

/* Its releases newest first, the distribution's other releases last */
bool newerFirst(const GuestOs::Os &a, const GuestOs::Os &b)
{
    static const QRegularExpression number("^\\d+(\\.\\d+)*$");
    /* the development ones (Rawhide, testing) first, then the numbered */
    auto rank = [](const GuestOs::Os &os) {
        return os.isGeneric() ? 2 : number.match(os.version).hasMatch() ? 1 : 0;
    };
    if (rank(a) != rank(b)) {
        return rank(a) < rank(b);
    }
    if (rank(a) == 1) {
        const QStringList x = a.version.split('.');
        const QStringList y = b.version.split('.');
        for (qsizetype i = 0; i < qMax(x.size(), y.size()); i++) {
            if (x.value(i).toInt() != y.value(i).toInt()) {
                return x.value(i).toInt() > y.value(i).toInt();
            }
        }
    }
    if (a.released != b.released) {
        return a.released > b.released;
    }
    return a.name.localeAwareCompare(b.name) < 0;
}

QStandardItem *systemRow(const GuestOs::Os &os, const QString &distro, const QString &family,
                         const QDate &today)
{
    auto *item = new QStandardItem(Systems::osName(os));

    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setData(os.id, OsChooser::IdRole);
    item->setData(GuestOs::guestFamily(os), OsChooser::FamilyRole);
    item->setData(QStringList({Systems::osName(os), os.id, os.aliases.join(' '), distro, family,
                               os.version, os.editions.join(' ')})
                      .join(' '),
                  OsChooser::SearchRole);
    item->setData(os.isEol(today), OsChooser::EolRole);
    return item;
}

}

OsChooser::OsChooser(QWidget *parent) : QComboBox(parent)
{
    /* one row, the system shown; the list is the popup's */
    addItem(QString());
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(22);
    setSystem({});
}

void OsChooser::setSystem(const QString &id, const QString &family)
{
    const GuestOs::Os os = GuestOs::Catalogue::instance().find(id);
    QString name;

    m_id = id;
    m_family = os.isNull() ? family : GuestOs::guestFamily(os);
    name = os.isNull() ? Systems::name({m_family, {}, id}) : Systems::osName(os);
    setItemText(0, name.isEmpty() ? tr("Not set") : name);
    setItemIcon(0, Systems::icon(os, m_family));
    setToolTip(id);
}

void OsChooser::makePopup()
{
    if (m_popup) {
        return;
    }
    m_popup = new PopupFrame(this, Qt::Popup);
    m_popup->setObjectName("systems");
    /* the click that closes it on the field does not open it again */
    m_popup->setAttribute(Qt::WA_NoMouseReplay);
    m_search = new QLineEdit;
    m_search->setObjectName("search");
    m_search->setPlaceholderText(tr("Search, e.g. fedora 44"));
    m_search->setClearButtonEnabled(true);
    m_tree = new QTreeView;
    m_tree->setObjectName("list");
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_old = new QCheckBox(tr("Show releases no longer supported"));
    m_old->setObjectName("old");
    m_model = new QStandardItemModel(this);
    m_filter = new SystemFilter(this);
    m_filter->setRecursiveFilteringEnabled(true);
    m_filter->setSourceModel(m_model);
    m_tree->setModel(m_filter);

    auto *layout = new QVBoxLayout(m_popup);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_old);

    connect(m_search, &QLineEdit::textChanged, this, &OsChooser::filter);
    connect(m_old, &QCheckBox::toggled, this, &OsChooser::filter);
    connect(m_tree, &QTreeView::clicked, this, &OsChooser::activate);
    m_search->installEventFilter(this);
    m_tree->installEventFilter(this);
}

void OsChooser::fill()
{
    const GuestOs::Catalogue &catalogue = GuestOs::Catalogue::instance();
    const QDate today = QDate::currentDate();
    QList<GuestOs::Os> systems = catalogue.systems();
    /* by family, then by distribution: its name in lower case, its name, its releases */
    QMap<QString, std::pair<QString, QList<GuestOs::Os>>> groups[Families];

    /* the one shown, a release newer than those of the list (fedora45) */
    if (const GuestOs::Os shown = catalogue.find(m_id);
        !shown.isNull() && std::none_of(systems.cbegin(), systems.cend(),
                                        [&shown](const auto &os) { return os.id == shown.id; })) {
        systems << shown;
    }
    for (const GuestOs::Os &os : std::as_const(systems)) {
        /* the database's generic ones (Generic Linux 2024): the families'
           rows stand for them */
        if (os.distro.isEmpty()) {
            continue;
        }
        /* Windows NT 3.x and 4.0 are Windows too */
        const QString distro = catalogue.distroName(os.distro == "winnt" ? "win" : os.distro);
        auto &group = groups[familyOf(os.family)][distro.toLower()];
        group.first = distro;
        group.second << os;
    }

    m_model->clear();
    for (int f = Linux; f < Families; f++) {
        auto *family = new QStandardItem(familyName(Family(f)));
        QFont bold = font();

        bold.setBold(true);
        family->setFont(bold);
        family->setFlags(Qt::ItemIsEnabled);
        family->setData(familyName(Family(f)), SearchRole);
        for (auto [distro, releases] : std::as_const(groups[f])) {
            std::sort(releases.begin(), releases.end(), newerFirst);
            const QIcon icon = Systems::icon(releases.first());
            /* a family of one distribution: its releases right there */
            if (groups[f].size() == 1) {
                for (const GuestOs::Os &os : std::as_const(releases)) {
                    family->appendRow(systemRow(os, distro, familyName(Family(f)), today));
                }
                continue;
            }
            if (releases.size() == 1) {
                QStandardItem *row =
                    systemRow(releases.first(), distro, familyName(Family(f)), today);
                row->setIcon(icon);
                /* "Zorin OS", rather than its other release among none */
                if (releases.first().isGeneric()) {
                    row->setText(distro);
                }
                family->appendRow(row);
                continue;
            }
            auto *group = new QStandardItem(icon, distro);
            group->setFlags(Qt::ItemIsEnabled);
            group->setData(distro, SearchRole);
            for (const GuestOs::Os &os : std::as_const(releases)) {
                group->appendRow(systemRow(os, distro, familyName(Family(f)), today));
            }
            family->appendRow(group);
        }
        /* the family alone, for a system the list has not */
        if (f == Linux || f == Windows || f == Others) {
            const QString guest = f == Linux ? "linux" : f == Windows ? "windows" : "other";
            auto *alone = new QStandardItem(Systems::icon({}, guest),
                                            f == Linux     ? tr("Another distribution")
                                            : f == Windows ? tr("Another version")
                                                           : tr("Another system"));
            alone->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            alone->setData(QString(), IdRole);
            alone->setData(guest, FamilyRole);
            alone->setData(alone->text() + ' ' + familyName(Family(f)), SearchRole);
            family->appendRow(alone);
        }
        m_model->appendRow(family);
    }
}

void OsChooser::filter()
{
    const QString text = m_search->text().simplified();

    m_filter->set(text.split(' ', Qt::SkipEmptyParts), m_old->isChecked(), m_id);
    m_tree->collapseAll();
    if (!text.isEmpty()) {
        /* what the search found, all of it */
        m_tree->expandAll();
        m_tree->setCurrentIndex(firstSystem());
        return;
    }
    /* the families, and the one shown */
    for (int f = 0; f < m_filter->rowCount(); f++) {
        m_tree->expand(m_filter->index(f, 0));
    }
    std::function<QModelIndex(const QModelIndex &)> shown = [&](const QModelIndex &parent) {
        for (int r = 0; r < m_filter->rowCount(parent); r++) {
            const QModelIndex i = m_filter->index(r, 0, parent);
            if (m_filter->hasChildren(i)) {
                if (const QModelIndex found = shown(i); found.isValid()) {
                    return found;
                }
            } else if (i.data(IdRole).toString() == m_id &&
                       (!m_id.isEmpty() || i.data(FamilyRole).toString() == m_family)) {
                return i;
            }
        }
        return QModelIndex();
    };
    const QModelIndex current = shown({});
    if (current.isValid()) {
        for (QModelIndex p = current.parent(); p.isValid(); p = p.parent()) {
            m_tree->expand(p);
        }
        m_tree->setCurrentIndex(current);
        m_tree->scrollTo(current, QAbstractItemView::PositionAtCenter);
    }
}

QModelIndex OsChooser::firstSystem(const QModelIndex &parent) const
{
    for (int r = 0; r < m_filter->rowCount(parent); r++) {
        const QModelIndex i = m_filter->index(r, 0, parent);
        if (!m_filter->hasChildren(i)) {
            return i;
        }
        if (const QModelIndex found = firstSystem(i); found.isValid()) {
            return found;
        }
    }
    return {};
}

void OsChooser::showPopup()
{
    makePopup();
    fill();
    {
        const QSignalBlocker block(m_search);
        m_search->clear();
    }
    filter();

    /* under the field, or over it where the screen has no room under it */
    const QRect screen = (window()->screen() ? window()->screen() : QGuiApplication::primaryScreen())
                             ->availableGeometry();
    const QSize size(qMax(width(), Widgets::em(this) * 26),
                     qMin(Widgets::em(this) * 24, screen.height()));
    QPoint at = mapToGlobal(QPoint(0, height()));
    if (at.y() + size.height() > screen.bottom() &&
        mapToGlobal(QPoint(0, 0)).y() - size.height() >= screen.top()) {
        at = mapToGlobal(QPoint(0, -size.height()));
    }
    m_popup->setGeometry(QRect(at, size));
    m_popup->show();
    m_search->setFocus(Qt::PopupFocusReason);
}

void OsChooser::hidePopup()
{
    if (m_popup) {
        m_popup->hide();
    }
}

void OsChooser::activate(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    if (m_filter->hasChildren(index)) {
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
        return;
    }
    setSystem(index.data(IdRole).toString(), index.data(FamilyRole).toString());
    hidePopup();
    emit systemChosen();
}

bool OsChooser::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress) {
        return QComboBox::eventFilter(watched, event);
    }
    const int key = static_cast<QKeyEvent *>(event)->key();
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        /* the row the keys are on, else the first the search found */
        const QModelIndex current = m_tree->currentIndex();
        activate(current.isValid() ? current : firstSystem());
        return true;
    }
    if (watched == m_search &&
        (key == Qt::Key_Down || key == Qt::Key_Up || key == Qt::Key_PageDown ||
         key == Qt::Key_PageUp)) {
        /* through the list, the search kept */
        m_tree->setFocus();
        if (!m_tree->currentIndex().isValid()) {
            m_tree->setCurrentIndex(firstSystem());
        }
        return true;
    }
    return QComboBox::eventFilter(watched, event);
}
