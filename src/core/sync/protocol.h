#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/util.h"

// Wire types of the OurNotes sync protocol (v1), shared by the client, the local
// folder backend and the ournotesd daemon. See docs/sync-protocol.md.
namespace on::sync {

constexpr int kApiVersion = 1;

// Synced object types.
inline constexpr const char* kCollection = "collection";
inline constexpr const char* kItem = "item";
inline constexpr const char* kSetting = "setting";
inline constexpr const char* kSearch = "search";
bool isObjectType(const std::string& type);

class SyncError : public on::Error {
public:
    explicit SyncError(const std::string& msg, int status = 0) : on::Error(msg), status(status) {}
    int status;  // HTTP status, 0 for transport/IO errors
};

struct RemoteObject {
    std::string type;
    std::string key;
    int64_t version = 0;
    bool deleted = false;
    Json data;  // null when deleted
};

struct RemoteLibrary {
    std::string uid;
    std::string name;
    std::string type;   // "user" | "group"
    std::string role;   // "owner" | "write" | "read"
    std::string owner;
    int64_t version = 0;
};

struct ChangesPage {
    int64_t version = 0;  // library version when the page was produced
    std::vector<RemoteObject> objects;
    std::string next;     // opaque cursor; empty when there are no more pages
};

struct PushObject {
    std::string type;
    std::string key;
    int64_t baseVersion = 0;  // version the client last saw (0 = new object)
    bool deleted = false;
    Json data;
};

struct PushEntry {
    std::string type;
    std::string key;
    std::string status;  // "ok" | "conflict" | "error"
    int64_t version = 0;
    std::optional<RemoteObject> current;  // server copy on conflict
    std::string message;
};

struct PushResult {
    int64_t version = 0;  // library version after the push
    std::vector<PushEntry> results;  // same order as the request
};

struct FileInfo {
    std::string key;
    std::string md5;
    std::string filename;
    std::string contentType;
    int64_t size = 0;
    int64_t mtime = 0;  // milliseconds since epoch
};

Json toJson(const RemoteObject& o);
Json toJson(const RemoteLibrary& l);
Json toJson(const ChangesPage& p);
Json toJson(const PushObject& o);
Json toJson(const PushEntry& e);
Json toJson(const PushResult& r);
Json toJson(const FileInfo& f);

RemoteObject remoteObjectFromJson(const Json& j);
RemoteLibrary remoteLibraryFromJson(const Json& j);
ChangesPage changesPageFromJson(const Json& j);
PushObject pushObjectFromJson(const Json& j);
PushEntry pushEntryFromJson(const Json& j);
PushResult pushResultFromJson(const Json& j);
FileInfo fileInfoFromJson(const Json& j);

// Return false to cancel a transfer.
using ProgressFn = std::function<bool(int64_t done, int64_t total)>;

// A sync backend: an ournotesd server (HttpTransport) or a folder (LocalTransport).
class Transport {
public:
    virtual ~Transport() = default;

    virtual Json info() = 0;
    virtual std::vector<RemoteLibrary> libraries() = 0;
    // Create the library if it does not exist (the caller becomes its owner); for its
    // owner, also renames it to name.
    virtual RemoteLibrary ensureLibrary(const std::string& uid, const std::string& name, const std::string& type) = 0;
    // Delete an empty library (owner only): 409 while it has entries, 404 when it is
    // not there (or the server predates this call).
    virtual void deleteLibrary(const std::string& uid) = 0;
    virtual ChangesPage changes(const std::string& uid, int64_t since, const std::string& cursor, int limit) = 0;
    virtual PushResult push(const std::string& uid, const std::vector<PushObject>& objects) = 0;

    virtual std::vector<FileInfo> files(const std::string& uid) = 0;
    virtual FileInfo download(const std::string& uid, const std::string& key, const fs::path& dest,
                              const ProgressFn& progress = {}) = 0;
    virtual FileInfo upload(const std::string& uid, const std::string& key, const fs::path& src,
                            const FileInfo& meta, const ProgressFn& progress = {}) = 0;
};

} // namespace on::sync
