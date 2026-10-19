#include "gui/library_view.h"

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QSplitter>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/collection_tree.h"
#include "gui/common.h"
#include "gui/icons.h"
#include "gui/item_pane.h"
#include "gui/note_editor.h"
#include "gui/tag_selector.h"

namespace fp::gui {

namespace {

// Item list with Zotero-like keyboard handling.
class ItemsTreeView : public QTreeView {
public:
    using QTreeView::QTreeView;
    std::function<void()> onDelete;
    std::function<void()> onOpen;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && onDelete) {
            onDelete();
            return;
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && onOpen) {
            onOpen();
            return;
        }
        if (e->key() == Qt::Key_Plus || (e->key() == Qt::Key_Equal && e->modifiers() == Qt::NoModifier)) {
            expandAll();
            return;
        }
        if (e->key() == Qt::Key_Minus) {
            collapseAll();
            return;
        }
        QTreeView::keyPressEvent(e);
    }
};

} // namespace

LibraryView::LibraryView(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter_);

    // left: collections + tags
    leftSplitter_ = new QSplitter(Qt::Vertical, splitter_);
    tree_ = new CollectionTree(leftSplitter_);
    tags_ = new TagSelector(leftSplitter_);
    leftSplitter_->setStretchFactor(0, 3);
    leftSplitter_->setStretchFactor(1, 1);

    // middle: search + items
    auto* middle = new QWidget(splitter_);
    auto* mv = new QVBoxLayout(middle);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(4);
    auto* searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(4, 4, 4, 0);
    searchMode_ = new QComboBox(middle);
    searchMode_->addItem(tr("Title, Creator, Year"), (int)SearchMode::TitleCreatorYear);
    searchMode_->addItem(tr("All Fields & Tags"), (int)SearchMode::AllFields);
    searchMode_->addItem(tr("Everything"), (int)SearchMode::Everything);
    searchMode_->setCurrentIndex(
        (int)AppContext::get().config().get("/ui/searchMode", (int)SearchMode::AllFields).get<int>());
    search_ = new QLineEdit(middle);
    search_->setPlaceholderText(tr("Search"));
    search_->setClearButtonEnabled(true);
    search_->addAction(icon("search"), QLineEdit::LeadingPosition);
    searchRow->addWidget(search_, 1);
    searchRow->addWidget(searchMode_);
    mv->addLayout(searchRow);

    model_ = new ItemsModel(this);
    auto* view = new ItemsTreeView(middle);
    items_ = view;
    items_->setModel(model_);
    items_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    items_->setSortingEnabled(true);
    items_->sortByColumn(ItemsModel::Title, Qt::AscendingOrder);
    items_->setUniformRowHeights(true);
    items_->setDragEnabled(true);
    items_->setAcceptDrops(true);
    items_->setDropIndicatorShown(true);
    items_->setDragDropMode(QAbstractItemView::DragDrop);
    items_->setDefaultDropAction(Qt::CopyAction);
    items_->setContextMenuPolicy(Qt::CustomContextMenu);
    items_->setAllColumnsShowFocus(true);
    items_->setAlternatingRowColors(true);
    items_->setIconSize(QSize(16, 16));
    items_->setExpandsOnDoubleClick(false);
    auto* header = items_->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionsMovable(true);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->resizeSection(ItemsModel::Title, 420);
    header->resizeSection(ItemsModel::Creator, 160);
    header->resizeSection(ItemsModel::Year, 55);
    header->resizeSection(ItemsModel::Attachments, 34);
    header->resizeSection(ItemsModel::Notes, 34);
    for (int c : {ItemsModel::ItemType, ItemsModel::Date, ItemsModel::Publication, ItemsModel::DateModified,
                  ItemsModel::Notes})
        header->hideSection(c);
    mv->addWidget(items_, 1);

    // right: item pane
    pane_ = new ItemPane(splitter_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 0);
    splitter_->setSizes({240, 760, 380});

    view->onDelete = [this] {
        auto s = scope();
        if (s.kind == ScopeKind::Trash)
            eraseSelected();
        else if (s.kind == ScopeKind::Collection)
            removeFromCollection();
        else
            trashSelected();
    };
    view->onOpen = [this] { openIndex(items_->currentIndex()); };

    connect(tree_, &CollectionTree::scopeChanged, this, &LibraryView::onScopeChanged);
    connect(tree_, &CollectionTree::exportRequested, this, [this](const Scope& s) { emit exportRequested(s, {}); });
    connect(tree_, &CollectionTree::bibliographyRequested, this,
            [this](const Scope& s) { emit bibliographyRequested(s, {}); });
    connect(tree_, &CollectionTree::newLibraryRequested, this, &LibraryView::newLibraryRequested);
    connect(tags_, &TagSelector::selectionChanged, this, [this](const QStringList& tags) {
        model_->setTagFilter(tags);
        if (model_->isSearching())
            items_->expandAll();
    });
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(250);
    connect(search_, &QLineEdit::textChanged, &searchTimer_, qOverload<>(&QTimer::start));
    auto applySearch = [this] {
        model_->setSearch(search_->text(), (SearchMode)searchMode_->currentData().toInt());
        if (model_->isSearching())
            items_->expandAll();
    };
    connect(&searchTimer_, &QTimer::timeout, this, applySearch);
    connect(searchMode_, &QComboBox::currentIndexChanged, this, [this, applySearch] {
        AppContext::get().config().set("/ui/searchMode", searchMode_->currentData().toInt());
        applySearch();
    });
    connect(items_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LibraryView::onSelectionChanged);
    connect(items_, &QTreeView::doubleClicked, this, &LibraryView::openIndex);
    connect(items_, &QWidget::customContextMenuRequested, this, &LibraryView::showItemMenu);
    connect(header, &QWidget::customContextMenuRequested, this, [this, header](const QPoint& pos) {
        QMenu menu(this);
        for (int c = 0; c < ItemsModel::ColumnCount; ++c) {
            if (c == ItemsModel::Title)
                continue;
            auto* a = menu.addAction(ItemsModel::columnName(c));
            a->setCheckable(true);
            a->setChecked(!header->isSectionHidden(c));
            connect(a, &QAction::toggled, this, [header, c](bool on) { header->setSectionHidden(c, !on); });
        }
        menu.exec(header->mapToGlobal(pos));
    });
    connect(model_, &ItemsModel::filesDropped, this, &LibraryView::importFiles);
    connect(pane_, &ItemPane::openAttachment, this,
            [this](const QString& key) { emit openAttachmentRequested(libraryId(), key); });
    connect(pane_, &ItemPane::selectItem, this, [this](const QString& key) { selectItem(key); });

    reloadTimer_.setSingleShot(true);
    reloadTimer_.setInterval(60);
    connect(&reloadTimer_, &QTimer::timeout, this, &LibraryView::reloadItems);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList&) {
                if ((lib == libraryId() || lib == 0) && (type == "item" || type == "*" || type == "collection"))
                    reloadTimer_.start();
            });

    tree_->reload();
}

