#include "gui/collection_tree.h"

#include <QCollator>
#include <QContextMenuEvent>
#include <QDrag>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QScrollBar>

#include <functional>
#include <map>

#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"

namespace on::gui {

static const char* kCollectionMime = "application/x-ournotes-collection";

CollectionTree::CollectionTree(QWidget* parent) : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setColumnCount(1);
    setSelectionMode(SingleSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(DragDrop);
    // scrolled while a drag is near the top or the bottom (see dragScroll)
    setAutoScroll(false);
    setVerticalScrollMode(ScrollPerPixel);
    dragScrollTimer_.setInterval(16);
    connect(&dragScrollTimer_, &QTimer::timeout, this, &CollectionTree::dragScroll);
    setEditTriggers(NoEditTriggers);
    setUniformRowHeights(true);
    setIconSize(QSize(16, 16));
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
        if (cur)
            emit scopeChanged(scopeOf(cur));
    });
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64, const QString& type, const QString&, const QStringList&) {
                if (type == "collection" || type == "library" || type == "*")
                    reload();
            });
}

QTreeWidgetItem* CollectionTree::addScopeItem(QTreeWidgetItem* parent, const QString& text, const QIcon& icon,
                                              const Scope& s)
{
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
    item->setText(0, text);
    item->setIcon(0, icon);
    item->setData(0, KindRole, (int)s.kind);
    item->setData(0, LibraryRole, (qint64)s.libraryId);
    item->setData(0, CollectionRole, qs(s.collectionKey));
    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (s.kind == ScopeKind::Collection)
        flags |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    if (s.kind == ScopeKind::Library || s.kind == ScopeKind::Trash)
        flags |= Qt::ItemIsDropEnabled;
    item->setFlags(flags);
    return item;
}

Scope CollectionTree::scopeOf(const QTreeWidgetItem* item) const
{
    Scope s;
    if (!item)
        return s;
    s.kind = (ScopeKind)item->data(0, KindRole).toInt();
    s.libraryId = item->data(0, LibraryRole).toLongLong();
    s.collectionKey = ss(item->data(0, CollectionRole).toString());
    return s;
}

Scope CollectionTree::currentScope() const { return scopeOf(currentItem()); }

QTreeWidgetItem* CollectionTree::findItem(const Scope& s) const
{
    QTreeWidgetItemIterator it(const_cast<CollectionTree*>(this));
    for (; *it; ++it)
        if (scopeOf(*it) == s)
            return *it;
    return nullptr;
}

void CollectionTree::selectScope(const Scope& s)
{
    if (auto* item = findItem(s)) {
        setCurrentItem(item);
        scrollToItem(item);
    }
}

QStringList CollectionTree::expandedState() const
{
    QStringList out;
    QTreeWidgetItemIterator it(const_cast<CollectionTree*>(this));
    for (; *it; ++it)
        if ((*it)->isExpanded()) {
            auto s = scopeOf(*it);
            out << QString("%1:%2").arg(s.libraryId).arg(qs(s.collectionKey));
        }
    return out;
}

void CollectionTree::restoreExpanded(const QStringList& state)
{
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it) {
        auto s = scopeOf(*it);
        if (s.kind == ScopeKind::Library || s.kind == ScopeKind::Collection)
            (*it)->setExpanded(state.contains(QString("%1:%2").arg(s.libraryId).arg(qs(s.collectionKey))));
    }
}

