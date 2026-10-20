#include "gui/library_view.h"

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QPainter>
#include <QDir>
#include <QDateTime>
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
#include <QToolButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/collection_tree.h"
#include "gui/common.h"
#include "gui/icons.h"
#include "gui/item_pane.h"
#include "gui/note_editor.h"
#include "gui/search_field.h"
#include "gui/side_pane.h"
#include "gui/tag_selector.h"
#include "gui/theme.h"

namespace on::gui {

namespace {

// Item list with Zotero-like keyboard handling, and optionally a picture behind its rows.
class ItemsTreeView : public QTreeView {
public:
    using QTreeView::QTreeView;
    std::function<void(bool permanently)> onDelete;  // Delete; with Shift, not to the trash
    std::function<void()> onOpen;

    // Drawn faintly behind the rows, whole and centred; a null image shows none. Over a
    // picture the rows do not alternate, in light and dark mode and on every platform: the
    // stripes (macOS's style also paints them below the last item) cut it into bands.
    void setBackgroundImage(const QImage& image)
    {
        background_ = image;
        scaled_ = QPixmap();
        setAlternatingRowColors(image.isNull());
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        if (!background_.isNull()) {
            QPainter p(viewport());
            QSize area = viewport()->size();
            qreal dpr = viewport()->devicePixelRatioF();
            if (scaled_.isNull() || scaledFor_ != area) {  // scaled once per size
                QSize s = background_.size().scaled(area, Qt::KeepAspectRatio);
                scaled_ = QPixmap::fromImage(background_.scaled(s * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                scaled_.setDevicePixelRatio(dpr);
                scaledFor_ = area;
            }
            QSizeF s = scaled_.deviceIndependentSize();
            // faint enough to read the columns over it
            p.setOpacity(isDarkTheme() ? 0.18 : 0.14);
            p.drawPixmap(QPointF((area.width() - s.width()) / 2, (area.height() - s.height()) / 2), scaled_);
        }
        QTreeView::paintEvent(e);
    }
    // At a fractional scale (125%, 150%) a row's edge falls inside a pixel it shares with
    // the row above; redrawing only the rows whose selection changed left a line of the
    // old highlight behind, and cut one off the new. The rows in view are redrawn.
    void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override
    {
        QTreeView::selectionChanged(selected, deselected);
        viewport()->update();
    }
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override
    {
        QTreeView::currentChanged(current, previous);
        viewport()->update();
    }
    // the picture stays put: scrolling moves the rows over it, so everything is redrawn
    void scrollContentsBy(int dx, int dy) override
    {
        QTreeView::scrollContentsBy(dx, dy);
        if (!background_.isNull())
            viewport()->update();
    }
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem o(option);
        // the item a drag would go into stands out, as if selected
        if (dropTarget_.isValid() && index.siblingAtColumn(0) == dropTarget_)
            o.state |= QStyle::State_Selected;
        QTreeView::drawRow(painter, o, index);
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        QTreeView::dragMoveEvent(e);
        QModelIndex target;
        if (e->isAccepted() && dropIndicatorPosition() == OnItem)
            target = indexAt(e->position().toPoint()).siblingAtColumn(0);
        setDropTarget(target);
    }
    void dragLeaveEvent(QDragLeaveEvent* e) override
    {
        QTreeView::dragLeaveEvent(e);
        setDropTarget({});
    }
    void dropEvent(QDropEvent* e) override
    {
        setDropTarget({});
        QTreeView::dropEvent(e);
    }
    void setDropTarget(const QModelIndex& index)
    {
        if (index == dropTarget_)
            return;
        dropTarget_ = index;
        viewport()->update();
    }

public:
    QModelIndex dropTarget() const { return dropTarget_; }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && onDelete) {
            onDelete(e->modifiers() & Qt::ShiftModifier);
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

private:
    QImage background_;
    QPixmap scaled_;  // background_ at the size it was last drawn
    QSize scaledFor_;
    QPersistentModelIndex dropTarget_;  // the item a drag hovers to drop onto
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
    auto* middle = middle_ = new QWidget(splitter_);
    auto* mv = new QVBoxLayout(middle);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(4);
    auto* searchRow = searchRow_ = new QHBoxLayout;
    searchRow->setContentsMargins(4, 4, 4, 0);
    searchRow->setSpacing(2);
    searchMode_ = new QComboBox(middle);
    searchMode_->addItem(tr("Title, Creator, Year"), (int)SearchMode::TitleCreatorYear);
    searchMode_->addItem(tr("All Fields & Tags"), (int)SearchMode::AllFields);
    searchMode_->addItem(tr("Everything"), (int)SearchMode::Everything);
    searchMode_->setCurrentIndex(
        (int)AppContext::get().config().get("/ui/searchMode", (int)SearchMode::AllFields).get<int>());
    search_ = new SearchField(middle);
    search_->setObjectName("librarySearch");
    search_->setPlaceholderText(tr("Search"));
    search_->setClearButtonEnabled(true);
    search_->addAction(icon("search"), QLineEdit::LeadingPosition);
    searchRow->addWidget(search_, 1);
    searchRow->addWidget(searchMode_);
    // shows and hides the item pane, like the notes pane of a reader tab
    paneToggle_ = new QToolButton(middle);
    paneToggle_->setObjectName("itemPaneToggle");
    paneToggle_->setIcon(icon("context-pane"));
    paneToggle_->setIconSize(QSize(18, 18));
    paneToggle_->setAutoRaise(true);
    paneToggle_->setCheckable(true);
    paneToggle_->setToolTip(tr("Show or hide the item pane"));
    searchRow->addWidget(paneToggle_);
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
    // the columns always fill the list exactly (see fitColumns), so no sideways scrolling
    items_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    items_->viewport()->installEventFilter(this);
    auto* header = items_->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionsMovable(true);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->resizeSection(ItemsModel::Title, 420);
    header->resizeSection(ItemsModel::Creator, 160);
    header->resizeSection(ItemsModel::Year, 55);
    // a date and time whole ("2026-04-27 18:03"), not "2026-04-27 1..."
    int dateWidth = items_->fontMetrics().horizontalAdvance(QStringLiteral("0000-00-00 00:00")) + 24;
    header->resizeSection(ItemsModel::DateAdded, dateWidth);
    header->resizeSection(ItemsModel::DateModified, dateWidth);
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
    shareSidePaneWidth(splitter_);  // the item pane as wide as the PDFs' Info and Notes
    // dragging an edge far does not collapse the list (or the item pane, which has its
    // own button) to nothing
    splitter_->setCollapsible(1, false);
    splitter_->setCollapsible(2, false);
    // dragging the item pane's edge squeezes the item list, never the collections
    splitter_->handle(2)->installEventFilter(this);
    connect(paneToggle_, &QToolButton::toggled, this, [this](bool on) {
        setItemPaneVisible(on);
        AppContext::get().config().set("/ui/itemPaneVisible", on);
    });
    paneToggle_->setChecked(AppContext::get().config().get("/ui/itemPaneVisible", true).get<bool>());
    setItemPaneVisible(paneToggle_->isChecked());

    // Sorting rearranges the rows, and the header then puts its sections back one at a
    // time, the hidden ones shown at their old width for a moment: taken for drags, each
    // moved the title's edge, and the columns ended short of the list's right edge. The
    // widths stay as they were.
    connect(model_, &QAbstractItemModel::layoutAboutToBeChanged, this, [this, header] {
        relayoutWidths_.clear();
        for (int c = 0; c < header->count(); ++c)
            relayoutWidths_.push_back(header->isSectionHidden(c) ? -1 : header->sectionSize(c));
        relayout_ = true;
    });
    connect(model_, &QAbstractItemModel::layoutChanged, this, [this, header] {  // after the header's
        fitting_ = true;
        for (int c = 0; c < header->count() && c < (int)relayoutWidths_.size(); ++c)
            if (relayoutWidths_[c] > 0 && header->sectionSize(c) != relayoutWidths_[c])
                header->resizeSection(c, relayoutWidths_[c]);
        fitting_ = false;
        relayout_ = false;
        fitColumns();
    });
    connect(header, &QHeaderView::sectionResized, this, [this, header](int section, int oldSize, int newSize) {
        if (fitting_ || relayout_)
            return;
        if (newSize > 0 && oldSize > 0) {
            // The dragged edge follows the mouse: the columns after it give the width (the
            // next one first, down to its minimum) or take it, so the list stays filled.
            // Left to the title, which is before it, the edge stood still and the columns
            // before it moved the other way.
            int left = newSize - oldSize;
            bool after = false;
            fitting_ = true;
            for (int v = header->visualIndex(section) + 1; v < header->count() && left != 0; ++v) {
                int next = header->logicalIndex(v);
                if (header->isSectionHidden(next))
                    continue;
                after = true;
                int size = header->sectionSize(next);
                int fitted = std::max(header->minimumSectionSize(), size - left);
                left -= size - fitted;
                header->resizeSection(next, fitted);
            }
            if (after && left > 0)  // all after it at their minimum: no wider
                header->resizeSection(section, newSize - left);
            fitting_ = false;
        }
        fitColumns();
    });

    view->onDelete = [this](bool permanently) {
        auto s = scope();
        if (s.kind == ScopeKind::Trash || permanently)
            eraseSelected();
        else if (s.kind == ScopeKind::Collection)
            deleteInCollection();
        else
            trashSelected();
    };
    view->onOpen = [this] { openSelected(); };

    connect(tree_, &CollectionTree::scopeChanged, this, &LibraryView::onScopeChanged);
    connect(tree_, &CollectionTree::exportRequested, this, [this](const Scope& s) { emit exportRequested(s, {}); });
    connect(tree_, &CollectionTree::bibliographyRequested, this,
            [this](const Scope& s) { emit bibliographyRequested(s, {}); });
    connect(tree_, &CollectionTree::newLibraryRequested, this, &LibraryView::newLibraryRequested);
    connect(tree_, &CollectionTree::deleteLibraryRequested, this, &LibraryView::deleteLibraryRequested);
    connect(tree_, &CollectionTree::filesDropped, this, &LibraryView::importFilesInto);
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
    // Esc in the search: no search, nothing selected, the keys back on the item list
    connect(search_, &SearchField::escapePressed, this, [this, applySearch] {
        search_->clear();
        searchTimer_.stop();
        applySearch();
        items_->selectionModel()->clearSelection();
        items_->setFocus();
    });
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
            connect(a, &QAction::toggled, this, [this, header, c](bool on) {
                header->setSectionHidden(c, !on);
                fitColumns();
            });
        }
        menu.exec(header->mapToGlobal(pos));
    });
    connect(model_, &ItemsModel::filesDropped, this, &LibraryView::importFiles);
    connect(model_, &ItemsModel::itemsMoved, this, [this](const QStringList& keys, const QString& parentKey) {
        auto s = scope();
        std::vector<std::string> moved;
        for (auto& k : keys)
            moved.push_back(ss(k));
        // dragged off its item, a child stays in the collection shown
        AppContext::get().library().moveToParent(s.libraryId, moved, ss(parentKey),
                                                 s.kind == ScopeKind::Collection ? s.collectionKey : std::string());
        if (!parentKey.isEmpty())
            items_->expand(model_->indexOfKey(parentKey));
    });
    connect(pane_, &ItemPane::openAttachment, this,
            [this](const QString& key) { emit openAttachmentRequested(libraryId(), key); });
    connect(pane_, &ItemPane::selectItem, this, [this](const QString& key) { selectItem(key); });
    // a part of the item's location: show that collection, with the item selected in it
    connect(pane_, &ItemPane::showScope, this, [this](const Scope& s, const QString& key) {
        search_->clear();
        tree_->selectScope(s);
        // in a parent collection the item may not be listed (it is in a subcollection)
        if (model_->indexOfKey(key).isValid())
            selectItem(key, s.libraryId);
    });

