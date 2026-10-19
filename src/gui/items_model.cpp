#include "gui/items_model.h"

#include <QApplication>
#include <QCollator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPalette>
#include <QUrl>

#include <algorithm>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"

namespace on::gui {

ItemsModel::ItemsModel(QObject* parent) : QAbstractItemModel(parent) {}

void ItemsModel::setScope(const Scope& scope)
{
    scope_ = scope;
    reload();
}

void ItemsModel::setSearch(const QString& text, SearchMode mode)
{
    if (text == search_ && mode == mode_)
        return;
    search_ = text;
    mode_ = mode;
    reload();
}

void ItemsModel::setTagFilter(const QStringList& tags)
{
    if (tags == tags_)
        return;
    tags_ = tags;
    reload();
}

QString ItemsModel::columnName(int column)
{
    switch (column) {
    case Title: return tr("Title");
    case Creator: return tr("Creator");
    case Year: return tr("Year");
    case ItemType: return tr("Item Type");
    case Date: return tr("Date");
    case Publication: return tr("Publication");
    case DateAdded: return tr("Date Added");
    case DateModified: return tr("Date Modified");
    case Attachments: return tr("Attachments");
    case Notes: return tr("Notes");
    }
    return {};
}

void ItemsModel::reload()
{
    beginResetModel();
    roots_.clear();
    byKey_.clear();
    if (!scope_.libraryId) {
        endResetModel();
        return;
    }
    auto& lib = AppContext::get().library();
    auto rows = lib.itemRows(scope_.libraryId);
    std::map<std::string, ItemRow*> rowByKey;
    for (auto& r : rows)
        rowByKey[r.key] = &r;
    auto topOf = [&](const std::string& key) {
        auto it = rowByKey.find(key);
        if (it == rowByKey.end())
            return lib.topLevelKey(scope_.libraryId, key);  // annotations are not in rows
        std::string cur = key;
        for (int i = 0; i < 8; ++i) {
            auto r = rowByKey.find(cur);
            if (r == rowByKey.end() || r->second->parentKey.empty())
                break;
            cur = r->second->parentKey;
        }
        return cur;
    };

    bool trashView = scope_.kind == ScopeKind::Trash;
    std::set<std::string> scopeKeys;
    bool useScope = true;
    switch (scope_.kind) {
    case ScopeKind::Collection: {
        bool recursive = AppContext::get().config().get("/ui/showSubcollectionItems", false).get<bool>();
        scopeKeys = lib.collectionItemKeys(scope_.libraryId, scope_.collectionKey, recursive);
        break;
    }
    case ScopeKind::Unfiled:
        scopeKeys = lib.unfiledItemKeys(scope_.libraryId);
        break;
    case ScopeKind::Duplicates:
        for (auto& group : lib.duplicateGroups(scope_.libraryId))
            scopeKeys.insert(group.begin(), group.end());
        break;
    default:
        useScope = false;
    }

    // Matching keys (any level) and the top-level items they belong to.
    std::set<std::string> matched, matchedTop;
    bool filtering = !search_.trimmed().isEmpty() || !tags_.isEmpty();
    if (filtering) {
        std::set<std::string> keys;
        bool first = true;
        if (!search_.trimmed().isEmpty()) {
            keys = lib.search(scope_.libraryId, ss(search_), mode_);
            first = false;
        }
        if (!tags_.isEmpty()) {
            auto tagged = lib.itemKeysWithTags(scope_.libraryId, toStdVector(tags_));
            if (first) {
                keys = tagged;
            } else {
                // a top-level item matches when both filters match somewhere in its tree
                std::set<std::string> tops1, tops2;
                for (auto& k : keys)
                    tops1.insert(topOf(k));
                for (auto& k : tagged)
                    tops2.insert(topOf(k));
                std::set<std::string> both;
                for (auto& k : keys)
                    if (tops2.count(topOf(k)))
                        both.insert(k);
                for (auto& k : tagged)
                    if (tops1.count(topOf(k)))
                        both.insert(k);
                keys = both;
            }
        }
        matched = keys;
        for (auto& k : keys)
            matchedTop.insert(topOf(k));
    }

    auto parentTrashed = [&](const ItemRow& r) {
        auto it = rowByKey.find(r.parentKey);
        return it != rowByKey.end() && it->second->trashed;
    };

    for (auto& r : rows) {
        bool topLevel = r.parentKey.empty() || !rowByKey.count(r.parentKey);
        if (trashView) {
            // trashed top-level items, plus trashed children of items that are not in the trash
            if (!r.trashed || (!topLevel && parentTrashed(r)))
                continue;
        } else {
            if (!topLevel || r.trashed)
                continue;
            if (useScope && !scopeKeys.count(r.key))
                continue;
        }
        if (filtering && !matchedTop.count(r.key) && !matched.count(r.key))
            continue;
        auto node = std::make_unique<Node>();
        node->row = r;
        node->matches = !filtering || matched.count(r.key);
        if (trashView && !topLevel)  // a trashed "PDF" alone does not say whose it is
            node->context = qs(rowByKey.at(r.parentKey)->title);
        byKey_[r.key] = node.get();
        roots_.push_back(std::move(node));
    }

    // children
    for (auto& r : rows) {
        if (r.parentKey.empty())
            continue;
        auto parentIt = byKey_.find(r.parentKey);
        if (parentIt == byKey_.end() || parentIt->second->parent)
            continue;
        Node* parent = parentIt->second;
        if (r.trashed && !trashView)
            continue;
        if (trashView && r.trashed && !parent->row.trashed)
            continue;  // shown as its own row
        auto child = std::make_unique<Node>();
        child->row = r;
        child->parent = parent;
        child->matches = !filtering || matched.count(r.key);
        byKey_[r.key] = child.get();
        parent->children.push_back(std::move(child));
    }

    // per-item summaries: note count and the attachment shown in the Attachments column
    for (auto& root : roots_) {
        int bestScore = -1;
        for (auto& c : root->children) {
            if (c->row.itemType == "note") {
                ++root->notes;
                continue;
            }
            if (c->row.itemType != "attachment")
                continue;
            const auto& ct = c->row.contentType;
            int score = ct == "application/pdf" ? 3 : ct == "application/epub+zip" ? 2 : c->row.linkMode == "linked_url" ? 0 : 1;
            if (score > bestScore) {
                bestScore = score;
                root->bestType = "attachment";
                root->bestContent = qs(ct);
                root->bestLinkMode = qs(c->row.linkMode);
                if (c->row.linkMode != "linked_url") {
                    auto data = lib.item(scope_.libraryId, c->row.key);
                    c->fileMissing = data && !lib.attachmentFileExists(scope_.libraryId, *data);
                    root->fileMissing = c->fileMissing;
                }
            }
        }
        if (root->row.itemType == "attachment" && root->row.linkMode != "linked_url") {
            auto data = lib.item(scope_.libraryId, root->row.key);
            root->fileMissing = data && !lib.attachmentFileExists(scope_.libraryId, *data);
        }
    }
    sortNodes(roots_);
    for (auto& root : roots_)
        std::stable_sort(root->children.begin(), root->children.end(), [](auto& a, auto& b) {
            if (a->row.itemType != b->row.itemType)
                return a->row.itemType < b->row.itemType;  // attachments before notes
            return normalize(a->row.title) < normalize(b->row.title);
        });
    endResetModel();
}

QVariant ItemsModel::sortValue(const Node& n, int column) const
{
    switch (column) {
    case Title: return qs(normalize(n.row.title));
    case Creator: return qs(normalize(n.row.creator)) + "\x01" + qs(n.row.year) + "\x01" + qs(normalize(n.row.title));
    case Year: return qs(n.row.year);
    case ItemType: return qs(schema::typeLabel(n.row.itemType));
    case Date: return qs(n.row.date);
    case Publication: return qs(normalize(n.row.publication));
    case DateAdded: return qs(n.row.dateAdded);
    case DateModified: return qs(n.row.dateModified);
    case Attachments: return n.bestContent.isEmpty() ? 0 : n.fileMissing ? 1 : 2;
    case Notes: return n.notes;
    }
    return {};
}

void ItemsModel::sortNodes(std::vector<std::unique_ptr<Node>>& nodes)
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    int col = sortColumn_;
    bool desc = sortOrder_ == Qt::DescendingOrder;
    std::stable_sort(nodes.begin(), nodes.end(), [&](const auto& a, const auto& b) {
        auto va = sortValue(*a, col), vb = sortValue(*b, col);
        int c;
        if (va.typeId() == QMetaType::Int)
            c = va.toInt() - vb.toInt();
        else
            c = collator.compare(va.toString(), vb.toString());
        if (c == 0)
            c = collator.compare(qs(normalize(a->row.title)), qs(normalize(b->row.title)));
        // empty values always last
        bool ea = va.toString().isEmpty(), eb = vb.toString().isEmpty();
        if (ea != eb)
            return eb;
        return desc ? c > 0 : c < 0;
    });
}

