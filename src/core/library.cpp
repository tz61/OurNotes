#include "core/library.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <numeric>

#include "core/fulltext.h"
#include "core/hash.h"
#include "core/schema.h"

namespace on {

namespace {

constexpr int kSchemaVersion = 2;

const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS libraries(
    id INTEGER PRIMARY KEY,
    uid TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    type TEXT NOT NULL DEFAULT 'user',
    editable INTEGER NOT NULL DEFAULT 1,
    sync_target TEXT,
    sync_version INTEGER NOT NULL DEFAULT 0,
    created TEXT NOT NULL DEFAULT '',
    sync_name TEXT);
CREATE TABLE IF NOT EXISTS items(
    library_id INTEGER NOT NULL,
    key TEXT NOT NULL,
    item_type TEXT NOT NULL,
    parent_key TEXT,
    title TEXT NOT NULL DEFAULT '',
    creator TEXT NOT NULL DEFAULT '',
    year TEXT NOT NULL DEFAULT '',
    date TEXT NOT NULL DEFAULT '',
    publication TEXT NOT NULL DEFAULT '',
    date_added TEXT NOT NULL DEFAULT '',
    date_modified TEXT NOT NULL DEFAULT '',
    content_type TEXT NOT NULL DEFAULT '',
    link_mode TEXT NOT NULL DEFAULT '',
    trashed INTEGER NOT NULL DEFAULT 0,
    quick_text TEXT NOT NULL DEFAULT '',
    search_text TEXT NOT NULL DEFAULT '',
    version INTEGER NOT NULL DEFAULT 0,
    synced INTEGER NOT NULL DEFAULT 0,
    rev INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    PRIMARY KEY(library_id, key)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS items_parent ON items(library_id, parent_key);
CREATE INDEX IF NOT EXISTS items_dirty ON items(library_id, synced);
CREATE TABLE IF NOT EXISTS item_tags(
    library_id INTEGER NOT NULL,
    item_key TEXT NOT NULL,
    tag TEXT NOT NULL,
    type INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(library_id, item_key, tag)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS item_tags_tag ON item_tags(library_id, tag);
CREATE TABLE IF NOT EXISTS item_collections(
    library_id INTEGER NOT NULL,
    collection_key TEXT NOT NULL,
    item_key TEXT NOT NULL,
    PRIMARY KEY(library_id, collection_key, item_key)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS item_collections_item ON item_collections(library_id, item_key);
CREATE TABLE IF NOT EXISTS collections(
    library_id INTEGER NOT NULL,
    key TEXT NOT NULL,
    name TEXT NOT NULL DEFAULT '',
    parent_key TEXT,
    version INTEGER NOT NULL DEFAULT 0,
    synced INTEGER NOT NULL DEFAULT 0,
    rev INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    PRIMARY KEY(library_id, key)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS settings(
    library_id INTEGER NOT NULL,
    key TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 0,
    synced INTEGER NOT NULL DEFAULT 0,
    rev INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    PRIMARY KEY(library_id, key)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS searches(
    library_id INTEGER NOT NULL,
    key TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 0,
    synced INTEGER NOT NULL DEFAULT 0,
    rev INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL,
    PRIMARY KEY(library_id, key)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS tombstones(
    library_id INTEGER NOT NULL,
    type TEXT NOT NULL,
    key TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 0,
    date_deleted TEXT NOT NULL DEFAULT '',
    PRIMARY KEY(library_id, type, key)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS conflicts(
    id INTEGER PRIMARY KEY,
    library_id INTEGER NOT NULL,
    type TEXT NOT NULL,
    key TEXT NOT NULL,
    kept TEXT NOT NULL,
    local_data TEXT,
    remote_data TEXT,
    created TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS file_state(
    library_id INTEGER NOT NULL,
    key TEXT NOT NULL,
    size INTEGER NOT NULL DEFAULT 0,
    mtime_ns INTEGER NOT NULL DEFAULT 0,
    md5 TEXT NOT NULL DEFAULT '',
    synced_md5 TEXT NOT NULL DEFAULT '',
    PRIMARY KEY(library_id, key)) WITHOUT ROWID;
)SQL";

const std::set<std::string> kMetaKeys = {"key",       "version",   "itemType", "dateAdded", "dateModified", "tags",
                                         "collections", "relations", "creators", "deleted",   "parentItem"};

struct Derived {
    std::string title, creator, year, date, publication, contentType, linkMode, parentKey;
    bool trashed = false;
    std::string quick, search;        // as stored, folded (see normalize)
    std::string quickRaw, searchRaw;  // the same as written, for a search that minds case
};

std::string creatorsText(const Json& creators)
{
    std::string out;
    for (auto& c : creators) {
        out += ' ' + jstr(c, "firstName") + ' ' + jstr(c, "lastName") + ' ' + jstr(c, "name");
    }
    return out;
}

std::string tagsText(const Json& data)
{
    std::string out;
    for (auto& t : data.value("tags", Json::array()))
        out += ' ' + jstr(t, "tag");
    return out;
}

Derived derive(const Json& d)
{
    Derived r;
    std::string type = jstr(d, "itemType");
    r.parentKey = jstr(d, "parentItem");
    r.trashed = jbool(d, "deleted");
    r.date = jstr(d, "date");
    r.year = yearOf(r.date);
    if (type == "note") {
        r.title = noteTitle(jstr(d, "note"));
        r.quickRaw = r.title;
        r.searchRaw = htmlToText(jstr(d, "note")) + tagsText(d);
    } else if (type == "attachment") {
        r.title = jstr(d, "title");
        if (r.title.empty())
            r.title = jstr(d, "filename");
        r.contentType = jstr(d, "contentType");
        r.linkMode = jstr(d, "linkMode");
        r.quickRaw = r.title;
        r.searchRaw = r.title + ' ' + jstr(d, "filename") + ' ' + jstr(d, "url") + ' ' + htmlToText(jstr(d, "note")) +
                      tagsText(d);
    } else if (type == "annotation") {
        r.title = jstr(d, "annotationText");
        if (r.title.empty())
            r.title = jstr(d, "annotationComment");
        r.quickRaw = r.title;
        r.searchRaw = jstr(d, "annotationText") + ' ' + jstr(d, "annotationComment") + tagsText(d);
    } else {
        r.title = jstr(d, "title");
        r.creator = creatorSummary(d.value("creators", Json::array()), schema::primaryCreatorType(type));
        r.publication = schema::getField(d, "publicationTitle");
        if (r.publication.empty())
            r.publication = schema::getField(d, "repository");
        std::string all;
        for (auto& [k, v] : d.items())
            if (!kMetaKeys.count(k) && v.is_string())
                all += ' ' + v.get<std::string>();
        auto creators = creatorsText(d.value("creators", Json::array()));
        r.quickRaw = r.title + ' ' + creators + ' ' + r.year;
        r.searchRaw = all + ' ' + creators + tagsText(d);
    }
    r.quick = normalize(r.quickRaw);
    r.search = normalize(r.searchRaw);
    return r;
}

std::string likePattern(const std::string& token)
{
    std::string out = "%";
    for (char c : token) {
        if (c == '%' || c == '_' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "%";
}

void ensureArrays(Json& data)
{
    if (!data.contains("tags") || !data["tags"].is_array())
        data["tags"] = Json::array();
    if (!data.contains("relations") || !data["relations"].is_object())
        data["relations"] = Json::object();
    std::string type = jstr(data, "itemType");
    bool topLevel = jstr(data, "parentItem").empty();
    if (type != "annotation" && topLevel && (!data.contains("collections") || !data["collections"].is_array()))
        data["collections"] = Json::array();
    if (!topLevel)
        data.erase("collections");
    if (data.contains("parentItem") && !data["parentItem"].is_string())
        data.erase("parentItem");
    data.erase("version");
}

} // namespace

std::string relationTargetKey(const std::string& value)
{
    if (value.size() == 8 && isValidKey(value))
        return value;
    auto pos = value.rfind("/items/");
    if (pos != std::string::npos) {
        auto key = value.substr(pos + 7);
        if (isValidKey(key))
            return key;
    }
    return {};
}

// ===================================================================================

Library::Library(const fs::path& dataDir) : dataDir_(dataDir)
{
    fs::create_directories(dataDir_);
    fs::create_directories(storageDir());
    // the database of a library from before the rename to OurNotes
    std::error_code ec;
    if (!fs::exists(dataDir_ / "ournotes.sqlite", ec) && fs::exists(dataDir_ / "freepen.sqlite", ec))
        for (const char* suffix : {"", "-wal", "-shm", "-journal"})
            if (fs::exists(dataDir_ / ("freepen.sqlite" + std::string(suffix)), ec))
                fs::rename(dataDir_ / ("freepen.sqlite" + std::string(suffix)),
                           dataDir_ / ("ournotes.sqlite" + std::string(suffix)), ec);
    db_.open(dataDir_ / "ournotes.sqlite");
    db_.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=OFF;");
    migrate();
}

Library::~Library() = default;

void Library::migrate()
{
    int v = db_.userVersion();
    if (v >= kSchemaVersion)
        return;
    sql::Transaction tx(db_);
    db_.exec(kSchemaSql);
    // 2: the library name last agreed with the sync target (names are synced)
    if (!db_.scalarInt("SELECT 1 FROM pragma_table_info('libraries') WHERE name='sync_name'"))
        db_.exec("ALTER TABLE libraries ADD COLUMN sync_name TEXT");
    if (!db_.scalarStr("SELECT value FROM meta WHERE key='created'"))
        db_.run("INSERT INTO meta(key, value) VALUES('created', ?)", nowIso());
    db_.setUserVersion(kSchemaVersion);
    tx.commit();
}

FulltextIndex& Library::fulltext()
{
    if (!fulltext_)
        fulltext_ = std::make_unique<FulltextIndex>(dataDir_ / "fulltext.sqlite");
    return *fulltext_;
}

// --- notifications -------------------------------------------------------------------

int Library::subscribe(Listener l)
{
    listeners_[nextListener_] = std::move(l);
    return nextListener_++;
}

void Library::unsubscribe(int id) { listeners_.erase(id); }

void Library::beginBatch() { ++batchDepth_; }

void Library::endBatch()
{
    if (--batchDepth_ > 0)
        return;
    auto queued = std::move(queued_);
    queued_.clear();
    std::map<std::tuple<int64_t, std::string, std::string>, std::set<std::string>> merged;
    for (auto& ev : queued)
        merged[{ev.libraryId, ev.type, ev.action}].insert(ev.keys.begin(), ev.keys.end());
    for (auto& [k, keys] : merged)
        notify({std::get<0>(k), std::get<1>(k), std::get<2>(k), {keys.begin(), keys.end()}});
}

void Library::notify(ChangeEvent ev)
{
    if (batchDepth_ > 0) {
        queued_.push_back(std::move(ev));
        return;
    }
    auto copy = listeners_;
    for (auto& [id, l] : copy)
        l(ev);
}

void Library::notifyRefresh(int64_t lib) { notify({lib, "*", "refresh", {}}); }

// --- libraries ------------------------------------------------------------------------

static LibraryInfo readLibrary(sql::Stmt& st)
{
    LibraryInfo l;
    l.id = st.i64(0);
    l.uid = st.str(1);
    l.name = st.str(2);
    l.type = st.str(3);
    l.editable = st.i64(4) != 0;
    l.syncTarget = st.str(5);
    l.syncVersion = st.i64(6);
    l.syncName = st.str(7);
    return l;
}

static const char* kLibraryCols =
    "SELECT id, uid, name, type, editable, sync_target, sync_version, sync_name FROM libraries ";

std::vector<LibraryInfo> Library::libraries()
{
    std::vector<LibraryInfo> out;
    auto st = db_.query(std::string(kLibraryCols) + "ORDER BY (type != 'user'), name COLLATE NOCASE, id");
    while (st.step())
        out.push_back(readLibrary(st));
    return out;
}

std::optional<LibraryInfo> Library::library(int64_t id)
{
    auto st = db_.query(std::string(kLibraryCols) + "WHERE id=?", id);
    if (st.step())
        return readLibrary(st);
    return std::nullopt;
}

std::optional<LibraryInfo> Library::libraryByUid(const std::string& uid)
{
    auto st = db_.query(std::string(kLibraryCols) + "WHERE uid=?", uid);
    if (st.step())
        return readLibrary(st);
    return std::nullopt;
}

int64_t Library::createLibrary(const std::string& name, const std::string& type, const std::string& uid)
{
    std::string id = uid.empty() ? randomToken(8) : uid;
    db_.run("INSERT INTO libraries(uid, name, type, created) VALUES(?,?,?,?)", id, name, type, nowIso());
    int64_t rowid = db_.lastInsertId();
    notify({rowid, "library", "add", {}});
    return rowid;
}

void Library::renameLibrary(int64_t id, const std::string& name)
{
    db_.run("UPDATE libraries SET name=? WHERE id=?", name, id);
    notify({id, "library", "modify", {}});
}

void Library::deleteLibrary(int64_t id)
{
    std::vector<std::string> stored;
    {
        auto st = db_.query("SELECT key FROM items WHERE library_id=? AND item_type='attachment'", id);
        while (st.step())
            stored.push_back(st.str(0));
    }
    sql::Transaction tx(db_);
    for (const char* table : {"items", "item_tags", "item_collections", "collections", "settings", "searches",
                              "tombstones", "conflicts", "file_state"})
        db_.run(std::string("DELETE FROM ") + table + " WHERE library_id=?", id);
    db_.run("DELETE FROM libraries WHERE id=?", id);
    tx.commit();
    for (auto& key : stored)
        removeStorage(key);
    fulltext().removeLibrary(id);
    notify({id, "library", "delete", {}});
}

int64_t Library::defaultLibrary()
{
    if (auto id = db_.scalarInt("SELECT id FROM libraries WHERE type='user' ORDER BY id LIMIT 1"))
        return *id;
    if (auto id = db_.scalarInt("SELECT id FROM libraries ORDER BY id LIMIT 1"))
        return *id;
    return createLibrary(kDefaultName, "user");
}

void Library::setSyncState(int64_t id, const std::string& target, int64_t version)
{
    db_.run("UPDATE libraries SET sync_target=?, sync_version=? WHERE id=?", target, version, id);
}

void Library::setSyncName(int64_t id, const std::string& name)
{
    db_.run("UPDATE libraries SET sync_name=? WHERE id=?", name, id);
}

LibraryContents Library::libraryContents(int64_t id)
{
    LibraryContents c;
    c.items = db_.scalarInt("SELECT count(*) FROM items WHERE library_id=? AND (parent_key IS NULL OR parent_key='')",
                            id).value_or(0);
    c.collections = db_.scalarInt("SELECT count(*) FROM collections WHERE library_id=?", id).value_or(0);
    c.other = db_.scalarInt("SELECT count(*) FROM items WHERE library_id=? AND parent_key IS NOT NULL"
                            " AND parent_key!=''",
                            id).value_or(0);
    return c;
}

void Library::setLibraryEditable(int64_t id, bool editable)
{
    db_.run("UPDATE libraries SET editable=? WHERE id=?", editable, id);
}

// --- object storage -----------------------------------------------------------------------

std::string Library::tableFor(const std::string& type) const
{
    if (type == "item")
        return "items";
    if (type == "collection")
        return "collections";
    if (type == "setting")
        return "settings";
    if (type == "search")
        return "searches";
    throw Error("unknown object type: " + type);
}

void Library::writeItem(int64_t lib, Json data, int64_t version, bool synced, bool bumpRev)
{
    ensureArrays(data);
    auto key = jstr(data, "key");
    if (!isValidKey(key))
        throw Error("invalid item key: " + key);
    if (!schema::isValidType(jstr(data, "itemType")))
        throw Error("invalid item type: " + jstr(data, "itemType"));
    auto d = derive(data);
    sql::Transaction tx(db_);
    db_.query(R"SQL(
        INSERT INTO items(library_id, key, item_type, parent_key, title, creator, year, date, publication,
                          date_added, date_modified, content_type, link_mode, trashed, quick_text, search_text,
                          version, synced, rev, data)
        VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20)
        ON CONFLICT(library_id, key) DO UPDATE SET
            item_type=excluded.item_type, parent_key=excluded.parent_key, title=excluded.title,
            creator=excluded.creator, year=excluded.year, date=excluded.date, publication=excluded.publication,
            date_added=excluded.date_added, date_modified=excluded.date_modified,
            content_type=excluded.content_type, link_mode=excluded.link_mode, trashed=excluded.trashed,
            quick_text=excluded.quick_text, search_text=excluded.search_text, version=excluded.version,
            synced=excluded.synced, rev=items.rev + ?19, data=excluded.data)SQL")
        .bindAll(lib, key, jstr(data, "itemType"), d.parentKey.empty() ? std::optional<std::string>() : d.parentKey,
                 d.title, d.creator, d.year, d.date, d.publication, jstr(data, "dateAdded"),
                 jstr(data, "dateModified"), d.contentType, d.linkMode, d.trashed, d.quick, d.search, version,
                 synced, (int64_t)(bumpRev ? 1 : 0), data.dump())
        .exec();
    db_.run("DELETE FROM item_tags WHERE library_id=? AND item_key=?", lib, key);
    for (auto& t : data["tags"]) {
        auto tag = trim(jstr(t, "tag"));
        if (!tag.empty())
            db_.run("INSERT OR IGNORE INTO item_tags(library_id, item_key, tag, type) VALUES(?,?,?,?)", lib, key, tag,
                    jint(t, "type"));
    }
    db_.run("DELETE FROM item_collections WHERE library_id=? AND item_key=?", lib, key);
    if (data.contains("collections"))
        for (auto& c : data["collections"])
            if (c.is_string())
                db_.run("INSERT OR IGNORE INTO item_collections(library_id, collection_key, item_key) VALUES(?,?,?)",
                        lib, c.get<std::string>(), key);
    tx.commit();
}

void Library::writeObject(int64_t lib, const std::string& type, const std::string& key, Json data, int64_t version,
                          bool synced, bool bumpRev)
{
    if (type == "item") {
        data["key"] = key;
        writeItem(lib, std::move(data), version, synced, bumpRev);
        return;
    }
    data.erase("version");
    if (type == "collection") {
        data["key"] = key;
        auto parent = data.value("parentCollection", Json());
        std::optional<std::string> parentKey;
        if (parent.is_string() && !parent.get<std::string>().empty())
            parentKey = parent.get<std::string>();
        db_.query(R"SQL(
            INSERT INTO collections(library_id, key, name, parent_key, version, synced, rev, data)
            VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
            ON CONFLICT(library_id, key) DO UPDATE SET name=excluded.name, parent_key=excluded.parent_key,
                version=excluded.version, synced=excluded.synced, rev=collections.rev + ?7, data=excluded.data)SQL")
            .bindAll(lib, key, jstr(data, "name"), parentKey, version, synced, (int64_t)(bumpRev ? 1 : 0), data.dump())
            .exec();
        return;
    }
    auto table = tableFor(type);
    db_.query("INSERT INTO " + table + R"SQL((library_id, key, version, synced, rev, data)
            VALUES(?1, ?2, ?3, ?4, ?5, ?6)
            ON CONFLICT(library_id, key) DO UPDATE SET version=excluded.version, synced=excluded.synced,
                rev=rev + ?5, data=excluded.data)SQL")
        .bindAll(lib, key, version, synced, (int64_t)(bumpRev ? 1 : 0), data.dump())
        .exec();
}

void Library::removeObject(int64_t lib, const std::string& type, const std::string& key, bool tombstone)
{
    auto table = tableFor(type);
    sql::Transaction tx(db_);
    auto version = db_.scalarInt("SELECT version FROM " + table + " WHERE library_id=? AND key=?", lib, key);
    db_.run("DELETE FROM " + table + " WHERE library_id=? AND key=?", lib, key);
    if (type == "item") {
        db_.run("DELETE FROM item_tags WHERE library_id=? AND item_key=?", lib, key);
        db_.run("DELETE FROM item_collections WHERE library_id=? AND item_key=?", lib, key);
        db_.run("DELETE FROM file_state WHERE library_id=? AND key=?", lib, key);
    } else if (type == "collection") {
        db_.run("DELETE FROM item_collections WHERE library_id=? AND collection_key=?", lib, key);
    }
    if (tombstone && version && *version > 0)
        db_.run("INSERT OR REPLACE INTO tombstones(library_id, type, key, version, date_deleted) VALUES(?,?,?,?,?)",
                lib, type, key, *version, nowIso());
    tx.commit();
}

// --- items ------------------------------------------------------------------------------

std::optional<Json> Library::item(int64_t lib, const std::string& key)
{
    if (auto s = db_.scalarStr("SELECT data FROM items WHERE library_id=? AND key=?", lib, key))
        return Json::parse(*s);
    return std::nullopt;
}

std::vector<Json> Library::items(int64_t lib, const std::vector<std::string>& keys)
{
    std::vector<Json> out;
    auto st = db_.prepare("SELECT data FROM items WHERE library_id=? AND key=?");
    for (auto& k : keys) {
        st.reset();
        st.bindAll(lib, k);
        if (st.step())
            out.push_back(Json::parse(st.str(0)));
    }
    return out;
}

std::vector<Json> Library::allItems(int64_t lib, bool includeTrashed)
{
    std::vector<Json> out;
    auto st = db_.query(std::string("SELECT data FROM items WHERE library_id=?") + (includeTrashed ? "" : " AND trashed=0"),
                        lib);
    while (st.step())
        out.push_back(Json::parse(st.str(0)));
    return out;
}

Json Library::createItem(int64_t lib, Json data)
{
    if (!data.contains("key") || !isValidKey(jstr(data, "key")))
        data["key"] = newKey();
    auto now = nowIso();
    if (jstr(data, "dateAdded").empty())
        data["dateAdded"] = now;
    if (jstr(data, "dateModified").empty())
        data["dateModified"] = now;
    // Re-creating a deleted item (undoing a deletion) before the deletion was synced:
    // as far as the server knows it was never deleted.
    int64_t version = 0;
    if (auto t = tombstone(lib, "item", jstr(data, "key"))) {
        version = t->version;
        clearTombstone(lib, "item", jstr(data, "key"));
    }
    writeItem(lib, data, version, false, true);
    ensureArrays(data);
    notify({lib, "item", "add", {jstr(data, "key")}});
    return data;
}

void Library::importObject(int64_t lib, const std::string& type, Json data)
{
    auto key = jstr(data, "key");
    auto existing = getObject(lib, type, key);
    writeObject(lib, type, key, std::move(data), existing ? existing->version : 0, false, true);
    notify({lib, type, existing ? "modify" : "add", {key}});
}

void Library::saveItem(int64_t lib, Json data)
{
    auto key = jstr(data, "key");
    auto existing = getObject(lib, "item", key);
    data["dateModified"] = nowIso();
    if (jstr(data, "dateAdded").empty())
        data["dateAdded"] = existing ? jstr(existing->data, "dateAdded", nowIso()) : nowIso();
    writeItem(lib, data, existing ? existing->version : 0, false, true);
    notify({lib, "item", existing ? "modify" : "add", {key}});
}

bool Library::updateItem(int64_t lib, const std::string& key, const std::function<void(Json&)>& edit)
{
    auto data = item(lib, key);
    if (!data)
        return false;
    Json before = *data;
    edit(*data);
    if (*data == before)
        return false;
    saveItem(lib, std::move(*data));
    return true;
}

int Library::moveToParent(int64_t lib, const std::vector<std::string>& keys, const std::string& parentKey,
                          const std::string& collectionKey)
{
    if (!parentKey.empty()) {
        auto parent = item(lib, parentKey);
        if (!parent || !jstr(*parent, "parentItem").empty() || !schema::isRegularType(jstr(*parent, "itemType")))
            return 0;
    }
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    int moved = 0;
    for (auto& key : keys) {
        if (key == parentKey)
            continue;
        moved += updateItem(lib, key, [&](Json& d) {
            auto type = jstr(d, "itemType");
            if (type != "note" && type != "attachment")
                return;  // regular items have no parent, annotations stay with their file
            if (parentKey.empty()) {
                d.erase("parentItem");
                if (!collectionKey.empty())
                    d["collections"] = Json::array({collectionKey});
            } else {
                d["parentItem"] = parentKey;  // a child is in its parent's collections, not its own
            }
        });
    }
    tx.commit();
    return moved;
}

void Library::trashItems(int64_t lib, const std::vector<std::string>& keys, bool trashed)
{
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    for (auto& key : keys)
        updateItem(lib, key, [&](Json& d) {
            if (trashed)
                d["deleted"] = true;
            else
                d.erase("deleted");
        });
    tx.commit();
}

void Library::collectDescendants(int64_t lib, const std::string& key, std::vector<std::string>& out)
{
    std::vector<std::string> kids;
    {
        auto st = db_.query("SELECT key FROM items WHERE library_id=? AND parent_key=?", lib, key);
        while (st.step())
            kids.push_back(st.str(0));
    }
    for (auto& k : kids) {
        out.push_back(k);
        collectDescendants(lib, k, out);
    }
}

void Library::removeStorage(const std::string& key)
{
    std::error_code ec;
    if (isValidKey(key))
        fs::remove_all(storageDir() / key, ec);
}

void Library::eraseItems(int64_t lib, const std::vector<std::string>& keys)
{
    std::vector<std::string> all;
    for (auto& k : keys) {
        all.push_back(k);
        collectDescendants(lib, k, all);
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::vector<std::string> storedFiles;
    {
        sql::Transaction tx(db_);
        for (auto& k : all) {
            auto st = db_.query("SELECT item_type, link_mode FROM items WHERE library_id=? AND key=?", lib, k);
            if (st.step() && st.str(0) == "attachment" && schema::isStoredLinkMode(st.str(1)))
                storedFiles.push_back(k);
            removeObject(lib, "item", k, true);
        }
        tx.commit();
    }
    for (auto& k : storedFiles) {
        removeStorage(k);
        fulltext().remove(lib, k);
    }
    notify({lib, "item", "delete", all});
}

void Library::emptyTrash(int64_t lib)
{
    std::vector<std::string> keys;
    auto st = db_.query("SELECT key FROM items WHERE library_id=? AND trashed=1", lib);
    while (st.step())
        keys.push_back(st.str(0));
    eraseItems(lib, keys);
}

std::vector<ItemRow> Library::itemRows(int64_t lib)
{
    std::vector<ItemRow> out;
    auto st = db_.query(R"SQL(
        SELECT key, item_type, parent_key, title, creator, year, date, publication, date_added, date_modified,
               content_type, link_mode, trashed
        FROM items WHERE library_id=? AND item_type != 'annotation')SQL",
                        lib);
    while (st.step()) {
        ItemRow r;
        r.key = st.str(0);
        r.itemType = st.str(1);
        r.parentKey = st.str(2);
        r.title = st.str(3);
        r.creator = st.str(4);
        r.year = st.str(5);
        r.date = st.str(6);
        r.publication = st.str(7);
        r.dateAdded = st.str(8);
        r.dateModified = st.str(9);
        r.contentType = st.str(10);
        r.linkMode = st.str(11);
        r.trashed = st.i64(12) != 0;
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Json> Library::children(int64_t lib, const std::string& parentKey, const std::string& itemType)
{
    std::vector<Json> out;
    std::string sql = "SELECT data FROM items WHERE library_id=? AND parent_key=?";
    if (!itemType.empty())
        sql += " AND item_type=?";
    sql += " ORDER BY date_added, key";
    auto st = db_.prepare(sql);
    st.bindAll(lib, parentKey);
    if (!itemType.empty())
        st.bind(3, itemType);
    while (st.step())
        out.push_back(Json::parse(st.str(0)));
    return out;
}

std::vector<Json> Library::annotations(int64_t lib, const std::string& attachmentKey)
{
    auto out = children(lib, attachmentKey, "annotation");
    std::stable_sort(out.begin(), out.end(), [](const Json& a, const Json& b) {
        return jstr(a, "annotationSortIndex") < jstr(b, "annotationSortIndex");
    });
    return out;
}

int Library::annotationCount(int64_t lib, const std::string& attachmentKey)
{
    return (int)db_
        .scalarInt("SELECT COUNT(*) FROM items WHERE library_id=? AND parent_key=? AND item_type='annotation'", lib,
                   attachmentKey)
        .value_or(0);
}

std::string Library::topLevelKey(int64_t lib, const std::string& key)
{
    std::string cur = key;
    for (int depth = 0; depth < 8; ++depth) {
        auto parent = db_.scalarStr("SELECT parent_key FROM items WHERE library_id=? AND key=?", lib, cur);
        if (!parent || parent->empty())
            return cur;
        cur = *parent;
    }
    return cur;
}

std::optional<Json> Library::bestAttachment(int64_t lib, const std::string& parentKey)
{
    auto self = item(lib, parentKey);
    if (!self)
        return std::nullopt;
    if (jstr(*self, "itemType") == "attachment")
        return self;
    std::optional<Json> best;
    int bestScore = -1;
    for (auto& att : children(lib, parentKey, "attachment")) {
        if (jbool(att, "deleted"))
            continue;
        auto ct = jstr(att, "contentType");
        bool exists = attachmentFileExists(lib, att);
        int score = (ct == "application/pdf" ? 30 : ct == "application/epub+zip" ? 20 : ct == "text/html" ? 10 : 5) +
                    (exists ? 100 : 0);
        if (jstr(att, "linkMode") == "linked_url")
            score = 0;
        if (score > bestScore) {
            bestScore = score;
            best = att;
        }
    }
    return best;
}

std::vector<std::string> Library::relatedKeys(int64_t lib, const std::string& key)
{
    std::vector<std::string> out;
    auto d = item(lib, key);
    if (!d)
        return out;
    auto rel = d->value("relations", Json::object());
    if (rel.contains("dc:relation")) {
        auto v = rel["dc:relation"];
        if (v.is_string())
            v = Json::array({v});
        for (auto& r : v)
            if (r.is_string()) {
                auto k = relationTargetKey(r.get<std::string>());
                if (!k.empty() && k != key)
                    out.push_back(k);
            }
    }
    return out;
}

static void editRelation(Json& data, const std::string& target, bool add)
{
    auto& rel = data["relations"];
    if (!rel.is_object())
        rel = Json::object();
    Json list = rel.value("dc:relation", Json::array());
    if (list.is_string())
        list = Json::array({list});
    Json out = Json::array();
    for (auto& r : list)
        if (r.is_string() && relationTargetKey(r.get<std::string>()) != target)
            out.push_back(r);
    if (add)
        out.push_back(target);
    if (out.empty())
        rel.erase("dc:relation");
    else
        rel["dc:relation"] = out;
}

void Library::addRelated(int64_t lib, const std::string& a, const std::string& b)
{
    if (a == b)
        return;
    BatchGuard batch(*this);
    updateItem(lib, a, [&](Json& d) { editRelation(d, b, true); });
    updateItem(lib, b, [&](Json& d) { editRelation(d, a, true); });
}

void Library::removeRelated(int64_t lib, const std::string& a, const std::string& b)
{
    BatchGuard batch(*this);
    updateItem(lib, a, [&](Json& d) { editRelation(d, b, false); });
    updateItem(lib, b, [&](Json& d) { editRelation(d, a, false); });
}

static std::string titleSignature(const std::string& title)
{
    std::string out;
    for (char c : normalize(title))
        if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80)
            out += c;
    return out;
}

std::vector<std::vector<std::string>> Library::duplicateGroups(int64_t lib)
{
    struct Entry {
        std::string key, sig, doi, year;
    };
    std::vector<Entry> entries;
    auto st = db_.query(R"SQL(SELECT key, title, year, data FROM items
                              WHERE library_id=? AND parent_key IS NULL AND trashed=0
                                AND item_type NOT IN ('note','attachment','annotation'))SQL",
                        lib);
    while (st.step()) {
        auto data = Json::parse(st.str(3));
        entries.push_back({st.str(0), titleSignature(st.str(1)), toLower(trim(jstr(data, "DOI"))), st.str(2)});
    }
    std::vector<size_t> parent(entries.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<size_t(size_t)> find = [&](size_t x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    std::map<std::string, size_t> bySig, byDoi;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if (!e.doi.empty()) {
            auto [it, inserted] = byDoi.emplace(e.doi, i);
            if (!inserted)
                parent[find(i)] = find(it->second);
        }
        if (e.sig.size() >= 10) {
            auto [it, inserted] = bySig.emplace(e.sig, i);
            if (!inserted) {
                auto& o = entries[it->second];
                bool doiClash = !e.doi.empty() && !o.doi.empty() && e.doi != o.doi;
                bool yearClash = !e.year.empty() && !o.year.empty() && std::abs(std::stoi(e.year) - std::stoi(o.year)) > 1;
                if (!doiClash && !yearClash)
                    parent[find(i)] = find(it->second);
            }
        }
    }
    std::map<size_t, std::vector<std::string>> groups;
    for (size_t i = 0; i < entries.size(); ++i)
        groups[find(i)].push_back(entries[i].key);
    std::vector<std::vector<std::string>> out;
    for (auto& [root, keys] : groups)
        if (keys.size() > 1)
            out.push_back(keys);
    return out;
}

void Library::mergeItems(int64_t lib, const std::string& master, const std::vector<std::string>& others)
{
    auto m = item(lib, master);
    if (!m)
        throw Error("item not found: " + master);
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    for (auto& key : others) {
        if (key == master)
            continue;
        auto o = item(lib, key);
        if (!o)
            continue;
        for (auto& [k, v] : o->items())
            if (!kMetaKeys.count(k) && v.is_string() && !v.get<std::string>().empty() &&
                jstr(*m, k.c_str()).empty() && schema::hasField(jstr(*m, "itemType"), k))
                (*m)[k] = v;
        for (auto& t : o->value("tags", Json::array())) {
            bool has = false;
            for (auto& mt : (*m)["tags"])
                has = has || jstr(mt, "tag") == jstr(t, "tag");
            if (!has)
                (*m)["tags"].push_back(t);
        }
        for (auto& c : o->value("collections", Json::array())) {
            auto& mc = (*m)["collections"];
            if (std::find(mc.begin(), mc.end(), c) == mc.end())
                mc.push_back(c);
        }
        auto& rel = (*m)["relations"];
        Json replaces = rel.value("dc:replaces", Json::array());
        replaces.push_back(key);
        rel["dc:replaces"] = replaces;
        for (auto& child : children(lib, key)) {
            child["parentItem"] = master;
            saveItem(lib, child);
        }
        for (auto& r : relatedKeys(lib, key))
            if (r != master)
                editRelation(*m, r, true);
        (*o)["deleted"] = true;
        saveItem(lib, *o);
    }
    saveItem(lib, *m);
    tx.commit();
}

// --- notes & attachments --------------------------------------------------------------------

Json Library::createNote(int64_t lib, const std::string& html, const std::string& parentKey)
{
    Json d{{"itemType", "note"}, {"note", html}};
    if (!parentKey.empty())
        d["parentItem"] = parentKey;
    return createItem(lib, d);
}

static int64_t fileMtimeMs(const fs::path& p)
{
    auto ft = fs::last_write_time(p);
    auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sys.time_since_epoch().count();
}

Json Library::importAttachment(int64_t lib, const fs::path& src, const std::string& parentKey, bool link,
                               const std::string& title)
{
    if (!fs::is_regular_file(src))
        throw Error("not a file: " + pathString(src));
    Json d{{"itemType", "attachment"}};
    std::string key = newKey();
    d["key"] = key;
    if (!parentKey.empty())
        d["parentItem"] = parentKey;
    auto contentType = guessContentType(src);
    auto filename = safeFilename(pathString(src.filename()));
    std::string t = title;
    if (t.empty()) {
        if (!parentKey.empty())
            t = contentType == "application/pdf" ? "PDF" : contentType == "application/epub+zip" ? "EPUB" : filename;
        else
            t = pathString(src.stem());
    }
    d["title"] = t;
    d["contentType"] = contentType;
    if (link) {
        d["linkMode"] = "linked_file";
        d["path"] = pathString(fs::absolute(src));
        d["filename"] = filename;
    } else {
        d["linkMode"] = "imported_file";
        d["filename"] = filename;
        auto dir = storageDir() / key;
        fs::create_directories(dir);
        fs::copy_file(src, dir / u8path(filename), fs::copy_options::overwrite_existing);
        d["md5"] = md5File(dir / u8path(filename));
        d["mtime"] = fileMtimeMs(dir / u8path(filename));
    }
    return createItem(lib, d);
}

std::vector<std::string> Library::copyItems(int64_t fromLib, const std::vector<std::string>& keys, int64_t toLib,
                                            const std::string& collectionKey)
{
    // Every item to copy, parents before their children, each with its new key first:
    // a note's images, an annotation's attachment are referred to by key.
    std::vector<Json> copies;
    std::map<std::string, std::string> newKeys;
    std::set<std::string> asked(keys.begin(), keys.end());
    std::function<void(const Json&)> collect = [&](const Json& d) {
        auto key = jstr(d, "key");
        if (newKeys.count(key) || jbool(d, "deleted"))
            return;
        newKeys[key] = newKey();
        copies.push_back(d);
        for (auto& child : children(fromLib, key))
            collect(child);
    };
    std::vector<std::string> tops;  // asked for, not under another one asked for
    for (auto& key : keys) {
        auto d = item(fromLib, key);
        if (!d)
            continue;
        bool under = false;
        for (auto parent = jstr(*d, "parentItem"); !parent.empty() && !under;) {
            under = asked.count(parent) > 0;
            auto p = item(fromLib, parent);
            parent = p ? jstr(*p, "parentItem") : std::string();
        }
        if (!under) {
            tops.push_back(key);
            collect(*d);
        }
    }

    BatchGuard batch(*this);
    std::vector<std::string> out;
    for (auto d : copies) {
        auto oldKey = jstr(d, "key");
        d["key"] = newKeys[oldKey];
        auto parent = newKeys.find(jstr(d, "parentItem"));
        if (parent != newKeys.end())
            d["parentItem"] = parent->second;
        else
            d.erase("parentItem");  // copied without it
        d.erase("dateAdded");
        d.erase("dateModified");
        d.erase("relations");
        d.erase("deleted");
        d["collections"] = !d.contains("parentItem") && !collectionKey.empty() ? Json::array({collectionKey})
                                                                               : Json::array();
        if (d.contains("note") && d["note"].is_string()) {
            auto html = d["note"].get<std::string>();
            for (auto& [from, to] : newKeys)
                html = replaceAll(html, "data-attachment-key=\"" + from + "\"", "data-attachment-key=\"" + to + "\"");
            d["note"] = html;
        }
        if (schema::isStoredLinkMode(jstr(d, "linkMode"))) {  // its own copy of the file
            Json old = d;
            old["key"] = oldKey;
            auto src = attachmentFile(fromLib, old), dest = attachmentFile(toLib, d);
            std::error_code ec;
            if (!src.empty() && !dest.empty() && fs::is_regular_file(src, ec)) {
                fs::create_directories(dest.parent_path());
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
            }
        }
        createItem(toLib, d);
    }
    for (auto& key : tops)
        out.push_back(newKeys[key]);
    return out;
}

Json Library::createLinkedUrl(int64_t lib, const std::string& url, const std::string& title,
                              const std::string& parentKey)
{
    Json d{{"itemType", "attachment"}, {"linkMode", "linked_url"}, {"title", title.empty() ? url : title},
           {"url", url},              {"accessDate", nowIso()}, {"contentType", ""}};
    if (!parentKey.empty())
        d["parentItem"] = parentKey;
    return createItem(lib, d);
}

Json Library::diskUsage()
{
    Json libs = Json::array();
    for (auto& l : libraries()) {
        int64_t items = 0, notes = 0, attachments = 0, annotations = 0, trashed = 0, files = 0, bytes = 0,
                missing = 0;
        Json missingNames = Json::array();  // the first few
        auto st = db_.query("SELECT item_type, link_mode, trashed, data FROM items WHERE library_id = ?", l.id);
        while (st.step()) {
            auto type = st.str(0);
            bool inTrash = st.i64(2) != 0;
            if (type == "attachment" && schema::isStoredLinkMode(st.str(1))) {
                auto att = Json::parse(st.str(3));
                std::error_code ec;
                auto size = fs::file_size(attachmentFile(l.id, att), ec);
                if (!ec) {
                    ++files, bytes += (int64_t)size;
                } else if (++missing <= 20) {
                    missingNames.push_back(jstr(att, "filename", jstr(att, "title")));
                }
            }
            if (inTrash)
                ++trashed;
            else
                ++(type == "note" ? notes : type == "attachment" ? attachments : type == "annotation" ? annotations : items);
        }
        libs.push_back(Json{{"id", l.id},
                            {"name", l.name},
                            {"items", items},
                            {"notes", notes},
                            {"attachments", attachments},
                            {"annotations", annotations},
                            {"trashed", trashed},
                            {"files", files},
                            {"fileBytes", bytes},
                            {"missingFiles", missing},
                            {"missing", missingNames}});
    }
    int64_t storageFiles = 0, storageBytes = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(storageDir(), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) {
            ++storageFiles;
            storageBytes += fileSizeOr0(it->path());
        }
    auto sizeOf = [&](const char* db) {
        int64_t n = 0;
        for (auto suffix : {"", "-wal", "-shm", "-journal"})
            n += fileSizeOr0(dataDir_ / (std::string(db) + suffix));
        return n;
    };
    return Json{{"dataDir", pathString(dataDir_)},
                {"database", sizeOf("ournotes.sqlite")},
                {"fulltextIndex", sizeOf("fulltext.sqlite")},
                {"storage", Json{{"files", storageFiles}, {"bytes", storageBytes}}},
                {"disk", diskSpace(dataDir_)},
                {"libraries", libs}};
}

fs::path Library::attachmentFile(int64_t lib, const Json& att)
{
    auto mode = jstr(att, "linkMode");
    if (schema::isStoredLinkMode(mode)) {
        // Item data may come from other clients: never let it address files outside storage/<KEY>/.
        auto filename = jstr(att, "filename");
        auto key = jstr(att, "key");
        if (filename.empty() || !isValidKey(key))
            return {};
        return storageDir() / key / u8path(safeFilename(filename));
    }
    if (mode == "linked_file") {
        auto path = jstr(att, "path");
        return path.empty() ? fs::path() : u8path(path);
    }
    return {};
}

bool Library::attachmentFileExists(int64_t lib, const Json& att)
{
    auto p = attachmentFile(lib, att);
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

std::string Library::fileMd5(int64_t lib, const std::string& key, const fs::path& path)
{
    std::error_code ec;
    auto size = (int64_t)fs::file_size(path, ec);
    if (ec)
        return {};
    auto mtimeNs = (int64_t)fs::last_write_time(path, ec).time_since_epoch().count();
    auto st = db_.query("SELECT size, mtime_ns, md5 FROM file_state WHERE library_id=? AND key=?", lib, key);
    if (st.step() && st.i64(0) == size && st.i64(1) == mtimeNs && !st.str(2).empty())
        return st.str(2);
    auto md5 = md5File(path);
    db_.run(R"SQL(INSERT INTO file_state(library_id, key, size, mtime_ns, md5) VALUES(?,?,?,?,?)
                  ON CONFLICT(library_id, key) DO UPDATE SET size=excluded.size, mtime_ns=excluded.mtime_ns,
                  md5=excluded.md5)SQL",
            lib, key, size, mtimeNs, md5);
    return md5;
}

bool Library::refreshAttachmentFile(int64_t lib, const std::string& key)
{
    auto att = item(lib, key);
    if (!att || !schema::isStoredLinkMode(jstr(*att, "linkMode")))
        return false;
    auto path = attachmentFile(lib, *att);
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec))
        return false;
    auto md5 = fileMd5(lib, key, path);
    if (md5 == jstr(*att, "md5"))
        return false;
    (*att)["md5"] = md5;
    (*att)["mtime"] = fileMtimeMs(path);
    saveItem(lib, *att);
    return true;
}

// --- collections ------------------------------------------------------------------------

std::vector<Json> Library::collections(int64_t lib)
{
    std::vector<Json> out;
    auto st = db_.query("SELECT data FROM collections WHERE library_id=? ORDER BY name COLLATE NOCASE", lib);
    while (st.step())
        out.push_back(Json::parse(st.str(0)));
    return out;
}

std::optional<Json> Library::collection(int64_t lib, const std::string& key)
{
    if (auto s = db_.scalarStr("SELECT data FROM collections WHERE library_id=? AND key=?", lib, key))
        return Json::parse(*s);
    return std::nullopt;
}

Json Library::createCollection(int64_t lib, const std::string& name, const std::string& parentKey)
{
    Json d{{"key", newKey()}, {"name", name}};
    d["parentCollection"] = parentKey.empty() ? Json(false) : Json(parentKey);
    d["relations"] = Json::object();
    d["dateModified"] = nowIso();
    writeObject(lib, "collection", jstr(d, "key"), d, 0, false, true);
    notify({lib, "collection", "add", {jstr(d, "key")}});
    return d;
}

void Library::saveCollection(int64_t lib, Json data)
{
    auto key = jstr(data, "key");
    auto existing = getObject(lib, "collection", key);
    data["dateModified"] = nowIso();
    writeObject(lib, "collection", key, data, existing ? existing->version : 0, false, true);
    notify({lib, "collection", existing ? "modify" : "add", {key}});
}

std::vector<std::string> Library::descendantCollections(int64_t lib, const std::string& key)
{
    std::vector<std::string> out;
    std::vector<std::string> stack{key};
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();
        auto st = db_.query("SELECT key FROM collections WHERE library_id=? AND parent_key=?", lib, cur);
        while (st.step()) {
            out.push_back(st.str(0));
            stack.push_back(st.str(0));
        }
    }
    return out;
}

void Library::deleteCollection(int64_t lib, const std::string& key, bool trashItems)
{
    auto all = descendantCollections(lib, key);
    all.insert(all.begin(), key);
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    std::set<std::string> affected;
    for (auto& c : all)
        for (auto& k : collectionItemKeys(lib, c))
            affected.insert(k);
    std::set<std::string> removed(all.begin(), all.end());
    for (auto& k : affected) {
        updateItem(lib, k, [&](Json& d) {
            Json kept = Json::array();
            for (auto& c : d.value("collections", Json::array()))
                if (!c.is_string() || !removed.count(c.get<std::string>()))
                    kept.push_back(c);
            d["collections"] = kept;
            if (trashItems)
                d["deleted"] = true;
        });
    }
    for (auto& c : all)
        removeObject(lib, "collection", c, true);
    tx.commit();
    notify({lib, "collection", "delete", all});
}

void Library::addToCollection(int64_t lib, const std::string& collectionKey, const std::vector<std::string>& itemKeys)
{
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    for (auto& k : itemKeys)
        updateItem(lib, topLevelKey(lib, k), [&](Json& d) {
            auto& c = d["collections"];
            if (!c.is_array())
                c = Json::array();
            if (std::find(c.begin(), c.end(), collectionKey) == c.end())
                c.push_back(collectionKey);
        });
    tx.commit();
}

void Library::removeFromCollection(int64_t lib, const std::string& collectionKey,
                                   const std::vector<std::string>& itemKeys)
{
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    for (auto& k : itemKeys)
        updateItem(lib, k, [&](Json& d) {
            Json kept = Json::array();
            for (auto& c : d.value("collections", Json::array()))
                if (c != collectionKey)
                    kept.push_back(c);
            d["collections"] = kept;
        });
    tx.commit();
}

std::set<std::string> Library::collectionItemKeys(int64_t lib, const std::string& collectionKey, bool recursive)
{
    std::vector<std::string> colls{collectionKey};
    if (recursive) {
        auto d = descendantCollections(lib, collectionKey);
        colls.insert(colls.end(), d.begin(), d.end());
    }
    std::set<std::string> out;
    auto st = db_.prepare("SELECT item_key FROM item_collections WHERE library_id=? AND collection_key=?");
    for (auto& c : colls) {
        st.reset();
        st.bindAll(lib, c);
        while (st.step())
            out.insert(st.str(0));
    }
    return out;
}

std::set<std::string> Library::unfiledItemKeys(int64_t lib)
{
    std::set<std::string> out;
    auto st = db_.query(R"SQL(SELECT key FROM items i WHERE library_id=?1 AND parent_key IS NULL
                                AND item_type != 'annotation'
                                AND NOT EXISTS (SELECT 1 FROM item_collections c
                                                WHERE c.library_id=?1 AND c.item_key=i.key))SQL",
                        lib);
    while (st.step())
        out.insert(st.str(0));
    return out;
}

// --- tags ---------------------------------------------------------------------------------

std::vector<TagInfo> Library::tags(int64_t lib)
{
    std::vector<TagInfo> out;
    auto colors = setting(lib, "tagColors", Json::array());
    std::map<std::string, std::pair<std::string, int>> colorMap;
    int pos = 0;
    for (auto& c : colors)
        colorMap[jstr(c, "name")] = {jstr(c, "color"), pos++};
    auto st = db_.query(R"SQL(SELECT t.tag, MIN(t.type), COUNT(*) FROM item_tags t
                              JOIN items i ON i.library_id=t.library_id AND i.key=t.item_key
                              WHERE t.library_id=? AND i.trashed=0
                              GROUP BY t.tag ORDER BY t.tag COLLATE NOCASE)SQL",
                        lib);
    std::set<std::string> seen;
    while (st.step()) {
        TagInfo t;
        t.name = st.str(0);
        t.type = st.i32(1);
        t.count = st.i32(2);
        if (auto it = colorMap.find(t.name); it != colorMap.end()) {
            t.color = it->second.first;
            t.position = it->second.second;
        }
        seen.insert(t.name);
        out.push_back(std::move(t));
    }
    // coloured tags are listed even when no item uses them (like Zotero)
    for (auto& [name, cp] : colorMap)
        if (!seen.count(name))
            out.push_back({name, 0, 0, cp.first, cp.second});
    return out;
}

std::set<std::string> Library::itemKeysWithTags(int64_t lib, const std::vector<std::string>& tags)
{
    std::set<std::string> out;
    if (tags.empty())
        return out;
    std::string placeholders;
    for (size_t i = 0; i < tags.size(); ++i)
        placeholders += i ? ",?" : "?";
    auto st = db_.prepare("SELECT item_key FROM item_tags WHERE library_id=? AND tag IN (" + placeholders +
                          ") GROUP BY item_key HAVING COUNT(DISTINCT tag)=?");
    int idx = 1;
    st.bind(idx++, lib);
    for (auto& t : tags)
        st.bind(idx++, t);
    st.bind(idx, (int64_t)tags.size());
    while (st.step())
        out.insert(st.str(0));
    return out;
}

void Library::addTag(int64_t lib, const std::vector<std::string>& keys, const std::string& tag, int type)
{
    auto name = trim(tag);
    if (name.empty())
        return;
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    for (auto& k : keys)
        updateItem(lib, k, [&](Json& d) {
            for (auto& t : d["tags"])
                if (jstr(t, "tag") == name)
                    return;
            Json t{{"tag", name}};
            if (type)
                t["type"] = type;
            d["tags"].push_back(t);
        });
    tx.commit();
}

void Library::removeTag(int64_t lib, const std::vector<std::string>& keys, const std::string& tag)
{
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    for (auto& k : keys)
        updateItem(lib, k, [&](Json& d) {
            Json kept = Json::array();
            for (auto& t : d.value("tags", Json::array()))
                if (jstr(t, "tag") != tag)
                    kept.push_back(t);
            d["tags"] = kept;
        });
    tx.commit();
}

void Library::renameTag(int64_t lib, const std::string& from, const std::string& to)
{
    auto target = trim(to);
    if (target.empty() || target == from)
        return;
    std::vector<std::string> keys;
    {
        auto st = db_.query("SELECT item_key FROM item_tags WHERE library_id=? AND tag=?", lib, from);
        while (st.step())
            keys.push_back(st.str(0));
    }
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    for (auto& k : keys)
        updateItem(lib, k, [&](Json& d) {
            Json out = Json::array();
            bool hasTarget = false;
            for (auto& t : d["tags"])
                hasTarget = hasTarget || jstr(t, "tag") == target;
            for (auto t : d["tags"]) {
                if (jstr(t, "tag") == from) {
                    if (hasTarget)
                        continue;
                    t["tag"] = target;
                    t.erase("type");
                }
                out.push_back(t);
            }
            d["tags"] = out;
        });
    auto colors = setting(lib, "tagColors", Json::array());
    for (auto& c : colors)
        if (jstr(c, "name") == from)
            c["name"] = target;
    saveTagColors(lib, colors);
    tx.commit();
}

void Library::deleteTag(int64_t lib, const std::string& tag)
{
    std::vector<std::string> keys;
    {
        auto st = db_.query("SELECT item_key FROM item_tags WHERE library_id=? AND tag=?", lib, tag);
        while (st.step())
            keys.push_back(st.str(0));
    }
    BatchGuard batch(*this);
    sql::Transaction tx(db_);
    removeTag(lib, keys, tag);
    setTagColor(lib, tag, "");
    tx.commit();
}

std::map<std::string, std::string> Library::tagColors(int64_t lib)
{
    std::map<std::string, std::string> out;
    for (auto& c : setting(lib, "tagColors", Json::array()))
        out[jstr(c, "name")] = jstr(c, "color");
    return out;
}

void Library::saveTagColors(int64_t lib, const Json& colors)
{
    setSetting(lib, "tagColors", colors.empty() ? Json() : colors);
}

void Library::setTagColor(int64_t lib, const std::string& tag, const std::string& color, int position)
{
    auto colors = setting(lib, "tagColors", Json::array());
    Json out = Json::array();
    bool changed = false;
    for (auto& c : colors) {
        if (jstr(c, "name") == tag)
            changed = true;
        else
            out.push_back(c);
    }
    if (!color.empty()) {
        Json entry{{"name", tag}, {"color", color}};
        if (position < 0 || position >= (int)out.size())
            out.push_back(entry);
        else
            out.insert(out.begin() + position, entry);
        changed = true;
    }
    if (changed)
        saveTagColors(lib, out);
}

// --- settings ---------------------------------------------------------------------------------

Json Library::setting(int64_t lib, const std::string& name, const Json& def)
{
    if (auto s = db_.scalarStr("SELECT data FROM settings WHERE library_id=? AND key=?", lib, name)) {
        auto d = Json::parse(*s);
        if (d.contains("value"))
            return d["value"];
    }
    return def;
}

void Library::setSetting(int64_t lib, const std::string& name, const Json& value)
{
    if (value.is_null()) {
        if (getObject(lib, "setting", name)) {
            removeObject(lib, "setting", name, true);
            notify({lib, "setting", "delete", {name}});
        }
        return;
    }
    auto existing = getObject(lib, "setting", name);
    if (existing && existing->data.value("value", Json()) == value)
        return;
    Json d{{"value", value}, {"dateModified", nowIso()}};
    writeObject(lib, "setting", name, d, existing ? existing->version : 0, false, true);
    notify({lib, "setting", existing ? "modify" : "add", {name}});
}

// --- reader state -----------------------------------------------------------------------------

Json Library::readerState(const std::string& key) const
{
    std::error_code ec;
    auto path = storageDir() / key / ".zotero-reader-state";
    if (!isValidKey(key) || !fs::is_regular_file(path, ec))
        return Json::object();
    try {
        auto j = Json::parse(readFile(path));
        if (j.is_object())
            return j;
    } catch (const std::exception&) {
    }
    return Json::object();
}

void Library::setReaderState(const std::string& key, const Json& state)
{
    if (!isValidKey(key))
        return;
    try {
        fs::create_directories(storageDir() / key);
        writeFileAtomic(storageDir() / key / ".zotero-reader-state", state.dump());
    } catch (const std::exception&) {
        // losing a reading position is not worth an error
    }
}

// --- search -----------------------------------------------------------------------------------

std::set<std::string> Library::search(int64_t lib, const std::string& query, SearchMode mode, bool caseSensitive)
{
    std::set<std::string> out;
    auto tokens = searchTokens(query);
    if (tokens.empty())
        return out;
    std::string column = mode == SearchMode::TitleCreatorYear ? "quick_text" : "search_text";
    std::string sql = "SELECT key, data FROM items WHERE library_id=?";
    for (size_t i = 0; i < tokens.size(); ++i)
        sql += " AND " + column + " LIKE ? ESCAPE '\\'";
    auto st = db_.prepare(sql);
    st.bind(1, lib);
    for (size_t i = 0; i < tokens.size(); ++i)
        st.bind((int)i + 2, likePattern(tokens[i]));
    // Minding case: the stored text is folded, so of what matches it only those whose own
    // text has every word as typed.
    auto exact = searchTokens(query, false);
    auto hasAll = [&](const std::string& text) {
        return std::all_of(exact.begin(), exact.end(), [&](auto& t) { return text.find(t) != std::string::npos; });
    };
    while (st.step()) {
        if (caseSensitive) {
            auto d = derive(Json::parse(st.str(1)));
            if (!hasAll(mode == SearchMode::TitleCreatorYear ? d.quickRaw : d.searchRaw))
                continue;
        }
        out.insert(st.str(0));
    }
    if (mode == SearchMode::Everything)
        for (auto& k : fulltext().search(lib, tokens))
            if (!caseSensitive || hasAll(fulltext().text(lib, k)))
                out.insert(k);
    return out;
}

// --- sync support -------------------------------------------------------------------------------

std::optional<StoredObject> Library::getObject(int64_t lib, const std::string& type, const std::string& key)
{
    auto st = db_.query("SELECT data, version, synced, rev FROM " + tableFor(type) + " WHERE library_id=? AND key=?",
                        lib, key);
    if (!st.step())
        return std::nullopt;
    StoredObject o;
    o.data = Json::parse(st.str(0));
    o.version = st.i64(1);
    o.synced = st.i64(2) != 0;
    o.rev = st.i64(3);
    return o;
}

void Library::putRemoteObject(int64_t lib, const std::string& type, const std::string& key, const Json& data,
                              int64_t version)
{
    bool existed = getObject(lib, type, key).has_value();
    sql::Transaction tx(db_);
    writeObject(lib, type, key, data, version, true, false);
    db_.run("DELETE FROM tombstones WHERE library_id=? AND type=? AND key=?", lib, type, key);
    tx.commit();
    notify({lib, type, existed ? "modify" : "add", {key}});
}

void Library::deleteRemoteObject(int64_t lib, const std::string& type, const std::string& key)
{
    if (type == "item") {
        auto st = db_.query("SELECT item_type, link_mode FROM items WHERE library_id=? AND key=?", lib, key);
        if (st.step() && st.str(0) == "attachment" && schema::isStoredLinkMode(st.str(1))) {
            removeStorage(key);
            fulltext().remove(lib, key);
        }
    }
    removeObject(lib, type, key, false);
    db_.run("DELETE FROM tombstones WHERE library_id=? AND type=? AND key=?", lib, type, key);
    notify({lib, type, "delete", {key}});
}

void Library::markPushed(int64_t lib, const std::string& type, const std::string& key, int64_t version, int64_t rev)
{
    db_.run("UPDATE " + tableFor(type) +
                " SET version=?, synced=CASE WHEN rev=? THEN 1 ELSE 0 END WHERE library_id=? AND key=?",
            version, rev, lib, key);
}

void Library::setObjectVersion(int64_t lib, const std::string& type, const std::string& key, int64_t version)
{
    db_.run("UPDATE " + tableFor(type) + " SET version=? WHERE library_id=? AND key=?", version, lib, key);
}

std::vector<DirtyObject> Library::dirtyObjects(int64_t lib)
{
    std::vector<DirtyObject> out;
    // collections first so items never reference unknown collections on the server
    for (const char* type : {"collection", "search", "setting", "item"}) {
        auto st = db_.query("SELECT key, data, version, rev FROM " + tableFor(type) +
                                " WHERE library_id=? AND synced=0 ORDER BY key",
                            lib);
        while (st.step())
            out.push_back({type, st.str(0), Json::parse(st.str(1)), st.i64(2), st.i64(3)});
    }
    return out;
}

std::vector<Tombstone> Library::tombstones(int64_t lib)
{
    std::vector<Tombstone> out;
    auto st = db_.query("SELECT type, key, version FROM tombstones WHERE library_id=?", lib);
    while (st.step())
        out.push_back({st.str(0), st.str(1), st.i64(2)});
    return out;
}

std::optional<Tombstone> Library::tombstone(int64_t lib, const std::string& type, const std::string& key)
{
    auto st = db_.query("SELECT type, key, version FROM tombstones WHERE library_id=? AND type=? AND key=?", lib, type,
                        key);
    if (st.step())
        return Tombstone{st.str(0), st.str(1), st.i64(2)};
    return std::nullopt;
}

void Library::clearTombstone(int64_t lib, const std::string& type, const std::string& key)
{
    db_.run("DELETE FROM tombstones WHERE library_id=? AND type=? AND key=?", lib, type, key);
}

void Library::resetSyncState(int64_t lib, const std::string& target)
{
    sql::Transaction tx(db_);
    for (const char* table : {"items", "collections", "settings", "searches"})
        db_.run(std::string("UPDATE ") + table + " SET version=0, synced=0 WHERE library_id=?", lib);
    db_.run("DELETE FROM tombstones WHERE library_id=?", lib);
    db_.run("UPDATE file_state SET synced_md5='' WHERE library_id=?", lib);
    db_.run("UPDATE libraries SET sync_target=?, sync_version=0 WHERE id=?", target, lib);
    tx.commit();
}

void Library::recordConflict(int64_t lib, const std::string& type, const std::string& key, const Json& local,
                             const Json& remote, const std::string& kept)
{
    db_.run("INSERT INTO conflicts(library_id, type, key, kept, local_data, remote_data, created) "
            "VALUES(?,?,?,?,?,?,?)",
            lib, type, key, kept, local.dump(), remote.dump(), nowIso());
}

std::vector<Conflict> Library::conflicts(int64_t lib)
{
    std::vector<Conflict> out;
    auto st = db_.query("SELECT id, library_id, type, key, kept, local_data, remote_data, created FROM conflicts "
                        "WHERE ?1=0 OR library_id=?1 ORDER BY id DESC",
                        lib);
    while (st.step()) {
        Conflict c;
        c.id = st.i64(0);
        c.libraryId = st.i64(1);
        c.type = st.str(2);
        c.key = st.str(3);
        c.kept = st.str(4);
        c.local = Json::parse(st.isNull(5) ? "null" : st.str(5));
        c.remote = Json::parse(st.isNull(6) ? "null" : st.str(6));
        c.created = st.str(7);
        out.push_back(std::move(c));
    }
    return out;
}

void Library::deleteConflict(int64_t id) { db_.run("DELETE FROM conflicts WHERE id=?", id); }

std::optional<FileState> Library::fileState(int64_t lib, const std::string& key)
{
    auto st = db_.query("SELECT size, mtime_ns, md5, synced_md5 FROM file_state WHERE library_id=? AND key=?", lib,
                        key);
    if (!st.step())
        return std::nullopt;
    return FileState{st.i64(0), st.i64(1), st.str(2), st.str(3)};
}

void Library::setSyncedMd5(int64_t lib, const std::string& key, const std::string& md5)
{
    db_.run(R"SQL(INSERT INTO file_state(library_id, key, synced_md5) VALUES(?,?,?)
                  ON CONFLICT(library_id, key) DO UPDATE SET synced_md5=excluded.synced_md5)SQL",
            lib, key, md5);
}

} // namespace on
