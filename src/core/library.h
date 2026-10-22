#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/sqlite.h"
#include "core/util.h"

namespace on {

class FulltextIndex;

struct LibraryInfo {
    int64_t id = 0;
    std::string uid;   // globally unique; identifies the library on sync targets
    std::string name;
    std::string type;  // "user" | "group"
    bool editable = true;
    std::string syncTarget;   // id of the target sync_version refers to
    int64_t syncVersion = 0;  // last remote library version pulled
    std::string syncName;     // the name last agreed with that target ("" before the first)
};

// What keeps a library from being deleted: entries in it or in its trash (saved
// searches go with it).
struct LibraryContents {
    int64_t items = 0;        // top-level items
    int64_t collections = 0;
    int64_t other = 0;        // child items (notes, attachments, annotations)
    bool empty() const { return items == 0 && collections == 0 && other == 0; }
};

// Flat item row for list views (derived columns only, no JSON parsing needed).
struct ItemRow {
    std::string key;
    std::string itemType;
    std::string parentKey;
    std::string title;
    std::string creator;
    std::string year;
    std::string date;
    std::string publication;
    std::string dateAdded;
    std::string dateModified;
    std::string contentType;  // attachments
    std::string linkMode;     // attachments
    bool trashed = false;
};

struct TagInfo {
    std::string name;
    int type = 0;  // 0 manual, 1 automatic
    int count = 0;
    std::string color;  // assigned colour (tagColors), empty if none
    int position = -1;  // order among coloured tags
};

struct ChangeEvent {
    int64_t libraryId = 0;  // 0: all libraries
    std::string type;       // "item" | "collection" | "setting" | "library" | "*"
    std::string action;     // "add" | "modify" | "delete" | "refresh"
    std::vector<std::string> keys;
};

enum class SearchMode { TitleCreatorYear, AllFields, Everything };

// Generic view of a synced object (item, collection, setting, search).
struct StoredObject {
    Json data;
    int64_t version = 0;
    bool synced = false;
    int64_t rev = 0;
};

struct DirtyObject {
    std::string type;
    std::string key;
    Json data;
    int64_t version = 0;
    int64_t rev = 0;
};

struct Tombstone {
    std::string type;
    std::string key;
    int64_t version = 0;
};

struct FileState {
    int64_t size = 0;
    int64_t mtimeNs = 0;
    std::string md5;        // md5 of the file at (size, mtime)
    std::string syncedMd5;  // md5 last uploaded/downloaded
};

struct Conflict {
    int64_t id = 0;
    int64_t libraryId = 0;
    std::string type;
    std::string key;
    std::string kept;  // "local" | "remote"
    Json local;
    Json remote;
    std::string created;
};

// The local library database: <dataDir>/ournotes.sqlite plus attachment files in
// <dataDir>/storage/<KEY>/. Item data uses the Zotero Web API JSON shape.
//
// An instance must only be used from one thread; other threads (sync, indexing)
// open their own instance on the same data directory.
class Library {
public:
    explicit Library(const fs::path& dataDir);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    const fs::path& dataDir() const { return dataDir_; }
    fs::path storageDir() const { return dataDir_ / "storage"; }
    sql::Db& db() { return db_; }
    FulltextIndex& fulltext();

    // --- change notification ---------------------------------------------------------
    using Listener = std::function<void(const ChangeEvent&)>;
    int subscribe(Listener listener);
    void unsubscribe(int id);
    // Group many changes: events are queued and delivered (merged) when the
    // outermost batch ends.
    void beginBatch();
    void endBatch();
    class BatchGuard {
    public:
        explicit BatchGuard(Library& lib) : lib_(lib) { lib_.beginBatch(); }
        ~BatchGuard() { lib_.endBatch(); }

    private:
        Library& lib_;
    };

    // --- libraries -------------------------------------------------------------------
    std::vector<LibraryInfo> libraries();
    std::optional<LibraryInfo> library(int64_t id);
    std::optional<LibraryInfo> libraryByUid(const std::string& uid);
    int64_t createLibrary(const std::string& name, const std::string& type = "user", const std::string& uid = {});
    void renameLibrary(int64_t id, const std::string& name);
    void deleteLibrary(int64_t id);
    static constexpr const char* kDefaultName = "My Library";
    int64_t defaultLibrary();  // creates "My Library" when there is none
    void setSyncState(int64_t id, const std::string& target, int64_t version);
    void setSyncName(int64_t id, const std::string& name);
    void setLibraryEditable(int64_t id, bool editable);
    LibraryContents libraryContents(int64_t id);  // trash included