void ItemsModel::sort(int column, Qt::SortOrder order)
{
    sortColumn_ = column;
    sortOrder_ = order;
    emit layoutAboutToBeChanged();
    auto before = persistentIndexList();
    std::vector<std::string> keys;
    for (auto& i : before)
        keys.push_back(nodeAt(i) ? nodeAt(i)->row.key : std::string());
    sortNodes(roots_);
    QModelIndexList after;
    for (size_t i = 0; i < keys.size(); ++i)
        after << (keys[i].empty() ? QModelIndex() : indexOfKey(qs(keys[i])).siblingAtColumn(before[(int)i].column()));
    changePersistentIndexList(before, after);
    emit layoutChanged();
}

const ItemsModel::Node* ItemsModel::nodeAt(const QModelIndex& index) const
{
    return index.isValid() ? static_cast<const Node*>(index.internalPointer()) : nullptr;
}

QString ItemsModel::keyAt(const QModelIndex& index) const
{
    auto n = nodeAt(index);
    return n ? qs(n->row.key) : QString();
}

QString ItemsModel::typeAt(const QModelIndex& index) const
{
    auto n = nodeAt(index);
    return n ? qs(n->row.itemType) : QString();
}

QModelIndex ItemsModel::indexOfKey(const QString& key) const
{
    auto it = byKey_.find(ss(key));
    if (it == byKey_.end())
        return {};
    Node* n = it->second;
    if (!n->parent) {
        for (size_t i = 0; i < roots_.size(); ++i)
            if (roots_[i].get() == n)
                return createIndex((int)i, 0, n);
    } else {
        auto& siblings = n->parent->children;
        for (size_t i = 0; i < siblings.size(); ++i)
            if (siblings[i].get() == n)
                return createIndex((int)i, 0, n);
    }
    return {};
}

