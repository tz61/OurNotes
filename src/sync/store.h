#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/sqlite.h"
#include "core/sync/protocol.h"

// Persistent storage for the sync protocol: users, tokens, libraries, versioned
// objects and file blobs. Used by the ournotesd daemon and, directly, by
// LocalTransport for folder sync targets.
//
// Layout:  <root>/store.sqlite      metadata
//          <root>/files/<uid>/<key> blobs (lowercase letters escaped as ".x"
//                                   and Windows device names as ".NUL", so
//                                   case-insensitive filesystems are safe)
//          <root>/tmp/              partial uploads
//
// Errors are reported as SyncError with an HTTP-like status (400, 403, 404, 409).
// Access control (roles) is the caller's job; see role().
namespace on::sync {

inline constexpr int kMaxPushObjects = 1000;
inline constexpr int kDefaultChangesLimit = 1000;
inline constexpr int kMaxChangesLimit = 5000;
inline constexpr int kPasswordIterations = 120000;

bool isValidUsername(const std::string& name);

class Store {
public:
    // wal=false selects a rollback journal, which also works on network shares
    // (WAL needs shared memory and does not work over SMB/NFS).
    explicit Store(const fs::path& root, bool wal = true);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    const fs::path& root() const { return root_; }

    // --- users and tokens -------------------------------------------------------
    void createUser(const std::string& username, const std::string& password, bool admin = false);
    // Also revokes all of the user's tokens.
    void setPassword(const std::string& username, const std::string& password);
    // Removes the user's tokens and memberships. Refuses (409) when the user still
    // owns libraries unless deleteOwnedLibraries is set.
    void deleteUser(const std::string& username, bool deleteOwnedLibraries = false);
    bool userExists(const std::string& username);
    bool isAdmin(const std::string& username);
    std::vector<Json> users();  // {username, admin, created, tokens}
    bool checkPassword(const std::string& username, const std::string& password);

    std::string createToken(const std::string& username, const std::string& device);
    std::optional<std::string> userForToken(const std::string& token);
    void revokeToken(const std::string& token);
    std::vector<Json> tokens(const std::string& username);  // {id, device, created, lastUsed}

    // --- libraries -----------------------------------------------------------------
    std::vector<RemoteLibrary> libraries(const std::string& username);  // owned + shared
    std::vector<RemoteLibrary> allLibraries();                          // role is empty
    std::optional<RemoteLibrary> library(const std::string& uid, const std::string& username);
    RemoteLibrary ensureLibrary(const std::string& uid, const std::string& name, const std::string& type,
                                const std::string& username);
    void share(const std::string& uid, const std::string& username, const std::string& role);
    bool unshare(const std::string& uid, const std::string& username);
    std::vector<Json> members(const std::string& uid);  // {username, role}
    // requireEmpty: refuses (409) while the library has items or collections that are
    // not deleted.
    void deleteLibrary(const std::string& uid, bool requireEmpty = false);
    void renameLibrary(const std::string& uid, const std::string& name);
    // "owner", "write", "read" or "" when the user has no access.
    std::string role(const std::string& uid, const std::string& username);

    // --- objects ---------------------------------------------------------------------
    ChangesPage changes(const std::string& uid, int64_t since, const std::string& cursor = {},
                        int limit = kDefaultChangesLimit);
    PushResult push(const std::string& uid, const std::vector<PushObject>& objects);

    // --- files -------------------------------------------------------------------------
    std::vector<FileInfo> files(const std::string& uid);
    std::optional<FileInfo> file(const std::string& uid, const std::string& key);
    fs::path blobPath(const std::string& uid, const std::string& key) const;
    // A fresh path inside the store (same filesystem as the blobs) for partial uploads.
    fs::path newTempPath() const;
    // Moves tmpFile into place (it is left untouched on failure). Rejects with 400 when
    // meta.md5 is set and does not match, with 409 when the item has been deleted.
    FileInfo putFile(const std::string& uid, const std::string& key, const fs::path& tmpFile,
                     const FileInfo& meta);
    bool deleteFile(const std::string& uid, const std::string& key);

    // What the store holds, and room left: {database (bytes), files {count, bytes}, disk
    // (see diskSpace), libraries: the user's, each as listed with items, notes,
    // attachments, annotations, files and fileBytes (what is not deleted)}.
    Json usage(const std::string& username);

private:
    void initSchema();
    void cleanTempDir();
    void requireUser(const std::string& username);
    int64_t requireLibraryVersion(const std::string& uid);
    std::string roleLocked(const std::string& uid, const std::string& username);
    std::optional<RemoteObject> loadObject(const std::string& uid, const std::string& type, const std::string& key);
    std::optional<FileInfo> fileLocked(const std::string& uid, const std::string& key);
    void removeBlob(const std::string& uid, const std::string& key);

    fs::path root_;
    sql::Db db_;
    std::mutex mutex_;  // serializes all access to db_ (and blob moves)
};

} // namespace on::sync