Scope LibraryView::scope() const { return model_->scope(); }

void LibraryView::onScopeChanged(const Scope& s)
{
    tags_->setLibrary(s.libraryId);
    model_->setScope(s);
    if (model_->isSearching())
        items_->expandAll();
    onSelectionChanged();
}

QStringList LibraryView::selectedKeys() const
{
    QStringList out;
    for (auto& i : items_->selectionModel()->selectedRows(0))
        out << model_->keyAt(i);
    return out;
}

QStringList LibraryView::selectedTopLevelKeys() const
{
    QStringList out;
    for (auto& i : items_->selectionModel()->selectedRows(0)) {
        auto top = i.parent().isValid() ? i.parent() : i;
        auto k = model_->keyAt(top);
        if (!out.contains(k))
            out << k;
    }
    return out;
}

void LibraryView::onSelectionChanged()
{
    pane_->showItems(libraryId(), selectedKeys());
    emit selectionChanged();
}

void LibraryView::reloadItems()
{
    auto selected = selectedKeys();
    auto current = model_->keyAt(items_->currentIndex());
    QStringList expanded;
    for (int i = 0; i < model_->rowCount(); ++i) {
        auto idx = model_->index(i, 0);
        if (items_->isExpanded(idx))
            expanded << model_->keyAt(idx);
    }
    int scroll = items_->verticalScrollBar()->value();
    {
        QSignalBlocker block(items_->selectionModel());
        model_->reload();
        for (auto& k : expanded)
            items_->setExpanded(model_->indexOfKey(k), true);
        if (model_->isSearching())
            items_->expandAll();
        if (!pendingSelect_.isEmpty()) {
            selected = {pendingSelect_};
            current = pendingSelect_;
            pendingSelect_.clear();
        }
        QItemSelection sel;
        for (auto& k : selected) {
            auto idx = model_->indexOfKey(k);
            if (idx.isValid()) {
                if (idx.parent().isValid())
                    items_->setExpanded(idx.parent(), true);
                sel.select(idx, idx.siblingAtColumn(ItemsModel::ColumnCount - 1));
            }
        }
        items_->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
        auto cur = model_->indexOfKey(current);
        if (cur.isValid())
            items_->selectionModel()->setCurrentIndex(cur, QItemSelectionModel::NoUpdate);
    }
    items_->verticalScrollBar()->setValue(scroll);
    if (!current.isEmpty() && model_->indexOfKey(current).isValid())
        items_->scrollTo(model_->indexOfKey(current));
    onSelectionChanged();
}

