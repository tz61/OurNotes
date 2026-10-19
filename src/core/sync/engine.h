#pragma once

#include <functional>
#include <string>
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

private:
    void pull(LibraryInfo& lib, SyncReport& report);
    bool push(LibraryInfo& lib, SyncReport& report);
    void syncFiles(LibraryInfo& lib, SyncReport& report);
    void applyRemote(int64_t lib, const RemoteObject& o, SyncReport& report);
    void syncName(const LibraryInfo& local, RemoteLibrary& remote);
    bool cancelled() const { return options_.cancelled && options_.cancelled(); }
    void log(const std::string& msg) const;
    void progress(const std::string& stage, int done, int total) const;

    Library& library_;
    Transport& transport_;
    std::string targetId_;
    SyncOptions options_;
};

} // namespace on::sync
