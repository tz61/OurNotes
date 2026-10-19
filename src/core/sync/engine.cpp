#include "core/sync/engine.h"

#include <algorithm>
#include <map>

#include "core/schema.h"

namespace on::sync {

namespace {

constexpr int kPageSize = 1000;
constexpr size_t kPushBatch = 250;
constexpr int kMaxRounds = 5;

Json withoutVolatile(Json d)
{
    if (d.is_object()) {
        d.erase("version");
        d.erase("dateModified");
    }
    return d;
}

std::string modifiedOf(const Json& d) { return d.is_object() ? jstr(d, "dateModified") : std::string(); }

} // namespace

std::string SyncReport::summary() const
{
    std::string s = "pulled " + std::to_string(pulled) + ", pushed " + std::to_string(pushed);
    if (deletedLocally)
        s += ", removed " + std::to_string(deletedLocally);
    if (uploaded || downloaded)
        s += ", files up " + std::to_string(uploaded) + " / down " + std::to_string(downloaded);
    if (conflicts)
        s += ", " + std::to_string(conflicts) + " conflict(s) resolved";
    if (!errors.empty())
        s += ", " + std::to_string(errors.size()) + " error(s)";
    if (cancelled)
        s += " (cancelled)";
    return s;
}

SyncEngine::SyncEngine(Library& library, Transport& transport, std::string targetId, SyncOptions options)
    : library_(library), transport_(transport), targetId_(std::move(targetId)), options_(std::move(options))
{
}

void SyncEngine::log(const std::string& msg) const
{
    if (options_.log)
        options_.log(msg);
}

void SyncEngine::progress(const std::string& stage, int done, int total) const
{
    if (options_.progress)
        options_.progress(stage, done, total);
}

SyncReport SyncEngine::syncAll()
{
    SyncReport report;
    auto remote = transport_.libraries();
    std::map<std::string, RemoteLibrary> remoteByUid;
    for (auto& r : remote)
        remoteByUid[r.uid] = r;

    for (auto& lib : library_.libraries()) {
        if (lib.syncTarget != targetId_) {
            log("First sync of \"" + lib.name + "\" with this target");
            library_.resetSyncState(lib.id, targetId_);
        }
        if (!remoteByUid.count(lib.uid))
            remoteByUid[lib.uid] = transport_.ensureLibrary(lib.uid, lib.name, lib.type);
    }
    for (auto& [uid, r] : remoteByUid) {
        auto local = library_.libraryByUid(uid);
        int64_t id = local ? local->id : library_.createLibrary(r.name, r.type, uid);
        if (!local) {
            log("Adding library \"" + r.name + "\" from sync target");
            library_.setSyncState(id, targetId_, 0);
        }
        library_.setLibraryEditable(id, r.role != "read");
    }

    for (auto& lib : library_.libraries()) {
        if (cancelled()) {
            report.cancelled = true;
            break;
        }
        try {
            syncLibrary(lib.id, report);
            ++report.libraries;
        } catch (const std::exception& e) {
            report.errors.push_back(lib.name + ": " + e.what());
            log("Error syncing \"" + lib.name + "\": " + e.what());
            if (auto* se = dynamic_cast<const SyncError*>(&e); se && (se->status == 401 || se->status == 0))
                break;  // authentication or connection problem: other libraries will fail too
        }
    }
    library_.notifyRefresh();
    return report;
}

void SyncEngine::syncLibrary(int64_t libraryId, SyncReport& report)
{
    auto lib = library_.library(libraryId);
    if (!lib)
        throw Error("no such library");
    if (lib->syncTarget != targetId_) {
        library_.resetSyncState(lib->id, targetId_);
        lib = library_.library(libraryId);
    }
    log("Syncing \"" + lib->name + "\"");
    pull(*lib, report);
    if (options_.syncFiles && !cancelled())
        syncFiles(*lib, report);
    for (int round = 0; round < kMaxRounds && !cancelled(); ++round) {
        if (!push(*lib, report))
            break;
        pull(*lib, report);
    }
    if (cancelled())
        report.cancelled = true;
}

// --- pull ---------------------------------------------------------------------------------

void SyncEngine::pull(LibraryInfo& lib, SyncReport& report)
{
    int64_t since = lib.syncVersion;
    std::string cursor;
    int64_t newVersion = -1;
    int seen = 0;
    do {
        auto page = transport_.changes(lib.uid, since, cursor, kPageSize);
        if (newVersion < 0) {
            if (page.version < since) {
                // The remote library is older than our state (restored backup or
                // recreated): start over and re-upload everything it is missing.
                log("Remote library \"" + lib.name + "\" was reset; resynchronising");
                library_.resetSyncState(lib.id, targetId_);
                since = 0;
                lib.syncVersion = 0;
                cursor.clear();
                newVersion = -1;
                continue;
            }
            newVersion = page.version;
        }
        {
            Library::BatchGuard batch(library_);
            sql::Transaction tx(library_.db());
            for (auto& o : page.objects)
                applyRemote(lib.id, o, report);
            tx.commit();
        }
        seen += (int)page.objects.size();
        progress("Downloading changes", seen, seen);
        cursor = page.next;
        if (cancelled()) {
            report.cancelled = true;
            return;  // keep the old version: the next sync re-fetches this range
        }
    } while (!cursor.empty());
    if (newVersion >= 0) {
        lib.syncVersion = std::max(since, newVersion);
        library_.setSyncState(lib.id, targetId_, lib.syncVersion);
    }
}

void SyncEngine::applyRemote(int64_t lib, const RemoteObject& o, SyncReport& report)
{
    if (!isObjectType(o.type) || !isValidKey(o.key))
        return;
    auto local = library_.getObject(lib, o.type, o.key);

    if (o.deleted) {
        if (!local) {
            library_.clearTombstone(lib, o.type, o.key);
            return;
        }
        if (local->synced || local->version >= o.version) {
            library_.deleteRemoteObject(lib, o.type, o.key);
            ++report.deletedLocally;
            return;
        }
        // Edited here but deleted remotely: keep the edit (it will be re-created remotely).
        library_.recordConflict(lib, o.type, o.key, local->data, nullptr, "local");
        library_.setObjectVersion(lib, o.type, o.key, o.version);
        ++report.conflicts;
        return;
    }

    if (!local) {
        if (auto tomb = library_.tombstone(lib, o.type, o.key)) {
            if (o.version <= tomb->version)
                return;  // our deletion is newer; it will be pushed
            // Deleted here but changed remotely: restore the remote version rather than lose it.
            library_.recordConflict(lib, o.type, o.key, nullptr, o.data, "remote");
            ++report.conflicts;
        }
        library_.putRemoteObject(lib, o.type, o.key, o.data, o.version);
        ++report.pulled;
        return;
    }

    if (local->synced) {
        if (local->version >= o.version)
            return;  // already have it (typically our own push coming back)
        library_.putRemoteObject(lib, o.type, o.key, o.data, o.version);
        ++report.pulled;
        return;
    }

    // Local object has unsynced changes.
    if (withoutVolatile(local->data) == withoutVolatile(o.data)) {
        library_.markPushed(lib, o.type, o.key, o.version, local->rev);
        return;
    }
    if (local->version >= o.version)
        return;  // remote unchanged since our last sync: our edit simply wins
    if (modifiedOf(o.data) > modifiedOf(local->data)) {
        library_.recordConflict(lib, o.type, o.key, local->data, o.data, "remote");
        library_.putRemoteObject(lib, o.type, o.key, o.data, o.version);
    } else {
        library_.recordConflict(lib, o.type, o.key, local->data, o.data, "local");
        library_.setObjectVersion(lib, o.type, o.key, o.version);
    }
    ++report.conflicts;
}

// --- push ---------------------------------------------------------------------------------

bool SyncEngine::push(LibraryInfo& lib, SyncReport& report)
{
    if (!lib.editable && lib.syncVersion > 0)
        return false;
    auto dirty = library_.dirtyObjects(lib.id);
    auto tombs = library_.tombstones(lib.id);
    if (dirty.empty() && tombs.empty())
        return false;

    struct Pending {
        PushObject obj;
        int64_t rev;
    };
    std::vector<Pending> pending;
    for (auto& d : dirty)
        pending.push_back({{d.type, d.key, d.version, false, d.data}, d.rev});
    for (auto& t : tombs)
        pending.push_back({{t.type, t.key, t.version, true, nullptr}, 0});

    bool needPull = false;
    int done = 0;
    for (size_t start = 0; start < pending.size() && !cancelled(); start += kPushBatch) {
        size_t end = std::min(pending.size(), start + kPushBatch);
        std::vector<PushObject> batch;
        for (size_t i = start; i < end; ++i)
            batch.push_back(pending[i].obj);
        auto result = transport_.push(lib.uid, batch);
        bool batchConflict = false;
        bool wroteCurrentVersion = false;
        {
            Library::BatchGuard guard(library_);
            sql::Transaction tx(library_.db());
            for (size_t i = 0; i < result.results.size() && i < batch.size(); ++i) {
                auto& e = result.results[i];
                auto& p = pending[start + i];
                if (e.status == "ok") {
                    wroteCurrentVersion = wroteCurrentVersion || e.version == result.version;
                    if (p.obj.deleted)
                        library_.clearTombstone(lib.id, p.obj.type, p.obj.key);
                    else
                        library_.markPushed(lib.id, p.obj.type, p.obj.key, e.version, p.rev);
                    ++report.pushed;
                } else if (e.status == "conflict") {
                    batchConflict = true;
                    if (e.current)
                        applyRemote(lib.id, *e.current, report);
                } else {
                    report.errors.push_back(p.obj.type + " " + p.obj.key + ": " + e.message);
                }
            }
            tx.commit();
        }
        // If our batch produced the only change since the last pull, skip re-pulling it.
        // (An "ok" for a no-op, e.g. re-deleting a tombstone, does not create a version.)
        if (!batchConflict && wroteCurrentVersion && result.version == lib.syncVersion + 1) {
            lib.syncVersion = result.version;
            library_.setSyncState(lib.id, targetId_, lib.syncVersion);
        } else if (result.version != lib.syncVersion) {
            needPull = true;
        }
        needPull = needPull || batchConflict;
        done += (int)batch.size();
        progress("Uploading changes", done, (int)pending.size());
    }
    // After a conflict resolved in our favour the object is still dirty: the next round pushes it.
    return needPull;
}

// --- files --------------------------------------------------------------------------------

void SyncEngine::syncFiles(LibraryInfo& lib, SyncReport& report)
{
    std::map<std::string, FileInfo> remote;
    for (auto& f : transport_.files(lib.uid))
        remote[f.key] = f;

    std::vector<Json> attachments;
    for (auto& d : library_.allItems(lib.id, true))
        if (jstr(d, "itemType") == "attachment" && schema::isStoredLinkMode(jstr(d, "linkMode")) &&
            !jstr(d, "filename").empty())
            attachments.push_back(d);

    int done = 0;
    for (auto& att : attachments) {
        if (cancelled())
            return;
        progress("Syncing files", ++done, (int)attachments.size());
        auto key = jstr(att, "key");
        auto path = library_.attachmentFile(lib.id, att);
        std::error_code ec;
        bool exists = fs::is_regular_file(path, ec);
        auto it = remote.find(key);
        auto state = library_.fileState(lib.id, key);
        std::string synced = state ? state->syncedMd5 : std::string();

        auto upload = [&](const std::string& md5) {
            if (!lib.editable)
                return;
            FileInfo meta;
            meta.key = key;
            meta.md5 = md5;
            meta.filename = jstr(att, "filename");
            meta.contentType = jstr(att, "contentType");
            meta.mtime = jint(att, "mtime");
            transport_.upload(lib.uid, key, path, meta);
            library_.setSyncedMd5(lib.id, key, md5);
            ++report.uploaded;
        };
        auto download = [&](const FileInfo& info) {
            if (!options_.downloadFiles)
                return;
            fs::create_directories(path.parent_path());
            auto got = transport_.download(lib.uid, key, path);
            library_.fileMd5(lib.id, key, path);
            library_.setSyncedMd5(lib.id, key, got.md5.empty() ? info.md5 : got.md5);
            ++report.downloaded;
        };

        try {
            if (!exists) {
                if (it != remote.end())
                    download(it->second);
                continue;
            }
            auto localMd5 = library_.fileMd5(lib.id, key, path);
            // Only a locally changed file may update the item's checksum; a stale
            // file that is about to be replaced must not.
            auto adoptLocal = [&] {
                if (jstr(att, "md5") != localMd5)
                    library_.refreshAttachmentFile(lib.id, key);
            };
            if (it == remote.end()) {
                adoptLocal();
                upload(localMd5);
            } else if (it->second.md5 == localMd5) {
                adoptLocal();
                if (synced != localMd5)
                    library_.setSyncedMd5(lib.id, key, localMd5);
            } else if (localMd5 == synced) {
                download(it->second);
            } else if (it->second.md5 == synced) {
                adoptLocal();
                upload(localMd5);
            } else {
                // Both sides changed: keep the local file as a separate attachment, take the remote one.
                log("File conflict for " + jstr(att, "filename") + "; keeping both copies");
                auto copy = path.parent_path().parent_path() / (key + ".conflict");
                fs::create_directories(copy);
                auto copyFile = copy / path.filename();
                fs::copy_file(path, copyFile, fs::copy_options::overwrite_existing);
                library_.importAttachment(lib.id, copyFile, jstr(att, "parentItem"), false,
                                          jstr(att, "title") + " (conflicted copy " + nowIso().substr(0, 10) + ")");
                fs::remove_all(copy, ec);
                download(it->second);
                ++report.conflicts;
            }
        } catch (const std::exception& e) {
            report.errors.push_back(jstr(att, "filename") + ": " + e.what());
            if (auto* se = dynamic_cast<const SyncError*>(&e); se && se->status == 0)
                throw;
        }
    }
}

bool SyncEngine::downloadAttachment(int64_t libraryId, const std::string& key)
{
    auto lib = library_.library(libraryId);
    auto att = library_.item(libraryId, key);
    if (!lib || !att)
        return false;
    auto path = library_.attachmentFile(libraryId, *att);
    if (path.empty())
        return false;
    fs::create_directories(path.parent_path());
    auto info = transport_.download(lib->uid, key, path);
    library_.fileMd5(libraryId, key, path);
    library_.setSyncedMd5(libraryId, key, info.md5);
    return true;
}

} // namespace on::sync