void LibraryView::selectItem(const QString& key, int64_t libraryId)
{
    auto& lib = AppContext::get().library();
    int64_t libId = libraryId ? libraryId : this->libraryId();
    auto idx = model_->indexOfKey(key);
    if (!idx.isValid()) {
        // not visible in the current view: switch to the library root
        search_->clear();
        model_->setSearch({}, SearchMode::AllFields);
        auto top = qs(lib.topLevelKey(libId, ss(key)));
        auto d = lib.item(libId, ss(top));
        Scope s{libId, d && jbool(*d, "deleted") ? ScopeKind::Trash : ScopeKind::Library, {}};
        tree_->selectScope(s);
        idx = model_->indexOfKey(key);
    }
    if (!idx.isValid())
        return;
    if (idx.parent().isValid())
        items_->setExpanded(idx.parent(), true);
    items_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    items_->scrollTo(idx);
}

void LibraryView::focusSearch()
{
    search_->setFocus();
    search_->selectAll();
}

QByteArray LibraryView::saveState() const
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds << splitter_->saveState() << leftSplitter_->saveState() << items_->header()->saveState();
    return out;
}

void LibraryView::restoreState(const QByteArray& state)
{
    QDataStream ds(state);
    QByteArray a, b, c;
    ds >> a >> b >> c;
    if (!a.isEmpty())
        splitter_->restoreState(a);
    if (!b.isEmpty())
        leftSplitter_->restoreState(b);
    if (!c.isEmpty())
        items_->header()->restoreState(c);
}

void LibraryView::openIndex(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    auto key = model_->keyAt(index);
    auto type = model_->typeAt(index);
    if (type == "note") {
        pane_->noteEditor()->focusEditor();
        return;
    }
    auto& lib = AppContext::get().library();
    if (type == "attachment") {
        emit openAttachmentRequested(libraryId(), key);
        return;
    }
    if (auto best = lib.bestAttachment(libraryId(), ss(key))) {
        emit openAttachmentRequested(libraryId(), jqs(*best, "key"));
        return;
    }
    if (auto d = lib.item(libraryId(), ss(key))) {
        auto url = jqs(*d, "url");
        if (url.isEmpty() && !jstr(*d, "DOI").empty())
            url = "https://doi.org/" + jqs(*d, "DOI");
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
    }
}

// --- actions -----------------------------------------------------------------------------------

void LibraryView::newItem(const QString& itemType)
{
    auto s = scope();
    if (!s.libraryId || !AppContext::get().editable(s.libraryId))
        return;
    Json data{{"itemType", ss(itemType)}, {"title", ""}, {"creators", Json::array()}};
    if (s.kind == ScopeKind::Collection)
        data["collections"] = Json::array({s.collectionKey});
    auto item = AppContext::get().library().createItem(s.libraryId, data);
    pendingSelect_ = jqs(item, "key");
    reloadItems();
}

