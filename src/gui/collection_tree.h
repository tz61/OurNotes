#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QTreeWidget>

#include "gui/items_model.h"

namespace on::gui {

// Left pane: libraries with their collections, Unfiled, Duplicates and Trash.
class CollectionTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit CollectionTree(QWidget* parent = nullptr);

    void reload();
    Scope currentScope() const;
    void selectScope(const Scope& scope);
    // Expanded libraries and collections, as "<libraryId>:<collectionKey>" (empty key for
    // the library itself).
    QStringList expandedState() const;
    void restoreExpanded(const QStringList& state);

signals:
    void scopeChanged(const Scope& scope);
    void exportRequested(const Scope& scope);
    void bibliographyRequested(const Scope& scope);
    void newLibraryRequested();
    void deleteLibraryRequested(qint64 libraryId);
    void filesDropped(const Scope& scope, const QStringList& files);  // from outside the app
    void statusMessage(const QString& text);  // empty: done

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void startDrag(Qt::DropActions actions) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    // the rows in view redrawn (see ItemsTreeView, in library_view.cpp)
    void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;

private:
    enum Roles { KindRole = Qt::UserRole + 1, LibraryRole, CollectionRole };
    QTreeWidgetItem* addScopeItem(QTreeWidgetItem* parent, const QString& text, const QIcon& icon, const Scope& s);
    Scope scopeOf(const QTreeWidgetItem* item) const;
    QTreeWidgetItem* findItem(const Scope& s) const;
    bool acceptsDrop(QDropEvent* e, QTreeWidgetItem** target) const;
    static QStringList droppedFiles(const QMimeData* mime);  // files dragged in from outside
    void setDropTarget(QTreeWidgetItem* item);  // shown as selected while a drag hovers on it
    // A drag within four rows of the top or the bottom scrolls the tree, the faster the
    // nearer the edge.
    int rowHeight() const;
    double dragScrollSpeed(int y) const;  // pixels a second, down (+) or up (-); 0 elsewhere
    void dragScroll();
    void stopDragScroll();

    // Items dragged onto another library or one of its collections: copied there, with
    // their files (those not on this computer downloaded first).
    void copyItems(qint64 from, const std::vector<std::string>& keys, const Scope& to);
    void newCollection(int64_t lib, const std::string& parentKey);
    void renameItem(QTreeWidgetItem* item);
    void deleteCollection(QTreeWidgetItem* item, bool trashItems);

    QPersistentModelIndex dropTarget_;
    QTimer dragScrollTimer_;
    QElapsedTimer dragScrollClock_;
    double dragScrollCarry_ = 0;  // a fraction of a pixel, scrolled with the next
    QPoint dragPos_;
    const QMimeData* dragMime_ = nullptr;  // the drag's, while it is over the tree
    Qt::DropActions dragActions_;
};

} // namespace on::gui