    reloadTimer_.setSingleShot(true);
    reloadTimer_.setInterval(60);
    connect(&reloadTimer_, &QTimer::timeout, this, &LibraryView::reloadItems);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList&) {
                if ((lib == libraryId() || lib == 0) && (type == "item" || type == "*" || type == "collection"))
                    reloadTimer_.start();
            });

    tree_->reload();

    bandTimer_.setSingleShot(true);
    connect(&bandTimer_, &QTimer::timeout, this, &LibraryView::updateBandBackground);
    connect(&AppContext::get(), &AppContext::appearanceChanged, this, &LibraryView::updateBandBackground);
    updateBandBackground();
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
        selectKeys(selected, current);
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
        // an item is in the trash when it, or the item it belongs to, was trashed
        auto self = lib.item(libId, ss(key));
        auto top = lib.item(libId, lib.topLevelKey(libId, ss(key)));
        bool trashed = (self && jbool(*self, "deleted")) || (top && jbool(*top, "deleted"));
        Scope s{libId, trashed ? ScopeKind::Trash : ScopeKind::Library, {}};
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

void LibraryView::selectKeys(const QStringList& keys, const QString& current)
{
    QItemSelection sel;
    for (auto& k : keys) {
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

namespace {

const char* const kScopeNames[] = {"library", "collection", "unfiled", "duplicates", "trash"};

} // namespace

Json LibraryView::viewState() const
{
    auto s = scope();
    Json state{{"libraryID", s.libraryId},
               {"scope", kScopeNames[int(s.kind)]},
               {"collection", s.collectionKey},
               {"selected", toStdVector(selectedKeys())},
               {"current", ss(model_->keyAt(items_->currentIndex()))},
               {"collections", toStdVector(tree_->expandedState())}};
    Json expanded = Json::array();
    for (int i = 0; i < model_->rowCount(); ++i) {
        auto idx = model_->index(i, 0);
        if (items_->isExpanded(idx))
            expanded.push_back(ss(model_->keyAt(idx)));
    }
    state["expanded"] = expanded;
    // the row at the top of the list, which survives changes of window size and sorting
    state["top"] = ss(model_->keyAt(items_->indexAt(QPoint(1, 1))));
    return state;
}

void LibraryView::restoreViewState(const Json& state)
{
    if (!state.is_object() || !state.contains("libraryID"))
        return;
    Scope s{jint(state, "libraryID"), ScopeKind::Library, jstr(state, "collection")};
    for (int k = 0; k < (int)std::size(kScopeNames); ++k)
        if (jstr(state, "scope") == kScopeNames[k])
            s.kind = ScopeKind(k);
    tree_->restoreExpanded(toQStringList(state.value("collections", std::vector<std::string>())));
    tree_->selectScope(s);
    for (auto& k : state.value("expanded", Json::array()))
        if (k.is_string())
            items_->setExpanded(model_->indexOfKey(qs(k.get<std::string>())), true);
    selectKeys(toQStringList(state.value("selected", std::vector<std::string>())), jqs(state, "current"));
    // scroll once the list has its size
    QTimer::singleShot(0, this, [this, top = jqs(state, "top"), current = jqs(state, "current")] {
        auto idx = model_->indexOfKey(top.isEmpty() ? current : top);
        if (idx.isValid())
            items_->scrollTo(idx, top.isEmpty() ? QAbstractItemView::EnsureVisible : QAbstractItemView::PositionAtTop);
    });
}

void LibraryView::addTool(QWidget* tool)
{
    tool->setParent(this);
    searchRow_->insertWidget(tools_++, tool);
    if (tools_ == 1)
        searchRow_->insertSpacing(tools_, 0);
}

// One of the twelve BanG Dream! bands behind the item list, a different one every two
// hours (Poppin'Party from midnight to Ikka Dumb Rock! from 22:00): the pictures built
// into the program (resources/bands), or those in <data dir>/backgrounds/bands, in name
// order.
void LibraryView::updateBandBackground()
{
    auto* view = static_cast<ItemsTreeView*>(items_);
    bandTimer_.stop();
    QStringList files;
    const QStringList kinds{"*.png", "*.jpg", "*.jpeg", "*.webp"};
    QDir dir(qpath(AppContext::get().library().dataDir() / "backgrounds" / "bands"));
    if (AppContext::get().config().get("/ui/bandBackground", true).get<bool>()) {
        files = dir.entryList(kinds, QDir::Files, QDir::Name);
        if (files.isEmpty()) {
            dir = QDir(":/bands");
            files = dir.entryList(kinds, QDir::Files, QDir::Name);
        }
    }
    if (files.isEmpty()) {
        view->setBackgroundImage({});
        return;
    }
    auto now = QDateTime::currentDateTime();
    int slot = now.time().hour() / 2;
    view->setBackgroundImage(QImage(dir.filePath(files[slot % files.size()])));
    // the next band at the next even hour
    QDateTime next(now.date().addDays(slot == 11 ? 1 : 0), QTime((slot + 1) % 12 * 2, 0));
    bandTimer_.start(int(std::clamp<qint64>(now.msecsTo(next) + 500, 1000, 2 * 3600 * 1000)));
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
    if (!c.isEmpty()) {
        fitting_ = true;  // the saved widths as they are, not traded between columns
        items_->header()->restoreState(c);
        fitting_ = false;
    }
    items_->header()->setStretchLastSection(false);
    setItemPaneVisible(paneToggle_->isChecked());  // not part of the splitter's state
    applySidePaneWidth(splitter_);
    fitColumns();
}

void LibraryView::setItemPaneVisible(bool visible)
{
    if (paneToggle_->isChecked() != visible) {
        paneToggle_->setChecked(visible);  // comes back here through toggled()
        return;
    }
    pane_->setVisible(visible);
}

bool LibraryView::isItemPaneVisible() const { return pane_->isVisible(); }

void LibraryView::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) {
        items_->setFocus();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool LibraryView::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == items_->viewport() && e->type() == QEvent::Resize)
        fitColumns();
    if (watched == splitter_->handle(2)) {
        // While the item pane's edge is dragged, the collections pane keeps its width and
        // the list keeps room for the title's minimum (or what it has, if less). Only
        // then: as a lasting minimum it would set the window's, too wide for small screens.
        if (e->type() == QEvent::MouseButtonPress) {
            leftMinimum_ = leftSplitter_->minimumWidth();
            leftSplitter_->setMinimumWidth(leftSplitter_->width());
            middle_->setMinimumWidth(std::min(middle_->width(), listMinimum_));
        } else if (e->type() == QEvent::MouseButtonRelease) {
            leftSplitter_->setMinimumWidth(leftMinimum_);
            middle_->setMinimumWidth(0);
        }
    }
    return QWidget::eventFilter(watched, e);
}

