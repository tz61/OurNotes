#include "core/sync/engine.h"

#include <algorithm>
#include <map>

#include "core/schema.h"
#include "core/util.h"

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

std::string shortened(std::string s)
{
    for (auto& c : s)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    if (s.size() > 120) {
        size_t cut = 117;
        while (cut > 0 && (s[cut] & 0xC0) == 0x80)  // not inside a UTF-8 sequence
            --cut;
        s = s.substr(0, cut) + "...";
    }
    return s;
}

} // namespace

std::string SyncReport::summary() const
{
    std::string s = "pulled " + std::to_string(pulled) + ", pushed " + std::to_string(pushed);
    if (deletedLocally)
        s += ", removed " + std::to_string(deletedLocally);
    if (librariesRemoved)
        s += ", " + std::to_string(librariesRemoved) + " empty librar" + (librariesRemoved == 1 ? "y" : "ies") +
             " removed";
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
    bool targetHasOwnLibrary =
        std::any_of(remote.begin(), remote.end(), [](auto& r) { return r.type == "user" && r.role == "owner"; });

    for (auto& lib : library_.libraries()) {
        bool known = remoteByUid.count(lib.uid) > 0;
        if (!known && library_.libraryContents(lib.id).empty()) {
            // sync_target is only set once the target has had the library
            std::string why;
            if (lib.syncTarget == targetId_)
                why = "it is no longer on the sync target";
            else if (lib.syncTarget.empty() && lib.type == "user" && targetHasOwnLibrary)
                why = "the sync target has your library already";  // made at first start
            if (!why.empty()) {
                log("Removing the empty library \"" + lib.name + "\": " + why);
                library_.deleteLibrary(lib.id);
                ++report.librariesRemoved;
                continue;
            }
        }
        if (!known) {
            try {
                remoteByUid[lib.uid] = transport_.ensureLibrary(lib.uid, lib.name, lib.type);
            } catch (const SyncError& e) {
                if (e.status != 403)
                    throw;
                report.errors.push_back(lib.name + ": " + e.what());  // no longer shared with us
                continue;
            }
            library_.setSyncName(lib.id, lib.name);
            if (lib.syncTarget == targetId_) {
                // deleted there while it had entries here: upload them all again
                log("Uploading \"" + lib.name + "\" again: it was no longer on the sync target");
                library_.resetSyncState(lib.id, targetId_);
            }
        }
        if (lib.syncTarget != targetId_) {
            log("First sync of \"" + lib.name + "\" with this target");
            library_.resetSyncState(lib.id, targetId_);
        }
    }
    for (auto& [uid, r] : remoteByUid) {
        auto local = library_.libraryByUid(uid);
        int64_t id = local ? local->id : library_.createLibrary(r.name, r.type, uid);
        if (!local) {
            log("Adding library \"" + r.name + "\" from sync target");
            library_.setSyncState(id, targetId_, 0);
            library_.setSyncName(id, r.name);
        } else if (local->name != r.name) {
            syncName(*local, r);
        } else if (local->syncName != r.name) {
            library_.setSyncName(id, r.name);
        }
        library_.setLibraryEditable(id, r.role != "read");
    }

    for (auto& lib : library_.libraries()) {
        if (!remoteByUid.count(lib.uid))
            continue;  // could not be uploaded
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

// The side whose name changed since the two last agreed wins; when both did, the
// target's. Before they first agree (versions before 0.1.3 did not sync names), a
// local name other than the default one is kept.
void SyncEngine::syncName(const LibraryInfo& local, RemoteLibrary& remote)
{
    const auto& base = local.syncName;
    bool localChanged = base.empty() ? local.name != Library::kDefaultName : local.name != base;
    bool remoteChanged = !base.empty() && remote.name != base;
    if (remote.role == "owner" && localChanged && !remoteChanged) {
        log("Renaming \"" + remote.name + "\" to \"" + local.name + "\" on the sync target");
        remote = transport_.ensureLibrary(remote.uid, local.name, remote.type);
    } else {
        log("Renaming \"" + local.name + "\" to \"" + remote.name + "\" as on the sync target");
        library_.renameLibrary(local.id, remote.name);
    }
    library_.setSyncName(local.id, remote.name);
}

void SyncEngine::deleteRemoteLibrary(const std::string& uid)
{
    try {
        transport_.deleteLibrary(uid);
    } catch (const SyncError& e) {
        if (e.status != 404 && e.status != 405)
            throw;
        // not there, or a server without this call
        auto remote = transport_.libraries();
        if (std::any_of(remote.begin(), remote.end(), [&](auto& r) { return r.uid == uid; }))
            throw SyncError("the sync server cannot delete libraries; update it to OurNotes 0.1.3 or later", 501);
    }
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
    struct Compacted {  // its changes recorded again taken out, when done or given up
        SyncEngine* engine;
        SyncReport& report;
        ~Compacted() { engine->compact(report); }
    } compacted{this, report};
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
                page = transport_.changes(lib.uid, since, cursor, kPageSize);  // (no cursor yet: a continue would end the loop)
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

void SyncEngine::record(SyncReport& report, int64_t lib, const std::string& type, const std::string& key,
                        const Json& data, const std::string& action, bool incoming, const std::string& by)
{
    SyncChange c;
    c.libraryId = lib;
    c.type = type;
    c.key = key;
    c.action = action;
    c.incoming = incoming;
    c.by = by;
    c.kind = type;
    if (data.is_object()) {
        if (type == "item" || type == "file") {
            auto itemType = jstr(data, "itemType");
            if (type == "item")
                c.kind = itemType;
            if (itemType == "note")
                c.title = noteTitle(jstr(data, "note"));
            else if (itemType == "annotation")
                c.title = jstr(data, "annotationText").empty() ? jstr(data, "annotationComment")
                                                               : jstr(data, "annotationText");
            else
                c.title = jstr(data, "title").empty() ? jstr(data, "filename") : jstr(data, "title");
            c.parentKey = jstr(data, "parentItem");
        } else {
            c.title = jstr(data, "name");
        }
    }
    if (type == "setting")
        c.title = key;
    c.title = shortened(c.title);
    if (indexed_ != &report || indexedSize_ != report.changes.size()) {  // another report, or changed elsewhere
        compact(report);
        changeIndex_.clear();
        for (size_t i = 0; i < report.changes.size(); ++i) {
            auto& o = report.changes[i];
            changeIndex_[{o.libraryId, o.type, o.key}] = i;
        }
        superseded_.assign(report.changes.size(), false);
        indexed_ = &report;
    }
    auto found = changeIndex_.find({lib, type, key});
    if (found != changeIndex_.end()) {
        auto& earlier = report.changes[found->second];
        // the last change wins, but an object that came or went in this sync stays added
        if (earlier.action == "added" && action == "changed")
            c.action = "added";
        if (c.title.empty())
            c.title = earlier.title;
        if (c.by.empty() && !incoming && !earlier.incoming)
            c.by = earlier.by;
        superseded_[found->second] = true;  // (taken out by compact())
    }
    if (auto info = library_.library(lib))
        c.library = info->name;
    changeIndex_[{lib, type, key}] = report.changes.size();
    report.changes.push_back(std::move(c));
    superseded_.push_back(false);
    indexedSize_ = report.changes.size();
}

void SyncEngine::compact(SyncReport& report)
{
    if (indexed_ != &report || superseded_.size() != report.changes.size())
        return;  // (nothing marked in it)
    size_t kept = 0;
    for (size_t i = 0; i < report.changes.size(); ++i)
        if (!superseded_[i]) {
            if (kept != i)
                report.changes[kept] = std::move(report.changes[i]);
            ++kept;
        }
    report.changes.resize(kept);
    indexed_ = nullptr;  // (made again at the next record())
    changeIndex_.clear();
    superseded_.clear();
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
            record(report, lib, o.type, o.key, local->data, "deleted", true);
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
        record(report, lib, o.type, o.key, o.data, "added", true);
        return;
    }

    if (local->synced) {
        if (local->version >= o.version)
            return;  // already have it (typically our own push coming back)
        library_.putRemoteObject(lib, o.type, o.key, o.data, o.version);
        ++report.pulled;
        record(report, lib, o.type, o.key, o.data, "changed", true);
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
        record(report, lib, o.type, o.key, o.data, "changed", true);
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
    for (auto& d : dirty)  // (moved: a first sync has every object here)
        pending.push_back({{d.type, d.key, d.version, false, std::move(d.data)}, d.rev});
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
                    auto by = library_.changedBy(lib.id, p.obj.type, p.obj.key);
                    if (p.obj.deleted)
                        library_.clearTombstone(lib.id, p.obj.type, p.obj.key);
                    else
                        library_.markPushed(lib.id, p.obj.type, p.obj.key, e.version, p.rev);
                    ++report.pushed;
                    record(report, lib.id, p.obj.type, p.obj.key, p.obj.data,
                           p.obj.deleted ? "deleted" : p.obj.baseVersion == 0 ? "added" : "changed", false, by);
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

    // only the attachments are read (every sync), not the whole library
    std::vector<Json> attachments;
    {
        auto st = library_.db().query("SELECT data FROM items WHERE library_id=? AND item_type='attachment'", lib.id);
        while (st.step()) {
            auto d = Json::parse(st.str(0));
            if (schema::isStoredLinkMode(jstr(d, "linkMode")) && !jstr(d, "filename").empty())
                attachments.push_back(std::move(d));
        }
    }

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
            // made by whoever made its attachment (still to be pushed: files go first)
            record(report, lib.id, "file", key, att, it == remote.end() ? "added" : "changed", false,
                   library_.changedBy(lib.id, "item", key));
        };
        auto download = [&](const FileInfo& info) {
            if (!options_.downloadFiles)
                return;
            fs::create_directories(path.parent_path());
            auto got = transport_.download(lib.uid, key, path);
            library_.fileMd5(lib.id, key, path);
            library_.setSyncedMd5(lib.id, key, got.md5.empty() ? info.md5 : got.md5);
            ++report.downloaded;
            record(report, lib.id, "file", key, att, exists ? "changed" : "added", true);
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
                // kept: not a conflict again if the download is skipped (on demand) or fails
                library_.setSyncedMd5(lib.id, key, localMd5);
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