    // --- items -----------------------------------------------------------------------
    std::optional<Json> item(int64_t lib, const std::string& key);
    std::vector<Json> items(int64_t lib, const std::vector<std::string>& keys);
    std::vector<Json> allItems(int64_t lib, bool includeTrashed = false);  // every type incl. annotations
    // Insert a new item; assigns key/dateAdded/dateModified when missing.
    Json createItem(int64_t lib, Json data);
    // Insert or replace an object coming from an importer, keeping its own dates.
    // It counts as a local change (pushed on the next sync).
    void importObject(int64_t lib, const std::string& type, Json data);
    // Save a locally edited item (sets dateModified, marks it for sync).
    void saveItem(int64_t lib, Json data);
    bool updateItem(int64_t lib, const std::string& key, const std::function<void(Json&)>& edit);
    // Notes and attachments become parentKey's (a regular item), or standalone when it
    // is empty (then in collectionKey, if given). Returns how many moved.
    int moveToParent(int64_t lib, const std::vector<std::string>& keys, const std::string& parentKey,
                     const std::string& collectionKey = {});
    void trashItems(int64_t lib, const std::vector<std::string>& keys, bool trashed = true);
    void eraseItems(int64_t lib, const std::vector<std::string>& keys);  // permanent, with children and files
    void emptyTrash(int64_t lib);
    std::vector<ItemRow> itemRows(int64_t lib);  // all items except annotations
    std::vector<Json> children(int64_t lib, const std::string& parentKey, const std::string& itemType = {});
    std::vector<Json> annotations(int64_t lib, const std::string& attachmentKey);
    int annotationCount(int64_t lib, const std::string& attachmentKey);
    std::string topLevelKey(int64_t lib, const std::string& key);
    std::optional<Json> bestAttachment(int64_t lib, const std::string& parentKey);
    std::vector<std::string> relatedKeys(int64_t lib, const std::string& key);
    void addRelated(int64_t lib, const std::string& a, const std::string& b);
    void removeRelated(int64_t lib, const std::string& a, const std::string& b);
    std::vector<std::vector<std::string>> duplicateGroups(int64_t lib);
    // Copies items into another library (or this one), each with what is under it: notes
    // with their images, attachments with their files, annotations; with new keys, into
    // collectionKey (if given), without relations. A note or attachment copied without its
    // parent stands on its own; what is in the trash is left out. Returns the new keys of
    // the copies of `keys`, in that order.
    std::vector<std::string> copyItems(int64_t fromLib, const std::vector<std::string>& keys, int64_t toLib,
                                       const std::string& collectionKey = {});
    // Merge `others` into `master`: moves children, collections, tags, relations; trashes others.
    void mergeItems(int64_t lib, const std::string& master, const std::vector<std::string>& others);

    // --- notes & attachments ---------------------------------------------------------
    Json createNote(int64_t lib, const std::string& html, const std::string& parentKey = {});
    // Copy (or link) a file into the library as an attachment item.
    Json importAttachment(int64_t lib, const fs::path& src, const std::string& parentKey = {}, bool link = false,
                          const std::string& title = {});
    Json createLinkedUrl(int64_t lib, const std::string& url, const std::string& title,
                         const std::string& parentKey = {});
    fs::path attachmentFile(int64_t lib, const Json& attachment);  // empty when not a file attachment
    bool attachmentFileExists(int64_t lib, const Json& attachment);
    // Recompute md5/mtime of a stored attachment after the file changed on disk.
    bool refreshAttachmentFile(int64_t lib, const std::string& key);
    std::string fileMd5(int64_t lib, const std::string& key, const fs::path& path);  // cached by size+mtime

    // What the library takes on disk, and room left: {dataDir, database, fulltextIndex
    // (bytes), storage {files, bytes} (everything in storage/), disk (see diskSpace),
    // libraries: {id, name, items, notes, attachments, annotations, trashed, files,
    // fileBytes, missingFiles (not downloaded)}}. Attachments' files are counted, in the
    // trash too; the rest leaves out what is in the trash.
    Json diskUsage();

    // --- collections -----------------------------------------------------------------
    std::vector<Json> collections(int64_t lib);
    std::optional<Json> collection(int64_t lib, const std::string& key);
    Json createCollection(int64_t lib, const std::string& name, const std::string& parentKey = {});
    void saveCollection(int64_t lib, Json data);
    void deleteCollection(int64_t lib, const std::string& key, bool trashItems = false);
    void addToCollection(int64_t lib, const std::string& collectionKey, const std::vector<std::string>& itemKeys);
    void removeFromCollection(int64_t lib, const std::string& collectionKey, const std::vector<std::string>& itemKeys);
    std::set<std::string> collectionItemKeys(int64_t lib, const std::string& collectionKey, bool recursive = false);
    std::set<std::string> unfiledItemKeys(int64_t lib);
    std::vector<std::string> descendantCollections(int64_t lib, const std::string& key);