QModelIndex ItemsModel::index(int row, int column, const QModelIndex& parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount)
        return {};
    if (!parent.isValid())
        return row < (int)roots_.size() ? createIndex(row, column, roots_[row].get()) : QModelIndex();
    auto p = nodeAt(parent);
    if (!p || row >= (int)p->children.size())
        return {};
    return createIndex(row, column, p->children[row].get());
}

QModelIndex ItemsModel::parent(const QModelIndex& index) const
{
    auto n = nodeAt(index);
    if (!n || !n->parent)
        return {};
    for (size_t i = 0; i < roots_.size(); ++i)
        if (roots_[i].get() == n->parent)
            return createIndex((int)i, 0, n->parent);
    return {};
}

int ItemsModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return (int)roots_.size();
    if (parent.column() != 0)
        return 0;
    auto n = nodeAt(parent);
    return n ? (int)n->children.size() : 0;
}

int ItemsModel::columnCount(const QModelIndex&) const { return ColumnCount; }

QVariant ItemsModel::data(const QModelIndex& index, int role) const
{
    auto n = nodeAt(index);
    if (!n)
        return {};
    const auto& r = n->row;
    int col = index.column();
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Title: {
            auto title = qs(r.title.empty() && r.itemType == "note" ? std::string("(empty note)") : r.title);
            return n->context.isEmpty() ? title : tr("%1 (of “%2”)").arg(title, n->context);
        }
        case Creator: return qs(r.creator);
        case Year: return qs(r.year);
        case ItemType: return qs(schema::typeLabel(r.itemType));
        case Date: return qs(r.date);
        case Publication: return qs(r.publication);
        case DateAdded: return qs(isoToLocalDisplay(r.dateAdded));
        case DateModified: return qs(isoToLocalDisplay(r.dateModified));
        case Notes: return n->notes > 0 ? QVariant(n->notes) : QVariant();
        }
        return {};
    case Qt::DecorationRole:
        if (col == Title)
            return itemTypeIcon(qs(r.itemType), qs(r.contentType), qs(r.linkMode));
        if (col == Attachments) {
            if (r.itemType == "attachment" && !n->parent)
                return {};
            if (n->bestContent.isEmpty() && n->bestLinkMode.isEmpty())
                return {};
            auto ic = itemTypeIcon("attachment", n->bestContent, n->bestLinkMode);
            if (n->fileMissing)
                return QIcon(ic.pixmap(16, QIcon::Disabled));
            return ic;
        }
        return {};
    case Qt::ForegroundRole:
        if (!n->matches)
            return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
        if (r.itemType == "attachment" && n->fileMissing && col == Title)
            return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
        return {};
    case Qt::ToolTipRole:
        if (col == Title)
            return qs(r.title);
        if (col == Attachments && n->fileMissing)
            return tr("The attached file could not be found");
        return {};
    case Qt::TextAlignmentRole:
        if (col == Notes || col == Year)
            return int(Qt::AlignCenter);
        return {};
    case KeyRole: return qs(r.key);
    case TypeRole: return qs(r.itemType);
    case ContentTypeRole: return qs(r.contentType);
    case LinkModeRole: return qs(r.linkMode);
    case SortRole: return sortValue(*n, col);
    }
    return {};
}

