#include "sync/store.h"

#include <algorithm>
#include <chrono>

#include "core/hash.h"

namespace on::sync {

namespace {

constexpr int kSchemaVersion = 1;
constexpr int64_t kTokenTouchSeconds = 60;  // throttles last_used updates
constexpr size_t kMaxNameBytes = 255;
constexpr size_t kMaxDeviceBytes = 200;

const char* const kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    username   TEXT PRIMARY KEY,
    pw_salt    TEXT NOT NULL,
    pw_hash    TEXT NOT NULL,
    iterations INTEGER NOT NULL,
    admin      INTEGER NOT NULL DEFAULT 0,
    created    TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS tokens (
    token_hash TEXT PRIMARY KEY,
    username   TEXT NOT NULL,
    device     TEXT NOT NULL DEFAULT '',
    created    TEXT NOT NULL,
    last_used  TEXT
);
CREATE INDEX IF NOT EXISTS tokens_username ON tokens(username);
CREATE TABLE IF NOT EXISTS libraries (
    uid     TEXT PRIMARY KEY,
    name    TEXT NOT NULL,
    type    TEXT NOT NULL,
    owner   TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 0,
    created TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS libraries_owner ON libraries(owner);
CREATE TABLE IF NOT EXISTS members (
    uid      TEXT NOT NULL,
    username TEXT NOT NULL,
    role     TEXT NOT NULL,
    PRIMARY KEY (uid, username)
);
CREATE INDEX IF NOT EXISTS members_username ON members(username);
CREATE TABLE IF NOT EXISTS objects (
    uid     TEXT NOT NULL,
    type    TEXT NOT NULL,
    key     TEXT NOT NULL,
    version INTEGER NOT NULL,
    deleted INTEGER NOT NULL DEFAULT 0,
    data    TEXT,
    PRIMARY KEY (uid, type, key)
);
CREATE INDEX IF NOT EXISTS objects_version ON objects(uid, version, type, key);
CREATE TABLE IF NOT EXISTS files (
    uid          TEXT NOT NULL,
    key          TEXT NOT NULL,
    md5          TEXT NOT NULL,
    size         INTEGER NOT NULL,
    mtime        INTEGER NOT NULL,
    filename     TEXT NOT NULL DEFAULT '',
    content_type TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (uid, key)
);
)SQL";

const char* const kLibraryColumns = "l.uid, l.name, l.type, l.owner, l.version";

// Plain BEGIN/COMMIT. Write transactions use BEGIN IMMEDIATE so that a
// read-then-write sequence cannot fail with SQLITE_BUSY_SNAPSHOT when another
// process (the admin CLI, a second client of a shared folder) writes meanwhile.
class Txn {
public:
    Txn(sql::Db& db, bool write) : db_(db) { db_.exec(write ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Txn()
    {
        if (!done_) {
            try {
                db_.exec("ROLLBACK");
            } catch (...) {
            }
        }
    }
    void commit()
    {
        db_.exec("COMMIT");
        done_ = true;
    }

private:
    sql::Db& db_;
    bool done_ = false;
};

bool isWindowsDeviceName(const std::string& name)
{
    if (name.size() == 3)
        return name == "CON" || name == "PRN" || name == "AUX" || name == "NUL";
    return name.size() == 4 && (startsWith(name, "COM") || startsWith(name, "LPT")) && name[3] >= '0' &&
           name[3] <= '9';
}

// Keys never contain '.', so prefixing lowercase letters with it keeps distinct
// keys distinct on case-insensitive filesystems ("aB" -> ".aB", "AB" -> "AB").
// Windows device names get a '.' prefix too ("NUL" -> ".NUL").
std::string fsName(const std::string& key)
{
    std::string out;
    out.reserve(key.size() + 8);
    for (char c : key) {
        if (c >= 'a' && c <= 'z')
            out += '.';
        out += c;
    }
    return isWindowsDeviceName(out) ? "." + out : out;
}

void requireKey(const std::string& key, const char* what)
{
    if (!isValidKey(key))
        throw SyncError(std::string("invalid ") + what + " '" + key.substr(0, 80) + "'", 400);
}

void validatePassword(const std::string& password)
{
    if (password.empty())
        throw SyncError("password must not be empty", 400);
    if (password.size() > 1024)
        throw SyncError("password is too long", 400);
}

std::string checkedName(const std::string& name)
{
    auto n = trim(name);
    if (n.size() > kMaxNameBytes)
        throw SyncError("library name is too long", 400);
    return n;
}

std::string hashPassword(const std::string& password, const std::string& salt, int iterations)
{
    return toHex(pbkdf2Sha256(password, salt, iterations));
}

RemoteLibrary readLibrary(const sql::Stmt& st, std::string role)
{
    RemoteLibrary l;
    l.uid = st.str(0);
    l.name = st.str(1);
    l.type = st.str(2);
    l.owner = st.str(3);
    l.version = st.i64(4);
    l.role = std::move(role);
    return l;
}

FileInfo readFileInfo(const sql::Stmt& st)  // key, md5, size, mtime, filename, content_type
{
    FileInfo f;
    f.key = st.str(0);
    f.md5 = st.str(1);
    f.size = st.i64(2);
    f.mtime = st.i64(3);
    f.filename = st.str(4);
    f.contentType = st.str(5);
    return f;
}

std::string pushError(const PushObject& o)
{
    if (!isObjectType(o.type))
        return "invalid object type '" + o.type.substr(0, 40) + "'";
    if (!isValidKey(o.key))
        return "invalid object key";
    if (o.baseVersion < 0)
        return "invalid baseVersion";
    if (!o.deleted && !o.data.is_object())
        return "data must be a JSON object";
    return {};
}

// Cursor: "<version>.<type>.<key>" of the last object returned. Clients treat it as opaque.
std::string makeCursor(const RemoteObject& o)
{
    return std::to_string(o.version) + "." + o.type + "." + o.key;
}

void parseCursor(const std::string& cursor, int64_t& version, std::string& type, std::string& key)
{
    auto parts = split(cursor, '.', false);
    bool ok = parts.size() == 3 && !parts[0].empty() && parts[0].size() < 19 &&
              std::all_of(parts[0].begin(), parts[0].end(), [](char c) { return c >= '0' && c <= '9'; }) &&
              isObjectType(parts[1]) && isValidKey(parts[2]);
    if (!ok)
        throw SyncError("invalid cursor", 400);
    version = std::stoll(parts[0]);
    type = parts[1];
    key = parts[2];
}

// Looks only at an ASCII extension: the name comes from clients and need not be valid UTF-8.
std::string contentTypeFor(const std::string& filename)
{
    auto dot = filename.rfind('.');
    auto ext = dot == std::string::npos ? std::string() : filename.substr(dot);
    bool ascii = std::all_of(ext.begin(), ext.end(), [](char c) { return (unsigned char)c < 0x80; });
    return ascii && ext.size() > 1 ? guessContentType(fs::path("file" + ext)) : "application/octet-stream";
}

// rename(), falling back to copy + rename when src and dest are on different filesystems.
void moveFile(const fs::path& src, const fs::path& dest)
{
    std::error_code ec;
    fs::rename(src, dest, ec);
    if (!ec)
        return;
    auto part = dest;
    part += ".part";
    fs::copy_file(src, part, fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::rename(part, dest, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(part, ignored);
        throw SyncError("cannot store " + pathString(dest) + ": " + ec.message());
    }
    fs::remove(src, ec);
}

void deleteLibraryRows(sql::Db& db, const std::string& uid)
{
    db.run("DELETE FROM objects WHERE uid = ?", uid);
    db.run("DELETE FROM files WHERE uid = ?", uid);
    db.run("DELETE FROM members WHERE uid = ?", uid);
    db.run("DELETE FROM libraries WHERE uid = ?", uid);
}

} // namespace

bool isValidUsername(const std::string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '-';
    });
}

Store::Store(const fs::path& root, bool wal) : root_(root)
{
    std::error_code ec;
    fs::create_directories(root_ / "files", ec);
    if (!ec)
        fs::create_directories(root_ / "tmp", ec);
    if (ec)
        throw SyncError("cannot create " + pathString(root_) + ": " + ec.message());

    db_.open(root_ / "store.sqlite");
    try {
        // Fails harmlessly when another connection holds the database in the other mode.
        db_.exec(wal ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE");
    } catch (const sql::Error&) {
    }
    // Durable commits: a version handed out to a client must never be reused.
    db_.exec("PRAGMA synchronous=FULL");
    initSchema();
    cleanTempDir();
}

Store::~Store() = default;

void Store::initSchema()
{
    int v = db_.userVersion();
    if (v > kSchemaVersion)
        throw SyncError(pathString(root_) + " was created by a newer version of OurNotes");
    db_.exec(kSchema);
    if (v < kSchemaVersion)
        db_.setUserVersion(kSchemaVersion);
}

// Leftovers of interrupted uploads. Recent ones may belong to a running server.
void Store::cleanTempDir()
{
    std::error_code ec;
    auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (auto it = fs::directory_iterator(root_ / "tmp", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2) && it->last_write_time(ec2) < cutoff)
            fs::remove(it->path(), ec2);
    }
}

// --- users and tokens -------------------------------------------------------------

void Store::requireUser(const std::string& username)
{
    if (!db_.scalarInt("SELECT 1 FROM users WHERE username = ?", username))
        throw SyncError("no such user '" + username + "'", 404);
}

void Store::createUser(const std::string& username, const std::string& password, bool admin)
{
    if (!isValidUsername(username))
        throw SyncError("invalid user name '" + username.substr(0, 80) + "' (use 1-64 of A-Z a-z 0-9 _ . -)", 400);
    validatePassword(password);
    auto salt = randomToken(16);
    auto hash = hashPassword(password, salt, kPasswordIterations);

    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    if (db_.scalarInt("SELECT 1 FROM users WHERE username = ?", username))
        throw SyncError("user '" + username + "' already exists", 409);
    db_.run("INSERT INTO users (username, pw_salt, pw_hash, iterations, admin, created) VALUES (?, ?, ?, ?, ?, ?)",
            username, salt, hash, kPasswordIterations, admin, nowIso());
    txn.commit();
}

void Store::setPassword(const std::string& username, const std::string& password)
{
    validatePassword(password);
    auto salt = randomToken(16);
    auto hash = hashPassword(password, salt, kPasswordIterations);

    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    requireUser(username);
    db_.run("UPDATE users SET pw_salt = ?, pw_hash = ?, iterations = ? WHERE username = ?", salt, hash,
            kPasswordIterations, username);
    db_.run("DELETE FROM tokens WHERE username = ?", username);
    txn.commit();
}

void Store::deleteUser(const std::string& username, bool deleteOwnedLibraries)
{
    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    requireUser(username);
    std::vector<std::string> owned;
    for (auto st = db_.query("SELECT uid FROM libraries WHERE owner = ? ORDER BY uid", username); st.step();)
        owned.push_back(st.str(0));
    if (!owned.empty() && !deleteOwnedLibraries)
        throw SyncError("user '" + username + "' still owns " + (owned.size() == 1 ? "library " : "libraries ") +
                            join(owned, ", "),
                        409);
    for (auto& uid : owned)
        deleteLibraryRows(db_, uid);
    db_.run("DELETE FROM tokens WHERE username = ?", username);
    db_.run("DELETE FROM members WHERE username = ?", username);
    db_.run("DELETE FROM users WHERE username = ?", username);
    txn.commit();

    for (auto& uid : owned) {
        std::error_code ec;
        fs::remove_all(root_ / "files" / fsName(uid), ec);
    }
}

bool Store::userExists(const std::string& username)
{
    std::lock_guard lock(mutex_);
    return db_.scalarInt("SELECT 1 FROM users WHERE username = ?", username).has_value();
}

bool Store::isAdmin(const std::string& username)
{
    std::lock_guard lock(mutex_);
    return db_.scalarInt("SELECT admin FROM users WHERE username = ?", username).value_or(0) != 0;
}

std::vector<Json> Store::users()
{
    std::lock_guard lock(mutex_);
    std::vector<Json> out;
    auto st = db_.query("SELECT u.username, u.admin, u.created,"
                        " (SELECT COUNT(*) FROM tokens t WHERE t.username = u.username)"
                        " FROM users u ORDER BY u.username");
    while (st.step())
        out.push_back(Json{{"username", st.str(0)}, {"admin", st.i64(1) != 0}, {"created", st.str(2)},
                           {"tokens", st.i64(3)}});
    return out;
}

bool Store::checkPassword(const std::string& username, const std::string& password)
{
    std::optional<std::string> salt, hash;
    int iterations = kPasswordIterations;
    {
        std::lock_guard lock(mutex_);
        auto st = db_.query("SELECT pw_salt, pw_hash, iterations FROM users WHERE username = ?", username);
        if (st.step()) {
            salt = st.str(0);
            hash = st.str(1);
            iterations = st.i32(2);
        }
    }
    if (!salt) {
        // Same amount of work as for a real user, so timing does not reveal user names.
        hashPassword(password, "ournotesd-no-such-user", kPasswordIterations);
        return false;
    }
    return constantTimeEquals(hashPassword(password, *salt, iterations), *hash);
}

std::string Store::createToken(const std::string& username, const std::string& device)
{
    auto dev = trim(device);
    if (dev.size() > kMaxDeviceBytes)
        dev.resize(kMaxDeviceBytes);
    auto token = randomToken(32);
    auto now = nowIso();

    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    requireUser(username);
    db_.run("INSERT INTO tokens (token_hash, username, device, created, last_used) VALUES (?, ?, ?, ?, ?)",
            sha256Hex(token), username, dev, now, now);
    txn.commit();
    return token;
}

std::optional<std::string> Store::userForToken(const std::string& token)
{
    if (token.empty() || token.size() > 512)
        return std::nullopt;
    auto hash = sha256Hex(token);

    std::lock_guard lock(mutex_);
    std::string username, lastUsed;
    {
        auto st = db_.query("SELECT username, last_used FROM tokens WHERE token_hash = ?", hash);
        if (!st.step())
            return std::nullopt;
        username = st.str(0);
        lastUsed = st.str(1);
    }
    auto now = nowIso();
    if (isoToEpoch(now) - isoToEpoch(lastUsed) >= kTokenTouchSeconds)
        db_.run("UPDATE tokens SET last_used = ? WHERE token_hash = ?", now, hash);
    return username;
}

void Store::revokeToken(const std::string& token)
{
    std::lock_guard lock(mutex_);
    db_.run("DELETE FROM tokens WHERE token_hash = ?", sha256Hex(token));
}

std::vector<Json> Store::tokens(const std::string& username)
{
    std::lock_guard lock(mutex_);
    std::vector<Json> out;
    auto st = db_.query("SELECT token_hash, device, created, last_used FROM tokens WHERE username = ? ORDER BY created",
                        username);
    while (st.step())
        out.push_back(Json{{"id", st.str(0).substr(0, 12)}, {"device", st.str(1)}, {"created", st.str(2)},
                           {"lastUsed", st.str(3)}});
    return out;
}

// --- libraries --------------------------------------------------------------------------

int64_t Store::requireLibraryVersion(const std::string& uid)
{
    auto v = db_.scalarInt("SELECT version FROM libraries WHERE uid = ?", uid);
    if (!v)
        throw SyncError("library '" + uid.substr(0, 80) + "' not found", 404);
    return *v;
}

std::string Store::roleLocked(const std::string& uid, const std::string& username)
{
    return db_
        .scalarStr("SELECT CASE WHEN l.owner = ?2 THEN 'owner'"
                   " ELSE (SELECT m.role FROM members m WHERE m.uid = ?1 AND m.username = ?2) END"
                   " FROM libraries l WHERE l.uid = ?1",
                   uid, username)
        .value_or("");
}

std::vector<RemoteLibrary> Store::libraries(const std::string& username)
{
    std::lock_guard lock(mutex_);
    std::vector<RemoteLibrary> out;
    auto st = db_.query(std::string("SELECT ") + kLibraryColumns +
                            ", CASE WHEN l.owner = ?1 THEN 'owner' ELSE m.role END"
                            " FROM libraries l LEFT JOIN members m ON m.uid = l.uid AND m.username = ?1"
                            " WHERE l.owner = ?1 OR m.username IS NOT NULL ORDER BY l.name, l.uid",
                        username);
    while (st.step())
        out.push_back(readLibrary(st, st.str(5)));
    return out;
}

std::vector<RemoteLibrary> Store::allLibraries()
{
    std::lock_guard lock(mutex_);
    std::vector<RemoteLibrary> out;
    auto st = db_.query(std::string("SELECT ") + kLibraryColumns + " FROM libraries l ORDER BY l.owner, l.name");
    while (st.step())
        out.push_back(readLibrary(st, ""));
    return out;
}

std::optional<RemoteLibrary> Store::library(const std::string& uid, const std::string& username)
{
    std::lock_guard lock(mutex_);
    auto role = roleLocked(uid, username);
    if (role.empty())
        return std::nullopt;
    auto st = db_.query(std::string("SELECT ") + kLibraryColumns + " FROM libraries l WHERE l.uid = ?", uid);
    if (!st.step())
        return std::nullopt;
    return readLibrary(st, role);
}

RemoteLibrary Store::ensureLibrary(const std::string& uid, const std::string& name, const std::string& type,
                                   const std::string& username)
{
    requireKey(uid, "library id");
    auto libType = type.empty() ? std::string("user") : type;
    if (libType != "user" && libType != "group")
        throw SyncError("invalid library type '" + type.substr(0, 40) + "'", 400);
    auto libName = checkedName(name);

    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    auto st = db_.query(std::string("SELECT ") + kLibraryColumns + " FROM libraries l WHERE l.uid = ?", uid);
    if (!st.step()) {
        requireUser(username);
        RemoteLibrary l{uid, libName.empty() ? uid : libName, libType, "owner", username, 0};
        db_.run("INSERT INTO libraries (uid, name, type, owner, version, created) VALUES (?, ?, ?, ?, 0, ?)", l.uid,
                l.name, l.type, l.owner, nowIso());
        txn.commit();
        return l;
    }
    auto role = roleLocked(uid, username);
    if (role.empty())
        throw SyncError("library '" + uid + "' belongs to another user", 403);
    auto lib = readLibrary(st, role);
    if (role == "owner" && !libName.empty() && libName != lib.name) {
        db_.run("UPDATE libraries SET name = ? WHERE uid = ?", libName, uid);
        lib.name = libName;
    }
    txn.commit();
    return lib;
}

void Store::share(const std::string& uid, const std::string& username, const std::string& role)
{
    if (role != "read" && role != "write")
        throw SyncError("invalid role '" + role.substr(0, 40) + "' (use read or write)", 400);
    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    requireLibraryVersion(uid);
    requireUser(username);
    if (db_.scalarInt("SELECT 1 FROM libraries WHERE uid = ? AND owner = ?", uid, username))
        throw SyncError("'" + username + "' owns library '" + uid + "'", 400);
    db_.run("INSERT INTO members (uid, username, role) VALUES (?, ?, ?)"
            " ON CONFLICT (uid, username) DO UPDATE SET role = excluded.role",
            uid, username, role);
    txn.commit();
}

bool Store::unshare(const std::string& uid, const std::string& username)
{
    std::lock_guard lock(mutex_);
    requireLibraryVersion(uid);
    db_.run("DELETE FROM members WHERE uid = ? AND username = ?", uid, username);
    return db_.changes() > 0;
}

std::vector<Json> Store::members(const std::string& uid)
{
    std::lock_guard lock(mutex_);
    requireLibraryVersion(uid);
    std::vector<Json> out;
    for (auto st = db_.query("SELECT username, role FROM members WHERE uid = ? ORDER BY username", uid); st.step();)
        out.push_back(Json{{"username", st.str(0)}, {"role", st.str(1)}});
    return out;
}

void Store::deleteLibrary(const std::string& uid, bool requireEmpty)
{
    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    requireLibraryVersion(uid);
    if (requireEmpty) {
        auto n = db_.scalarInt("SELECT count(*) FROM objects WHERE uid = ? AND deleted = 0"
                               " AND type IN ('item', 'collection')",
                               uid);
        if (n && *n > 0)
            throw SyncError("the library still has " + std::to_string(*n) + " entries on the server", 409);
    }
    deleteLibraryRows(db_, uid);
    txn.commit();
    if (isValidKey(uid)) {
        std::error_code ec;
        fs::remove_all(root_ / "files" / fsName(uid), ec);
    }
}

void Store::renameLibrary(const std::string& uid, const std::string& name)
{
    auto libName = checkedName(name);
    if (libName.empty())
        throw SyncError("library name must not be empty", 400);
    std::lock_guard lock(mutex_);
    db_.run("UPDATE libraries SET name = ? WHERE uid = ?", libName, uid);
    if (db_.changes() == 0)
        throw SyncError("library '" + uid.substr(0, 80) + "' not found", 404);
}

std::string Store::role(const std::string& uid, const std::string& username)
{
    std::lock_guard lock(mutex_);
    return roleLocked(uid, username);
}

// --- objects ---------------------------------------------------------------------------------

std::optional<RemoteObject> Store::loadObject(const std::string& uid, const std::string& type,
                                              const std::string& key)
{
    auto st = db_.query("SELECT version, deleted, data FROM objects WHERE uid = ? AND type = ? AND key = ?", uid,
                        type, key);
    if (!st.step())
        return std::nullopt;
    RemoteObject o;
    o.type = type;
    o.key = key;
    o.version = st.i64(0);
    o.deleted = st.i64(1) != 0;
    if (!o.deleted)
        o.data = Json::parse(st.str(2));
    return o;
}

ChangesPage Store::changes(const std::string& uid, int64_t since, const std::string& cursor, int limit)
{
    if (limit <= 0)
        limit = kDefaultChangesLimit;
    limit = std::min(limit, kMaxChangesLimit);
    since = std::max<int64_t>(since, 0);
    int64_t afterVersion = 0;
    std::string afterType, afterKey;
    if (!cursor.empty())
        parseCursor(cursor, afterVersion, afterType, afterKey);

    std::lock_guard lock(mutex_);
    Txn txn(db_, false);  // the version and the rows come from one snapshot
    ChangesPage page;
    page.version = requireLibraryVersion(uid);
    // ?2 lets the index range start at the cursor; the row-value test handles ties.
    auto st = db_.query("SELECT type, key, version, deleted, data FROM objects"
                        " WHERE uid = ?1 AND version >= ?2 AND (?3 OR deleted = 0)"
                        " AND (version, type, key) > (?4, ?5, ?6)"
                        " ORDER BY version, type, key LIMIT ?7",
                        uid, std::max(since + 1, afterVersion), since > 0, afterVersion, afterType, afterKey,
                        limit + 1);
    while (st.step()) {
        if ((int)page.objects.size() == limit) {
            page.next = makeCursor(page.objects.back());
            break;
        }
        RemoteObject o;
        o.type = st.str(0);
        o.key = st.str(1);
        o.version = st.i64(2);
        o.deleted = st.i64(3) != 0;
        if (!o.deleted)
            o.data = Json::parse(st.str(4));
        page.objects.push_back(std::move(o));
    }
    txn.commit();
    return page;
}

PushResult Store::push(const std::string& uid, const std::vector<PushObject>& objects)
{
    if ((int)objects.size() > kMaxPushObjects)
        throw SyncError("too many objects in one push (at most " + std::to_string(kMaxPushObjects) + ")", 400);

    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    const int64_t libVersion = requireLibraryVersion(uid);
    const int64_t newVersion = libVersion + 1;
    bool wrote = false;
    std::vector<std::string> removedFiles;
    PushResult result;

    auto upsert = db_.prepare("INSERT INTO objects (uid, type, key, version, deleted, data) VALUES (?, ?, ?, ?, ?, ?)"
                              " ON CONFLICT (uid, type, key) DO UPDATE SET"
                              " version = excluded.version, deleted = excluded.deleted, data = excluded.data");
    for (auto& o : objects) {
        PushEntry e;
        e.type = o.type;
        e.key = o.key;
        e.message = pushError(o);
        if (!e.message.empty()) {
            e.status = "error";
            result.results.push_back(std::move(e));
            continue;
        }
        auto current = loadObject(uid, o.type, o.key);
        if (current && current->version > o.baseVersion) {
            e.status = "conflict";
            e.version = current->version;
            e.current = std::move(current);
        } else if (current && current->deleted && o.deleted) {
            e.status = "ok";  // already deleted: nothing to write
            e.version = current->version;
        } else {
            std::optional<std::string> data;
            if (!o.deleted)
                data = o.data.dump(-1, ' ', false, Json::error_handler_t::replace);
            upsert.reset();
            upsert.bindAll(uid, o.type, o.key, newVersion, o.deleted, data);
            upsert.exec();
            if (o.deleted && o.type == kItem) {
                db_.run("DELETE FROM files WHERE uid = ? AND key = ?", uid, o.key);
                if (db_.changes() > 0)
                    removedFiles.push_back(o.key);
            }
            e.status = "ok";
            e.version = newVersion;
            wrote = true;
        }
        result.results.push_back(std::move(e));
    }
    if (wrote)
        db_.run("UPDATE libraries SET version = ? WHERE uid = ?", newVersion, uid);
    txn.commit();

    for (auto& key : removedFiles)
        removeBlob(uid, key);
    result.version = wrote ? newVersion : libVersion;
    return result;
}

// --- files ---------------------------------------------------------------------------------------

Json Store::usage(const std::string& username)
{
    auto libs = libraries(username);
    std::lock_guard lock(mutex_);
    Json list = Json::array();
    for (auto& l : libs) {
        Json j = toJson(l);
        int64_t items = 0, notes = 0, attachments = 0, annotations = 0;
        auto st = db_.query("SELECT CASE WHEN json_valid(data) THEN json_extract(data, '$.itemType') END, COUNT(*) FROM objects"
                            " WHERE uid = ? AND type = 'item' AND deleted = 0 GROUP BY 1",
                            l.uid);
        while (st.step()) {
            auto type = st.isNull(0) ? std::string() : st.str(0);
            auto n = st.i64(1);
            (type == "note" ? notes : type == "attachment" ? attachments : type == "annotation" ? annotations : items) += n;
        }
        auto f = db_.query("SELECT COUNT(*), COALESCE(SUM(size), 0) FROM files WHERE uid = ?", l.uid);
        f.step();
        j["items"] = items;
        j["notes"] = notes;
        j["attachments"] = attachments;
        j["annotations"] = annotations;
        j["files"] = f.i64(0);
        j["fileBytes"] = f.i64(1);
        list.push_back(j);
    }
    auto all = db_.query("SELECT COUNT(*), COALESCE(SUM(size), 0) FROM files");
    all.step();
    int64_t database = 0;
    for (auto suffix : {"", "-wal", "-shm", "-journal"})
        database += fileSizeOr0(root_ / ("store.sqlite" + std::string(suffix)));
    return Json{{"database", database},
                {"files", Json{{"count", all.i64(0)}, {"bytes", all.i64(1)}}},
                {"disk", diskSpace(root_)},
                {"libraries", list}};
}

fs::path Store::blobPath(const std::string& uid, const std::string& key) const
{
    requireKey(uid, "library id");
    requireKey(key, "file key");
    return root_ / "files" / fsName(uid) / fsName(key);
}

fs::path Store::newTempPath() const
{
    return root_ / "tmp" / (randomToken(12) + ".part");
}

void Store::removeBlob(const std::string& uid, const std::string& key)
{
    std::error_code ec;
    fs::remove(blobPath(uid, key), ec);
}

std::optional<FileInfo> Store::fileLocked(const std::string& uid, const std::string& key)
{
    auto st = db_.query("SELECT key, md5, size, mtime, filename, content_type FROM files WHERE uid = ? AND key = ?",
                        uid, key);
    if (!st.step())
        return std::nullopt;
    return readFileInfo(st);
}

std::vector<FileInfo> Store::files(const std::string& uid)
{
    std::lock_guard lock(mutex_);
    Txn txn(db_, false);
    requireLibraryVersion(uid);
    std::vector<FileInfo> out;
    auto st = db_.query("SELECT key, md5, size, mtime, filename, content_type FROM files WHERE uid = ? ORDER BY key",
                        uid);
    while (st.step())
        out.push_back(readFileInfo(st));
    txn.commit();
    return out;
}

std::optional<FileInfo> Store::file(const std::string& uid, const std::string& key)
{
    std::lock_guard lock(mutex_);
    return fileLocked(uid, key);
}

FileInfo Store::putFile(const std::string& uid, const std::string& key, const fs::path& tmpFile,
                        const FileInfo& meta)
{
    auto dest = blobPath(uid, key);  // validates uid and key
    if (meta.filename.size() > 1024 || meta.contentType.size() > 255)
        throw SyncError("file name or content type is too long", 400);

    std::error_code ec;
    auto size = fs::file_size(tmpFile, ec);
    if (ec)
        throw SyncError("cannot read " + pathString(tmpFile) + ": " + ec.message());
    FileInfo info;
    info.key = key;
    info.md5 = md5File(tmpFile);  // outside the lock: may take a while
    info.size = (int64_t)size;
    if (!meta.md5.empty() && toLower(meta.md5) != info.md5)
        throw SyncError("checksum mismatch: expected " + meta.md5.substr(0, 40) + ", received " + info.md5, 400);
    info.filename = meta.filename;
    info.contentType = meta.contentType.empty() ? contentTypeFor(meta.filename) : meta.contentType;
    info.mtime = meta.mtime > 0 ? meta.mtime : nowMillis();

    std::lock_guard lock(mutex_);
    Txn txn(db_, true);
    requireLibraryVersion(uid);
    auto item = loadObject(uid, kItem, key);
    if (item && item->deleted)
        throw SyncError("item '" + key + "' has been deleted", 409);
    db_.run("INSERT INTO files (uid, key, md5, size, mtime, filename, content_type) VALUES (?, ?, ?, ?, ?, ?, ?)"
            " ON CONFLICT (uid, key) DO UPDATE SET md5 = excluded.md5, size = excluded.size,"
            " mtime = excluded.mtime, filename = excluded.filename, content_type = excluded.content_type",
            uid, key, info.md5, info.size, info.mtime, info.filename, info.contentType);
    fs::create_directories(dest.parent_path(), ec);
    if (ec)
        throw SyncError("cannot create " + pathString(dest.parent_path()) + ": " + ec.message());
    moveFile(tmpFile, dest);
    txn.commit();
    return info;
}

bool Store::deleteFile(const std::string& uid, const std::string& key)
{
    auto path = blobPath(uid, key);
    std::lock_guard lock(mutex_);
    requireLibraryVersion(uid);
    db_.run("DELETE FROM files WHERE uid = ? AND key = ?", uid, key);
    bool existed = db_.changes() > 0;
    std::error_code ec;
    fs::remove(path, ec);
    return existed;
}

} // namespace on::sync
