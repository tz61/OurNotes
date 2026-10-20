#pragma once

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

    void newCollection(int64_t lib, const std::string& parentKey);
    void renameItem(QTreeWidgetItem* item);
    void deleteCollection(QTreeWidgetItem* item, bool trashItems);

    QPersistentModelIndex dropTarget_;
};

} // namespace on::gui