    // --- tags ------------------------------------------------------------------------
    std::vector<TagInfo> tags(int64_t lib);
    std::set<std::string> itemKeysWithTags(int64_t lib, const std::vector<std::string>& tags);  // AND
    void addTag(int64_t lib, const std::vector<std::string>& keys, const std::string& tag, int type = 0);
    void removeTag(int64_t lib, const std::vector<std::string>& keys, const std::string& tag);
    void renameTag(int64_t lib, const std::string& from, const std::string& to);
    void deleteTag(int64_t lib, const std::string& tag);
    std::map<std::string, std::string> tagColors(int64_t lib);
    void setTagColor(int64_t lib, const std::string& tag, const std::string& color, int position = -1);

    // --- settings (synced, per library) --------------------------------------------------
    Json setting(int64_t lib, const std::string& name, const Json& def = nullptr);
    void setSetting(int64_t lib, const std::string& name, const Json& value);  // null removes

    // --- reader state (local) ------------------------------------------------------------
    // Where an attachment was left in the reader (page, zoom, scroll position), kept in
    // storage/<KEY>/.zotero-reader-state as Zotero does; not synced. Empty if unknown.
    Json readerState(const std::string& attachmentKey) const;
    void setReaderState(const std::string& attachmentKey, const Json& state);

    // --- search --------------------------------------------------------------------------
    // Keys of all items (any level) matching every token of `query`; caseSensitive: each
    // word as typed (otherwise in any case, and with or without accents).
    std::set<std::string> search(int64_t lib, const std::string& query, SearchMode mode, bool caseSensitive = false);

    // --- sync support (used by SyncEngine) -------------------------------------------------
    std::optional<StoredObject> getObject(int64_t lib, const std::string& type, const std::string& key);
    void putRemoteObject(int64_t lib, const std::string& type, const std::string& key, const Json& data,
                         int64_t version);
    void deleteRemoteObject(int64_t lib, const std::string& type, const std::string& key);
    // After a push: record the server version; the object is clean again unless it
    // was modified (rev changed) in the meantime.
    void markPushed(int64_t lib, const std::string& type, const std::string& key, int64_t version, int64_t rev);
    void setObjectVersion(int64_t lib, const std::string& type, const std::string& key, int64_t version);
    std::vector<DirtyObject> dirtyObjects(int64_t lib);
    std::vector<Tombstone> tombstones(int64_t lib);
    std::optional<Tombstone> tombstone(int64_t lib, const std::string& type, const std::string& key);
    void clearTombstone(int64_t lib, const std::string& type, const std::string& key);
    void resetSyncState(int64_t lib, const std::string& target);  // everything dirty, version 0
    void recordConflict(int64_t lib, const std::string& type, const std::string& key, const Json& local,
                        const Json& remote, const std::string& kept);
    std::vector<Conflict> conflicts(int64_t lib = 0);
    void deleteConflict(int64_t id);
    std::optional<FileState> fileState(int64_t lib, const std::string& key);
    void setSyncedMd5(int64_t lib, const std::string& key, const std::string& md5);

    // Notify listeners that everything may have changed (e.g. after a sync in another thread).
    void notifyRefresh(int64_t lib = 0);

private:
    void migrate();
    void notify(ChangeEvent ev);
    std::string tableFor(const std::string& type) const;
    void writeItem(int64_t lib, Json data, int64_t version, bool synced, bool bumpRev);
    void writeObject(int64_t lib, const std::string& type, const std::string& key, Json data, int64_t version,
                     bool synced, bool bumpRev);
    void removeObject(int64_t lib, const std::string& type, const std::string& key, bool tombstone);
    void collectDescendants(int64_t lib, const std::string& key, std::vector<std::string>& out);
    void removeStorage(const std::string& key);
    void saveTagColors(int64_t lib, const Json& colors);

    fs::path dataDir_;
    sql::Db db_;
    std::unique_ptr<FulltextIndex> fulltext_;
    std::map<int, Listener> listeners_;
    int nextListener_ = 1;
    int batchDepth_ = 0;
    std::vector<ChangeEvent> queued_;
};

// Whether an item key reference in `relations` (bare key or Zotero URI) points at `key`.
std::string relationTargetKey(const std::string& value);

} // namespace on
