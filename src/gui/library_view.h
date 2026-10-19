#pragma once

#include <QTimer>
#include <QWidget>

#include "gui/items_model.h"

class QComboBox;
class QHBoxLayout;
class QLineEdit;
class QSplitter;
class QTreeView;

namespace fp::gui {

class CollectionTree;
class ItemPane;
class TagSelector;

// The "My Library" tab: collections | items | item pane.
class LibraryView : public QWidget {
    Q_OBJECT
public:
    explicit LibraryView(QWidget* parent = nullptr);

    Scope scope() const;
    int64_t libraryId() const { return scope().libraryId; }
    QStringList selectedKeys() const;
    QStringList selectedTopLevelKeys() const;
    void selectItem(const QString& key, int64_t libraryId = 0);
    void focusSearch();
    // Adds a button to the row of the search field, left of it.
    void addTool(QWidget* tool);
    QTreeView* itemsView() const { return items_; }
    CollectionTree* collectionTree() const { return tree_; }
    QByteArray saveState() const;  // layout: splitters and columns
    void restoreState(const QByteArray& state);
    // What is shown: collection, selected items, expanded rows and scroll position.
    Json viewState() const;
    void restoreViewState(const Json& state);

    // actions
    void newItem(const QString& itemType);
    void newNote(bool child);
    void addAttachments(bool link);
    void addLinkAttachment();
    void newCollection();
    void trashSelected();
    void restoreSelected();
    void eraseSelected();
    void removeFromCollection();
    void duplicateSelected();
    void mergeSelected();
    void importFiles(const QStringList& files, const QString& parentKey);

signals:
    void openAttachmentRequested(qint64 libraryId, const QString& key);
    void exportRequested(const Scope& scope, const QStringList& keys);
    void bibliographyRequested(const Scope& scope, const QStringList& keys);
    void retrieveMetadataRequested(qint64 libraryId, const QStringList& keys);
    void newLibraryRequested();
    void selectionChanged();

private:
    void onScopeChanged(const Scope& scope);
    void onSelectionChanged();
    void reloadItems();
    void openIndex(const QModelIndex& index);
    void showItemMenu(const QPoint& pos);
    void selectKeys(const QStringList& keys, const QString& current);

    CollectionTree* tree_;
    TagSelector* tags_;
    QLineEdit* search_;
    QComboBox* searchMode_;
    QHBoxLayout* searchRow_;
    int tools_ = 0;
    QTreeView* items_;
    ItemsModel* model_;
    ItemPane* pane_;
    QSplitter* splitter_;
    QSplitter* leftSplitter_;
    QTimer reloadTimer_;
    QTimer searchTimer_;
    QString pendingSelect_;
};

} // namespace fp::gui