void CollectionTree::reload()
{
    auto& lib = AppContext::get().library();
    Scope current = currentScope();
    std::set<std::pair<int64_t, std::string>> expanded;
    QTreeWidgetItemIterator it(this);
    for (; *it; ++it)
        if ((*it)->isExpanded()) {
            auto s = scopeOf(*it);
            expanded.insert({s.libraryId, s.collectionKey});
        }
    bool firstLoad = topLevelItemCount() == 0;

    QSignalBlocker block(this);
    clear();
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    for (auto& info : lib.libraries()) {
        auto* root = addScopeItem(nullptr, qs(info.name), icon(info.type == "group" ? "group" : "library"),
                                  {info.id, ScopeKind::Library, {}});
        auto colls = lib.collections(info.id);
        std::map<std::string, std::vector<Json>> byParent;
        for (auto& c : colls) {
            auto p = c.value("parentCollection", Json());
            byParent[p.is_string() ? p.get<std::string>() : std::string()].push_back(c);
        }
        std::function<void(QTreeWidgetItem*, const std::string&, int)> addChildren =
            [&](QTreeWidgetItem* parent, const std::string& key, int depth) {
                auto it = byParent.find(key);
                if (it == byParent.end() || depth > 32)
                    return;
                auto children = it->second;
                std::sort(children.begin(), children.end(), [&](const Json& a, const Json& b) {
                    return collator.compare(jqs(a, "name"), jqs(b, "name")) < 0;
                });
                for (auto& c : children) {
                    auto ckey = jstr(c, "key");
                    auto* item = addScopeItem(parent, jqs(c, "name"), icon("folder"),
                                              {info.id, ScopeKind::Collection, ckey});
                    addChildren(item, ckey, depth + 1);
                    if (expanded.count({info.id, ckey}))
                        item->setExpanded(true);
                }
            };
        addChildren(root, "", 0);
        addScopeItem(root, tr("Duplicate Items"), icon("duplicates"), {info.id, ScopeKind::Duplicates, {}});
        addScopeItem(root, tr("Unfiled Items"), icon("unfiled"), {info.id, ScopeKind::Unfiled, {}});
        addScopeItem(root, tr("Trash"), icon("trash"), {info.id, ScopeKind::Trash, {}});
        root->setExpanded(firstLoad || expanded.count({info.id, ""}));
    }
    auto* sel = findItem(current);
    if (!sel && topLevelItemCount())
        sel = topLevelItem(0);
    setCurrentItem(sel);
    block.unblock();
    if (sel && !(scopeOf(sel) == current))
        emit scopeChanged(scopeOf(sel));
}

// --- editing ------------------------------------------------------------------------------