// The title column takes whatever width the other columns leave, so the last column
// always ends where the list does. Dragging the item pane's edge stops when the title is
// at its minimum (see eventFilter); only a narrow window squeezes it further.
void LibraryView::fitColumns()
{
    if (fitting_)
        return;
    auto* header = items_->header();
    constexpr int kMinTitle = 180, kNarrowTitle = 60;
    int others = 0;
    for (int c = 0; c < header->count(); ++c)
        if (c != ItemsModel::Title && !header->isSectionHidden(c))
            others += header->sectionSize(c);
    fitting_ = true;
    header->resizeSection(ItemsModel::Title, std::max(kNarrowTitle, items_->viewport()->width() - others));
    fitting_ = false;
    items_->horizontalScrollBar()->setValue(0);
    int chrome = items_->frameWidth() * 2 + items_->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    listMinimum_ = std::max(kMinTitle + others + chrome, middle_->layout()->minimumSize().width());
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

// Enter on several items opens their files as tabs in list order (top to bottom) and
// shows the first; notes and items without a file are skipped.
void LibraryView::openSelected()
{
    auto rows = items_->selectionModel()->selectedRows(0);
    if (rows.size() <= 1) {
        openIndex(items_->currentIndex());
        return;
    }
    auto order = [](const QModelIndex& i) {
        return i.parent().isValid() ? std::pair(i.parent().row(), i.row() + 1) : std::pair(i.row(), 0);
    };
    std::sort(rows.begin(), rows.end(), [&](const QModelIndex& a, const QModelIndex& b) { return order(a) < order(b); });
    auto& lib = AppContext::get().library();
    QStringList files;
    for (auto& i : rows) {
        auto key = model_->keyAt(i);
        auto type = model_->typeAt(i);
        QString file;
        if (type == "attachment")
            file = key;
        else if (type != "note")
            if (auto best = lib.bestAttachment(libraryId(), ss(key)))
                file = jqs(*best, "key");
        if (!file.isEmpty() && !files.contains(file))
            files << file;
    }
    if (files.size() > 10 &&
        QMessageBox::question(this, tr("Open Items"), tr("Open %n items?", nullptr, files.size())) != QMessageBox::Yes)
        return;
    for (auto& f : files)
        emit openAttachmentRequested(libraryId(), f);
    if (files.size() > 1)
        emit openAttachmentRequested(libraryId(), files.first());  // already open: shows it
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

// Delete in a collection: items leave it, but a selected attachment or note goes to the
// trash (it is not in the collection itself; removing its item instead lost the item).
void LibraryView::deleteInCollection()
{
    auto s = scope();
    QStringList items, children;
    for (auto& i : items_->selectionModel()->selectedRows(0))
        (i.parent().isValid() ? children : items) << model_->keyAt(i);
    if (children.isEmpty()) {
        removeFromCollection();
        return;
    }
    if (!AppContext::get().editable(s.libraryId))
        return;
    auto question = items.isEmpty()
                        ? tr("Move %n attachment(s) or note(s) to the trash?", nullptr, children.size())
                        : tr("Remove %1 from this collection and move %2 to the trash?")
                              .arg(tr("%n item(s)", nullptr, items.size()),
                                   tr("%n attachment(s) or note(s)", nullptr, children.size()));
    if (QMessageBox::question(this, tr("Delete"), question) != QMessageBox::Yes)
        return;
    auto& lib = AppContext::get().library();
    Library::BatchGuard batch(lib);
    if (!items.isEmpty())
        lib.removeFromCollection(s.libraryId, s.collectionKey, toStdVector(items));
    lib.trashItems(s.libraryId, toStdVector(children));
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
    importFilesTo(scope(), files, parentKey);
}

void LibraryView::importFilesInto(const Scope& target, const QStringList& files)
{
    importFilesTo(target, files, {});
}

void LibraryView::importFilesTo(const Scope& s, const QStringList& files, const QString& parentKey)
{
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
    menu.addAction(icon("trash"), tr("Move Item to Trash…") + "\t" + QKeySequence(Qt::Key_Delete).toString(QKeySequence::NativeText),
                   this, &LibraryView::trashSelected)
        ->setEnabled(editable);
    menu.addAction(tr("Delete Permanently…") + "\t" +
                       QKeySequence(Qt::SHIFT | Qt::Key_Delete).toString(QKeySequence::NativeText),
                   this, &LibraryView::eraseSelected)
        ->setEnabled(editable);
    menu.exec(items_->viewport()->mapToGlobal(pos));
}

} // namespace on::gui
