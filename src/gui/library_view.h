#pragma once

#include <QTimer>
#include <QWidget>

#include <vector>

#include "gui/items_model.h"

class QComboBox;
class QHBoxLayout;
class QLineEdit;
class QSplitter;
class QToolButton;
class QTreeView;

namespace on::gui {

class CollectionTree;
class SearchField;
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
    // The item pane on the right (Info / Notes) can be hidden; the list then takes the room.
    void setItemPaneVisible(bool visible);
    bool isItemPaneVisible() const;

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
    void deleteInCollection();
    void duplicateSelected();
    void mergeSelected();
    void importFiles(const QStringList& files, const QString& parentKey);
    // Into a library or collection other than the one shown (dropped on it).
    void importFilesInto(const Scope& target, const QStringList& files);

signals:
    void openAttachmentRequested(qint64 libraryId, const QString& key);
    void exportRequested(const Scope& scope, const QStringList& keys);
    void bibliographyRequested(const Scope& scope, const QStringList& keys);
    void retrieveMetadataRequested(qint64 libraryId, const QStringList& keys);
    void newLibraryRequested();
    void deleteLibraryRequested(qint64 libraryId);
    void selectionChanged();
    void statusMessage(const QString& text);  // for the status bar; empty: done

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;
    // Esc in a field that has no use for it (the tag filter, the item's fields, a note…):
    // the keys back on the item list; Esc on the item list: nothing selected
    void keyPressEvent(QKeyEvent* e) override;

private:
    void importFilesTo(const Scope& target, const QStringList& files, const QString& parentKey);
    void onScopeChanged(const Scope& scope);
    void onSelectionChanged();
    void reloadItems();
    void openIndex(const QModelIndex& index);
    void openSelected();
    bool isStandaloneNote(const QString& key) const;
    void showItemMenu(const QPoint& pos);
    void selectKeys(const QStringList& keys, const QString& current);
    void fitColumns();
    void updateBandBackground();

    CollectionTree* tree_;
    TagSelector* tags_;
    SearchField* search_;
    QComboBox* searchMode_;
    QHBoxLayout* searchRow_;
    QWidget* middle_;
    QToolButton* paneToggle_;
    bool fitting_ = false;
    bool relayout_ = false;              // the rows rearranged (sorted): widths kept as below
    std::vector<int> relayoutWidths_;    // the columns' widths before, -1 for hidden ones
    int leftMinimum_ = 0;  // the collections pane's own minimum, while it is pinned
    int listMinimum_ = 0;  // room the list needs for the title's minimum
    int tools_ = 0;
    QTreeView* items_;
    ItemsModel* model_;
    ItemPane* pane_;
    QSplitter* splitter_;
    QSplitter* leftSplitter_;
    QTimer reloadTimer_;
    QTimer searchTimer_;
    QTimer bandTimer_;  // the next band, at the next even hour
    QString bandFile_;  // the picture behind the list now, empty for none
    QString pendingSelect_;
};

} // namespace on::gui