void CollectionTree::newCollection(int64_t lib, const std::string& parentKey)
{
    bool ok = false;
    auto name = QInputDialog::getText(this, tr("New Collection"), tr("Name:"), QLineEdit::Normal,
                                      tr("New Collection"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    auto c = AppContext::get().library().createCollection(lib, ss(name.trimmed()), parentKey);
    selectScope({lib, ScopeKind::Collection, jstr(c, "key")});
}

void CollectionTree::renameItem(QTreeWidgetItem* item)
{
    auto s = scopeOf(item);
    auto& lib = AppContext::get().library();
    bool ok = false;
    auto name = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal, item->text(0), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    if (s.kind == ScopeKind::Library) {
        lib.renameLibrary(s.libraryId, ss(name.trimmed()));
    } else if (auto c = lib.collection(s.libraryId, s.collectionKey)) {
        (*c)["name"] = ss(name.trimmed());
        lib.saveCollection(s.libraryId, *c);
    }
}

void CollectionTree::deleteCollection(QTreeWidgetItem* item, bool trashItems)
{
    auto s = scopeOf(item);
    auto text = trashItems
                    ? tr("Delete the collection \"%1\", its subcollections and move all their items to the trash?")
                    : tr("Delete the collection \"%1\" and its subcollections? Items stay in the library.");
    if (QMessageBox::question(this, tr("Delete Collection"), asPlainText(text.arg(item->text(0)))) != QMessageBox::Yes)
        return;
    AppContext::get().library().deleteCollection(s.libraryId, s.collectionKey, trashItems);
}

void CollectionTree::contextMenuEvent(QContextMenuEvent* e)
{
    auto* item = itemAt(e->pos());
    QMenu menu(this);
    auto* ctx = &AppContext::get();
    if (!item) {
        menu.addAction(tr("New Library…"), this, &CollectionTree::newLibraryRequested);
        menu.exec(e->globalPos());
        return;
    }
    auto s = scopeOf(item);
    bool editable = ctx->editable(s.libraryId);
    switch (s.kind) {
    case ScopeKind::Library: {
        menu.addAction(icon("new-collection"), tr("New Collection…"), this, [=] { newCollection(s.libraryId, {}); })
            ->setEnabled(editable);
        menu.addAction(tr("Rename Library…"), this, [=] { renameItem(item); });
        menu.addSeparator();
        menu.addAction(icon("export"), tr("Export Library…"), this, [=] { emit exportRequested(s); });
        menu.addAction(tr("Create Bibliography from Library…"), this, [=] { emit bibliographyRequested(s); });
        menu.addSeparator();
        menu.addAction(tr("New Library…"), this, &CollectionTree::newLibraryRequested);
        menu.addAction(tr("Delete Library…"), this, [=] { emit deleteLibraryRequested(s.libraryId); });
        break;
    }
    case ScopeKind::Collection:
        menu.addAction(icon("new-collection"), tr("New Subcollection…"), this,
                       [=] { newCollection(s.libraryId, s.collectionKey); })
            ->setEnabled(editable);
        menu.addAction(tr("Rename Collection…"), this, [=] { renameItem(item); })->setEnabled(editable);
        menu.addSeparator();
        menu.addAction(icon("export"), tr("Export Collection…"), this, [=] { emit exportRequested(s); });
        menu.addAction(tr("Create Bibliography from Collection…"), this, [=] { emit bibliographyRequested(s); });
        menu.addSeparator();
        menu.addAction(tr("Delete Collection…"), this, [=] { deleteCollection(item, false); })->setEnabled(editable);
        menu.addAction(tr("Delete Collection and Items…"), this, [=] { deleteCollection(item, true); })
            ->setEnabled(editable);
        break;
    case ScopeKind::Trash:
        menu.addAction(tr("Empty Trash…"), this, [=] {
            if (QMessageBox::question(this, tr("Empty Trash"),
                                      tr("Permanently delete all items in the trash?")) == QMessageBox::Yes)
                ctx->library().emptyTrash(s.libraryId);
        })->setEnabled(editable);
        break;
    default:
        return;
    }
    menu.exec(e->globalPos());
}

// --- drag and drop --------------------------------------------------------------------------

void CollectionTree::startDrag(Qt::DropActions)
{
    auto* item = currentItem();
    auto s = scopeOf(item);
    if (!item || s.kind != ScopeKind::Collection)
        return;
    auto* mime = new QMimeData;
    QJsonObject obj{{"library", (qint64)s.libraryId}, {"key", qs(s.collectionKey)}};
    mime->setData(kCollectionMime, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(item->icon(0).pixmap(16));
    drag->exec(Qt::MoveAction);
}

bool CollectionTree::acceptsDrop(QDropEvent* e, QTreeWidgetItem** target) const
{
    auto* item = itemAt(e->position().toPoint());
    *target = item;
    if (!item)
        return false;
    auto s = scopeOf(item);
    auto* mime = e->mimeData();
    if (mime->hasFormat(kItemsMime)) {
        auto obj = QJsonDocument::fromJson(mime->data(kItemsMime)).object();
        if (obj["library"].toInteger() != s.libraryId)  // into another library: copied there
            return (s.kind == ScopeKind::Library || s.kind == ScopeKind::Collection || s.kind == ScopeKind::Unfiled) &&
                   AppContext::get().editable(s.libraryId);
        return s.kind == ScopeKind::Collection || s.kind == ScopeKind::Trash;
    }
    if (mime->hasFormat(kCollectionMime)) {
        auto obj = QJsonDocument::fromJson(mime->data(kCollectionMime)).object();
        if (obj["library"].toInteger() != s.libraryId)
            return false;
        auto key = ss(obj["key"].toString());
        if (s.kind == ScopeKind::Library)
            return true;
        if (s.kind != ScopeKind::Collection || s.collectionKey == key)
            return false;
        // no dropping into own descendants
        for (auto& d : AppContext::get().library().descendantCollections(s.libraryId, key))
            if (d == s.collectionKey)
                return false;
        return true;
    }
    if (!droppedFiles(mime).isEmpty())  // from outside: into the library or a collection
        return (s.kind == ScopeKind::Library || s.kind == ScopeKind::Collection || s.kind == ScopeKind::Unfiled) &&
               AppContext::get().editable(s.libraryId);
    return false;
}

QStringList CollectionTree::droppedFiles(const QMimeData* mime)
{
    QStringList files;
    if (mime->hasFormat(kItemsMime) || mime->hasFormat(kCollectionMime))
        return files;
    for (auto& url : mime->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile())
            files << url.toLocalFile();
    return files;
}

void CollectionTree::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(kItemsMime) || e->mimeData()->hasFormat(kCollectionMime) ||
        !droppedFiles(e->mimeData()).isEmpty())
        e->acceptProposedAction();
    else
        e->ignore();
}

void CollectionTree::dragMoveEvent(QDragMoveEvent* e)
{
    dragPos_ = e->position().toPoint();
    dragMime_ = e->mimeData();
    dragActions_ = e->possibleActions();
    if (dragScrollSpeed(dragPos_.y()) != 0 && !dragScrollTimer_.isActive()) {
        dragScrollClock_.start();
        dragScrollCarry_ = 0;
        dragScrollTimer_.start();
    }
    QTreeWidgetItem* target = nullptr;
    if (acceptsDrop(e, &target)) {
        e->acceptProposedAction();
        setDropTarget(target);
    } else {
        e->ignore();
        setDropTarget(nullptr);
    }
}

void CollectionTree::dragLeaveEvent(QDragLeaveEvent* e)
{
    QTreeWidget::dragLeaveEvent(e);
    stopDragScroll();
    setDropTarget(nullptr);
}

int CollectionTree::rowHeight() const
{
    int row = topLevelItemCount() ? visualItemRect(topLevelItem(0)).height() : 0;
    return row > 0 ? row : fontMetrics().height() + 8;
}

double CollectionTree::dragScrollSpeed(int y) const
{
    int row = rowHeight(), height = viewport()->height();
    int zone = std::min(4 * row, height / 3);  // (a small tree is not all edge)
    double into = y < zone ? zone - y : y > height - zone ? y - (height - zone) : 0;
    if (zone <= 0 || into <= 0)
        return 0;
    double near = std::min(into / zone, 1.0);  // 0 where the zone begins, 1 at the edge
    double rows = 2 + 23 * near * near;        // rows a second: 2, up to 25
    return (y < zone ? -1 : 1) * rows * row;
}

void CollectionTree::dragScroll()
{
    auto* bar = verticalScrollBar();
    double speed = dragMime_ ? dragScrollSpeed(dragPos_.y()) : 0;
    double seconds = std::min(dragScrollClock_.restart() / 1000.0, 0.1);
    if (speed == 0 || bar->value() == (speed < 0 ? bar->minimum() : bar->maximum())) {
        dragScrollTimer_.stop();  // (again with the next move into a zone)
        return;
    }
    dragScrollCarry_ += speed * seconds;
    int step = int(dragScrollCarry_);
    dragScrollCarry_ -= step;
    bar->setValue(bar->value() + step);
    // what is under the drag now
    QDragMoveEvent at(dragPos_, dragActions_, dragMime_, Qt::LeftButton, Qt::NoModifier);
    QTreeWidgetItem* target = nullptr;
    setDropTarget(acceptsDrop(&at, &target) ? target : nullptr);
}

void CollectionTree::stopDragScroll()
{
    dragScrollTimer_.stop();
    dragMime_ = nullptr;
}

void CollectionTree::setDropTarget(QTreeWidgetItem* item)
{
    QModelIndex index = item ? indexFromItem(item) : QModelIndex();
    if (index == dropTarget_)
        return;
    dropTarget_ = index;
    viewport()->update();
}

// At a fractional scale a row's edge falls inside a pixel it shares with the row above:
// redrawing only the rows whose selection changed left a line of the old highlight behind.
void CollectionTree::selectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
    QTreeWidget::selectionChanged(selected, deselected);
    viewport()->update();
}