void LibraryView::newNote(bool child)
{
    auto s = scope();
    if (!s.libraryId || !AppContext::get().editable(s.libraryId))
        return;
    auto& lib = AppContext::get().library();
    std::string parent;
    if (child) {
        auto sel = selectedTopLevelKeys();
        if (sel.size() != 1)
            return;
        auto d = lib.item(s.libraryId, ss(sel[0]));
        if (!d || !schema::isRegularType(jstr(*d, "itemType")))
            return;
        parent = ss(sel[0]);
    }
    auto note = lib.createNote(s.libraryId, "", parent);
    if (!child && s.kind == ScopeKind::Collection)
        lib.addToCollection(s.libraryId, s.collectionKey, {jstr(note, "key")});
    pendingSelect_ = jqs(note, "key");
    reloadItems();
    pane_->noteEditor()->focusEditor();
}

void LibraryView::addAttachments(bool link)
{
    auto files = QFileDialog::getOpenFileNames(this, link ? tr("Link to Files") : tr("Attach Files"));
    if (files.isEmpty())
        return;
    auto sel = selectedTopLevelKeys();
    QString parent;
    if (sel.size() == 1) {
        auto d = AppContext::get().library().item(libraryId(), ss(sel[0]));
        if (d && schema::isRegularType(jstr(*d, "itemType")))
            parent = sel[0];
    }
    auto& lib = AppContext::get().library();
    QStringList created;
    for (auto& f : files) {
        try {
            auto att = lib.importAttachment(libraryId(), fspath(f), ss(parent), link);
            created << jqs(att, "key");
            if (parent.isEmpty() && scope().kind == ScopeKind::Collection)
                lib.addToCollection(libraryId(), scope().collectionKey, {jstr(att, "key")});
        } catch (const std::exception& e) {
            QMessageBox::warning(this, tr("Attach File"), qs(e.what()));
        }
    }
    if (parent.isEmpty() && !created.isEmpty())
        emit retrieveMetadataRequested(libraryId(), created);
}

void LibraryView::addLinkAttachment()
{
    bool ok = false;
    auto url = QInputDialog::getText(this, tr("Attach Link to URI"), tr("URI:"), QLineEdit::Normal, "https://", &ok);
    if (!ok || url.trimmed().isEmpty())
        return;
    auto sel = selectedTopLevelKeys();
    std::string parent = sel.size() == 1 ? ss(sel[0]) : std::string();
    auto att = AppContext::get().library().createLinkedUrl(libraryId(), ss(url.trimmed()), {}, parent);
    pendingSelect_ = jqs(att, "key");
}

