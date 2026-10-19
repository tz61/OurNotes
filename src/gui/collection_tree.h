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

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void startDrag(Qt::DropActions actions) override;

private:
    enum Roles { KindRole = Qt::UserRole + 1, LibraryRole, CollectionRole };
    QTreeWidgetItem* addScopeItem(QTreeWidgetItem* parent, const QString& text, const QIcon& icon, const Scope& s);
    Scope scopeOf(const QTreeWidgetItem* item) const;
    QTreeWidgetItem* findItem(const Scope& s) const;
    bool acceptsDrop(QDropEvent* e, QTreeWidgetItem** target) const;

    void newCollection(int64_t lib, const std::string& parentKey);
    void renameItem(QTreeWidgetItem* item);
    void deleteCollection(QTreeWidgetItem* item, bool trashItems);
};

} // namespace on::gui