void CollectionTree::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    QTreeWidget::currentChanged(current, previous);
    viewport()->update();
}

void CollectionTree::drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyleOptionViewItem o(option);
    if (dropTarget_.isValid() && index.siblingAtColumn(0) == dropTarget_)
        o.state |= QStyle::State_Selected;  // where the drag goes
    QTreeWidget::drawRow(painter, o, index);
}

void CollectionTree::dropEvent(QDropEvent* e)
{
    stopDragScroll();
    setDropTarget(nullptr);
    QTreeWidgetItem* target = nullptr;
    if (!acceptsDrop(e, &target)) {
        e->ignore();
        return;
    }
    auto s = scopeOf(target);
    auto& lib = AppContext::get().library();
    auto* mime = e->mimeData();
    if (auto files = droppedFiles(mime); !files.isEmpty()) {
        e->acceptProposedAction();
        emit filesDropped(s, files);
        return;
    }
    if (mime->hasFormat(kItemsMime)) {
        auto obj = QJsonDocument::fromJson(mime->data(kItemsMime)).object();
        std::vector<std::string> keys;
        for (auto k : obj["keys"].toArray())
            keys.push_back(ss(k.toString()));
        if (qint64 from = obj["library"].toInteger(); from != s.libraryId) {
            lib.copyItems(from, keys, s.libraryId, s.kind == ScopeKind::Collection ? s.collectionKey : std::string());
            e->setDropAction(Qt::CopyAction);  // where they came from, they stay
            e->accept();
            return;
        }
        if (s.kind == ScopeKind::Trash)
            lib.trashItems(s.libraryId, keys);
        else
            lib.addToCollection(s.libraryId, s.collectionKey, keys);
    } else {
        auto obj = QJsonDocument::fromJson(mime->data(kCollectionMime)).object();
        if (auto c = lib.collection(s.libraryId, ss(obj["key"].toString()))) {
            (*c)["parentCollection"] = s.kind == ScopeKind::Library ? Json(false) : Json(s.collectionKey);
            lib.saveCollection(s.libraryId, *c);
        }
    }
    e->acceptProposedAction();
}

} // namespace on::gui
