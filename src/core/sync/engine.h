#pragma once

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "core/library.h"
#include "core/sync/protocol.h"

namespace on::sync {

struct SyncOptions {
    bool syncFiles = true;
    bool downloadFiles = true;  // false: "on demand" mode, files are only uploaded
    std::function<void(const std::string&)> log;
    std::function<void(const std::string& stage, int done, int total)> progress;
    std::function<bool()> cancelled;
};

// An object a sync brought in or sent out: its last change in that sync.
struct SyncChange {
    int64_t libraryId = 0;
    std::string library;    // the library's name
    std::string type;       // "item", "collection", "search", "setting", or "file" (an attachment's file)
    std::string key;
    std::string kind;       // the item type ("note", "annotation", "book", ...); else the type
    std::string title;      // a note's first line, an annotation's text, a name; may be empty
    std::string parentKey;  // the item a note, attachment or annotation belongs to
    std::string action;     // "added", "changed" or "deleted"
    bool incoming = false;  // from the target (else sent to it)
    std::string by;         // sent: the programs that made it, when not only the app (Library::changedBy)
};

struct SyncReport {
    int libraries = 0;
    int pulled = 0;
    int pushed = 0;
    int deletedLocally = 0;
    int librariesRemoved = 0;  // empty libraries no longer on the target
    int conflicts = 0;
    int uploaded = 0;
    int downloaded = 0;
    std::vector<std::string> errors;
    std::vector<SyncChange> changes;  // one per object, in the order of their last change
    bool cancelled = false;

    std::string summary() const;
};

// Two-way synchronisation of a Library with a Transport (server or folder).
//
// Per library: pull changes since the last known remote version, reconcile them
// with local edits (last writer wins by dateModified; the losing version is kept
// in the conflicts table), transfer attachment files, then push local changes.
class SyncEngine {
public:
    SyncEngine(Library& library, Transport& transport, std::string targetId, SyncOptions options = {});

    // Libraries are matched by uid: the target's are added here and ours uploaded,
    // except an empty one that was deleted on the target (or a fresh install's empty
    // default library, when the target already has the user's own), which is removed.
    SyncReport syncAll();
    void syncLibrary(int64_t libraryId, SyncReport& report);
    // Delete an (empty) library on the target; done when it is not there.
    void deleteRemoteLibrary(const std::string& uid);
    // Fetch one attachment file (for "on demand" file mode).
    bool downloadAttachment(int64_t libraryId, const std::string& key);
    // Fetch attachment files (to copy them: Library::filesNotHere). One the target does not
    // have, while it has their library, is left as it is (as is each of a library not synced
    // with it); for each that could not be fetched otherwise, why (the target out of reach,
    // or without the library: a folder's drive not there).
    std::map<std::string, std::string> downloadAttachments(int64_t libraryId, const std::vector<std::string>& keys);

private:
    void pull(LibraryInfo& lib, SyncReport& report);
    bool push(LibraryInfo& lib, SyncReport& report);
    void syncFiles(LibraryInfo& lib, SyncReport& report);
    void applyRemote(int64_t lib, const RemoteObject& o, SyncReport& report);
    void syncName(const LibraryInfo& local, RemoteLibrary& remote);
    void record(SyncReport& report, int64_t lib, const std::string& type, const std::string& key, const Json& data,
                const std::string& action, bool incoming, const std::string& by = {});
    bool cancelled() const { return options_.cancelled && options_.cancelled(); }
    void log(const std::string& msg) const;
    void progress(const std::string& stage, int done, int total) const;

    Library& library_;
    Transport& transport_;
    std::string targetId_;
    SyncOptions options_;
    // Where each object is in the changes of the report record() last added to (by library,
    // type, key), so that a sync of many objects does not search them all for each one. An
    // object recorded again leaves its earlier change marked superseded (taking it out there,
    // and moving all after it, made a sync bringing everything in twice quadratic), which
    // compact() takes out at the end of a library's sync.
    std::map<std::tuple<int64_t, std::string, std::string>, size_t> changeIndex_;
    std::vector<bool> superseded_;
    const SyncReport* indexed_ = nullptr;
    size_t indexedSize_ = 0;
    void compact(SyncReport& report);
};

} // namespace on::sync