void LibraryView::newCollection()
{
    auto s = scope();
    if (!s.libraryId)
        return;
    bool ok = false;
    auto name = QInputDialog::getText(this, tr("New Collection"), tr("Name:"), QLineEdit::Normal,
                                      tr("New Collection"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    auto parent = s.kind == ScopeKind::Collection ? s.collectionKey : std::string();
    auto c = AppContext::get().library().createCollection(s.libraryId, ss(name.trimmed()), parent);
    tree_->reload();
    tree_->selectScope({s.libraryId, ScopeKind::Collection, jstr(c, "key")});
}

void LibraryView::trashSelected()
{
    auto keys = selectedKeys();
    if (keys.isEmpty() || !AppContext::get().editable(libraryId()))
        return;
    if (QMessageBox::question(this, tr("Move to Trash"), tr("Move %n item(s) to the trash?", nullptr, keys.size())) !=
        QMessageBox::Yes)
        return;
    AppContext::get().library().trashItems(libraryId(), toStdVector(keys));
}

void LibraryView::restoreSelected()
{
    AppContext::get().library().trashItems(libraryId(), toStdVector(selectedKeys()), false);
}

void LibraryView::eraseSelected()
{
    auto keys = selectedKeys();
    if (keys.isEmpty())
        return;
    if (QMessageBox::warning(this, tr("Delete Permanently"),
                             tr("Permanently delete %n item(s), including their attachments and notes? This cannot "
                                "be undone.",
                                nullptr, keys.size()),
                             QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    AppContext::get().library().eraseItems(libraryId(), toStdVector(keys));
}

void LibraryView::removeFromCollection()
{
    auto s = scope();
    if (s.kind != ScopeKind::Collection)
        return;
    auto keys = selectedTopLevelKeys();
    if (keys.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Remove from Collection"),
                              tr("Remove %n item(s) from this collection? They stay in the library.", nullptr,
                                 keys.size())) != QMessageBox::Yes)
        return;
    AppContext::get().library().removeFromCollection(s.libraryId, s.collectionKey, toStdVector(keys));
}

void LibraryView::duplicateSelected()
{
    auto keys = selectedTopLevelKeys();
    if (keys.size() != 1)
        return;
    auto& lib = AppContext::get().library();
    auto d = lib.item(libraryId(), ss(keys[0]));
    if (!d || !schema::isRegularType(jstr(*d, "itemType")))
        return;
    Json copy = *d;
    for (const char* k : {"key", "dateAdded", "dateModified", "relations"})
        copy.erase(k);
    auto item = lib.createItem(libraryId(), copy);
    lib.addRelated(libraryId(), jstr(item, "key"), ss(keys[0]));
    pendingSelect_ = jqs(item, "key");
}

void LibraryView::mergeSelected()
{
    auto keys = selectedTopLevelKeys();
    if (keys.size() < 2)
        return;
    auto& lib = AppContext::get().library();
    auto items = lib.items(libraryId(), toStdVector(keys));
    std::sort(items.begin(), items.end(),
              [](const Json& a, const Json& b) { return jstr(a, "dateAdded") < jstr(b, "dateAdded"); });
    auto master = jstr(items.front(), "key");
    if (QMessageBox::question(this, tr("Merge Items"),
                              tr("Merge %n items into \"%1\" (the oldest)? Notes, attachments, tags and collections "
                                 "are moved to it; the other items go to the trash.",
                                 nullptr, keys.size())
                                  .arg(jqs(items.front(), "title"))) != QMessageBox::Yes)
        return;
    std::vector<std::string> others;
    for (size_t i = 1; i < items.size(); ++i)
        others.push_back(jstr(items[i], "key"));
    lib.mergeItems(libraryId(), master, others);
}

void LibraryView::importFiles(const QStringList& files, const QString& parentKey)
{
    auto s = scope();
    if (!s.libraryId || !AppContext::get().editable(s.libraryId))
        return;
    auto& lib = AppContext::get().library();
    QStringList created;
    for (auto& f : files) {
        try {
            auto att = lib.importAttachment(s.libraryId, fspath(f), ss(parentKey));
            created << jqs(att, "key");
            if (parentKey.isEmpty() && s.kind == ScopeKind::Collection)
                lib.addToCollection(s.libraryId, s.collectionKey, {jstr(att, "key")});
        } catch (const std::exception& e) {
            QMessageBox::warning(this, tr("Import File"), qs(e.what()));
        }
    }
    if (parentKey.isEmpty() && !created.isEmpty())
        emit retrieveMetadataRequested(s.libraryId, created);
}

void LibraryView::showItemMenu(const QPoint& pos)
{
    auto index = items_->indexAt(pos);
    auto keys = selectedKeys();
    auto s = scope();
    auto& lib = AppContext::get().library();
    bool editable = AppContext::get().editable(s.libraryId);
    QMenu menu(this);
    if (!index.isValid() || keys.isEmpty()) {
        menu.addAction(icon("new-item"), tr("New Item"), this, [this] { newItem("journalArticle"); })
            ->setEnabled(editable);
        menu.addAction(icon("new-note"), tr("New Standalone Note"), this, [this] { newNote(false); })
            ->setEnabled(editable);
        menu.exec(items_->viewport()->mapToGlobal(pos));
        return;
    }
    auto type = model_->typeAt(index);
    bool single = keys.size() == 1;
    bool regular = schema::isRegularType(ss(type));

    if (single && (type == "attachment" || regular))
        menu.addAction(icon("open"), type == "attachment" ? tr("Open") : tr("Open PDF"), this,
                       [this, index] { openIndex(index); });
    if (single && type == "attachment") {
        menu.addAction(tr("Show File"), this, [&, key = keys[0]] {
            if (auto d = lib.item(s.libraryId, ss(key))) {
                auto p = lib.attachmentFile(s.libraryId, *d);
                if (!p.empty())
                    QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(p.parent_path())));
            }
        });
    }
    if (s.kind == ScopeKind::Trash) {
        menu.addSeparator();
        menu.addAction(tr("Restore to Library"), this, &LibraryView::restoreSelected)->setEnabled(editable);
        menu.addAction(tr("Delete Permanently…"), this, &LibraryView::eraseSelected)->setEnabled(editable);
        menu.exec(items_->viewport()->mapToGlobal(pos));
        return;
    }
    if (single && regular) {
        menu.addSeparator();
        menu.addAction(icon("new-note"), tr("Add Note"), this, [this] { newNote(true); })->setEnabled(editable);
        auto* attach = menu.addMenu(icon("attach"), tr("Add Attachment"));
        attach->setEnabled(editable);
        attach->addAction(tr("Attach Stored Copy of File…"), this, [this] { addAttachments(false); });
        attach->addAction(tr("Attach Link to File…"), this, [this] { addAttachments(true); });
        attach->addAction(tr("Attach Link to URI…"), this, &LibraryView::addLinkAttachment);
    }
    menu.addSeparator();
    auto* addTo = menu.addMenu(icon("folder"), tr("Add to Collection"));
    addTo->setEnabled(editable);
    {
        auto colls = lib.collections(s.libraryId);
        std::map<std::string, std::vector<Json>> byParent;
        for (auto& c : colls) {
            auto p = c.value("parentCollection", Json());
            byParent[p.is_string() ? p.get<std::string>() : ""].push_back(c);
        }
        std::function<void(QMenu*, const std::string&)> fill = [&](QMenu* m, const std::string& parent) {
            for (auto& c : byParent[parent]) {
                auto key = jstr(c, "key");
                if (byParent.count(key)) {
                    auto* sub = m->addMenu(icon("folder"), jqs(c, "name"));
                    sub->addAction(jqs(c, "name"), this, [&, key] {
                        lib.addToCollection(s.libraryId, key, toStdVector(selectedTopLevelKeys()));
                    });
                    sub->addSeparator();
                    fill(sub, key);
                } else {
                    m->addAction(icon("folder"), jqs(c, "name"), this, [&, key] {
                        lib.addToCollection(s.libraryId, key, toStdVector(selectedTopLevelKeys()));
                    });
                }
            }
        };
        fill(addTo, "");
        addTo->addSeparator();
        addTo->addAction(icon("new-collection"), tr("New Collection…"), this, [&] {
            bool ok = false;
            auto name = QInputDialog::getText(this, tr("New Collection"), tr("Name:"), QLineEdit::Normal, {}, &ok);
            if (ok && !name.trimmed().isEmpty()) {
                auto c = lib.createCollection(s.libraryId, ss(name.trimmed()));
                lib.addToCollection(s.libraryId, jstr(c, "key"), toStdVector(selectedTopLevelKeys()));
            }
        });
    }
    if (s.kind == ScopeKind::Collection)
        menu.addAction(tr("Remove Item from Collection…"), this, &LibraryView::removeFromCollection)
            ->setEnabled(editable);
    if (single && regular)
        menu.addAction(tr("Duplicate Item"), this, &LibraryView::duplicateSelected)->setEnabled(editable);
    if (s.kind == ScopeKind::Duplicates && keys.size() > 1)
        menu.addAction(tr("Merge %n Items…", nullptr, keys.size()), this, &LibraryView::mergeSelected)
            ->setEnabled(editable);
    menu.addSeparator();
    menu.addAction(icon("magic"), tr("Retrieve Metadata for PDF"), this, [this, keys] {
        emit retrieveMetadataRequested(libraryId(), keys);
    })->setEnabled(editable && type == "attachment");
    menu.addAction(tr("Create Bibliography from Items…"), this,
                   [this] { emit bibliographyRequested(scope(), selectedTopLevelKeys()); });
    menu.addAction(icon("export"), tr("Export Items…"), this,
                   [this] { emit exportRequested(scope(), selectedTopLevelKeys()); });
    menu.addSeparator();
    menu.addAction(icon("trash"), tr("Move Item to Trash…"), this, &LibraryView::trashSelected)->setEnabled(editable);
    menu.exec(items_->viewport()->mapToGlobal(pos));
}

} // namespace fp::gui