QVariant ItemsModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        if (section == Attachments)
            return QString();
        if (section == Notes)
            return QStringLiteral("#");
        return columnName(section);
    }
    if (role == Qt::DecorationRole && section == Attachments)
        return icon("attach");
    if (role == Qt::ToolTipRole)
        return columnName(section);
    return {};
}

Qt::ItemFlags ItemsModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    auto f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    auto n = nodeAt(index);
    if (n && !n->parent && schema::isRegularType(n->row.itemType))
        f |= Qt::ItemIsDropEnabled;
    return f;
}

QStringList ItemsModel::mimeTypes() const { return {kItemsMime, "text/uri-list"}; }

QMimeData* ItemsModel::mimeData(const QModelIndexList& indexes) const
{
    QJsonArray keys;
    QSet<QString> seen;
    for (auto& i : indexes) {
        auto k = keyAt(i);
        if (!k.isEmpty() && !seen.contains(k)) {
            seen.insert(k);
            keys.append(k);
        }
    }
    auto* mime = new QMimeData;
    QJsonObject obj{{"library", (qint64)scope_.libraryId}, {"keys", keys}};
    mime->setData(kItemsMime, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    // dragging attachments out of the application drops their files
    QList<QUrl> urls;
    auto& lib = AppContext::get().library();
    for (auto k : seen) {
        auto it = byKey_.find(ss(k));
        if (it != byKey_.end() && it->second->row.itemType == "attachment")
            if (auto d = lib.item(scope_.libraryId, ss(k))) {
                auto p = lib.attachmentFile(scope_.libraryId, *d);
                if (!p.empty())
                    urls << QUrl::fromLocalFile(qpath(p));
            }
    }
    if (!urls.isEmpty())
        mime->setUrls(urls);
    return mime;
}

Qt::DropActions ItemsModel::supportedDropActions() const { return Qt::CopyAction | Qt::MoveAction; }

QStringList ItemsModel::movableKeys(const QMimeData* data, const QModelIndex& parent, QString* parentKey) const
{
    QStringList keys;
    auto obj = QJsonDocument::fromJson(data->data(kItemsMime)).object();
    if (obj["library"].toInteger() != scope_.libraryId || !AppContext::get().editable(scope_.libraryId))
        return keys;
    QString target;
    if (parent.isValid()) {
        auto* n = nodeAt(parent.siblingAtColumn(0));
        if (!n || n->parent || !schema::isRegularType(n->row.itemType))
            return keys;  // onto a note, an attachment or a child: nothing
        target = qs(n->row.key);
    }
    for (auto k : obj["keys"].toArray()) {
        auto it = byKey_.find(ss(k.toString()));
        if (it == byKey_.end())
            continue;
        auto& row = it->second->row;
        if (row.itemType != "note" && row.itemType != "attachment")
            continue;
        if (target.isEmpty() ? !row.parentKey.empty() : qs(row.parentKey) != target && qs(row.key) != target)
            keys << qs(row.key);
    }
    *parentKey = target;
    return keys;
}

bool ItemsModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const
{
    QString target;
    if (data->hasFormat(kItemsMime))
        return !movableKeys(data, parent, &target).isEmpty();
    return data->hasUrls();
}

bool ItemsModel::dropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent)
{
    if (data->hasFormat(kItemsMime)) {
        QString target;
        auto keys = movableKeys(data, parent, &target);
        if (keys.isEmpty())
            return false;
        emit itemsMoved(keys, target);
        return true;
    }
    if (!data->hasUrls())
        return false;
    QStringList files;
    for (auto& url : data->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile())
            files << url.toLocalFile();
    if (files.isEmpty())
        return false;
    emit filesDropped(files, parent.isValid() ? keyAt(parent.siblingAtColumn(0)) : QString());
    return true;
}

} // namespace on::gui
