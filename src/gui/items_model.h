#pragma once

#include <QAbstractItemModel>
#include <QIcon>
#include <QSet>

#include <map>
#include <memory>
#include <set>

#include "core/library.h"

namespace fp::gui {

enum class ScopeKind { Library, Collection, Unfiled, Duplicates, Trash };

// What the item list shows: a whole library, one collection, unfiled items, ...
struct Scope {
    int64_t libraryId = 0;
    ScopeKind kind = ScopeKind::Library;
    std::string collectionKey;

    bool operator==(const Scope& o) const
    {
        return libraryId == o.libraryId && kind == o.kind && collectionKey == o.collectionKey;
    }
};

// Tree model of the item list: top-level items with their notes and attachments.
class ItemsModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Title, Creator, Year, ItemType, Date, Publication, DateAdded, DateModified, Attachments, Notes,
                  ColumnCount };
    enum Role { KeyRole = Qt::UserRole + 1, TypeRole, ContentTypeRole, LinkModeRole, SortRole };

    explicit ItemsModel(QObject* parent = nullptr);

    void setScope(const Scope& scope);
    const Scope& scope() const { return scope_; }
    void setSearch(const QString& text, SearchMode mode);
    void setTagFilter(const QStringList& tags);
    void reload();

    QString keyAt(const QModelIndex& index) const;
    QString typeAt(const QModelIndex& index) const;
    QModelIndex indexOfKey(const QString& key) const;
    int topLevelCount() const { return (int)roots_.size(); }
    bool isSearching() const { return !search_.isEmpty() || !tags_.isEmpty(); }
    static QString columnName(int column);

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDropActions() const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                         const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;

signals:
    // Files dropped onto the list (parentKey empty) or onto an item.
    void filesDropped(const QStringList& paths, const QString& parentKey);

private:
    struct Node {
        ItemRow row;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        int notes = 0;
        QString bestType;      // attachment icon in the Attachments column
        QString bestContent;
        QString bestLinkMode;
        bool fileMissing = false;
        bool matches = true;   // false: shown only for context (non-matching child)
    };

    QVariant sortValue(const Node& n, int column) const;
    void sortNodes(std::vector<std::unique_ptr<Node>>& nodes);
    const Node* nodeAt(const QModelIndex& index) const;

    Scope scope_;
    QString search_;
    SearchMode mode_ = SearchMode::AllFields;
    QStringList tags_;
    int sortColumn_ = Title;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    std::vector<std::unique_ptr<Node>> roots_;
    std::map<std::string, Node*> byKey_;
};

} // namespace fp::gui
