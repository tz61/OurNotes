#include <doctest.h>

#include <fstream>
#include <map>

#include "core/library.h"
#include "core/sync/engine.h"
#include "test_helpers.h"

using namespace on;
using namespace on::sync;

namespace {

// In-memory server with the same versioning semantics as ournotesd's Store.
class FakeTransport : public Transport {
public:
    struct Lib {
        RemoteLibrary info;
        std::map<std::pair<std::string, std::string>, RemoteObject> objects;
        std::map<std::string, std::pair<FileInfo, std::string>> files;
    };
    std::map<std::string, Lib> libs;

    Json info() override { return Json{{"server", "fake"}}; }
    std::vector<RemoteLibrary> libraries() override
    {
        std::vector<RemoteLibrary> out;
        for (auto& [uid, l] : libs)
            out.push_back(l.info);
        return out;
    }
    RemoteLibrary ensureLibrary(const std::string& uid, const std::string& name, const std::string& type) override
    {
        auto& l = libs[uid];
        if (l.info.uid.empty())
            l.info = {uid, name, type, "owner", "me", 0};
        else if (l.info.role == "owner" && !name.empty())
            l.info.name = name;
        return l.info;
    }
    bool canDelete = true;  // false: like a server from before libraries could be deleted
    void deleteLibrary(const std::string& uid) override
    {
        auto it = libs.find(uid);
        if (!canDelete || it == libs.end())
            throw SyncError("not found", 404);
        for (auto& [k, o] : it->second.objects)
            if (!o.deleted && (o.type == "item" || o.type == "collection"))
                throw SyncError("not empty", 409);
        libs.erase(it);
    }
    ChangesPage changes(const std::string& uid, int64_t since, const std::string& cursor, int limit) override
    {
        auto& l = libs.at(uid);
        std::vector<RemoteObject> all;
        for (auto& [k, o] : l.objects)
            if (o.version > since && !(since == 0 && o.deleted))
                all.push_back(o);
        std::sort(all.begin(), all.end(), [](auto& a, auto& b) {
            return std::tie(a.version, a.type, a.key) < std::tie(b.version, b.type, b.key);
        });
        size_t start = cursor.empty() ? 0 : std::stoul(cursor);
        ChangesPage p;
        p.version = l.info.version;
        for (size_t i = start; i < all.size() && (int)p.objects.size() < limit; ++i)
            p.objects.push_back(all[i]);
        if (start + p.objects.size() < all.size())
            p.next = std::to_string(start + p.objects.size());
        return p;
    }
    PushResult push(const std::string& uid, const std::vector<PushObject>& objects) override
    {
        auto& l = libs.at(uid);
        PushResult r;
        int64_t next = l.info.version + 1;
        bool wrote = false;
        for (auto& o : objects) {
            PushEntry e{o.type, o.key, "ok", next, std::nullopt, {}};
            auto it = l.objects.find({o.type, o.key});
            if (it != l.objects.end() && it->second.version > o.baseVersion) {
                e.status = "conflict";
                e.version = it->second.version;
                e.current = it->second;
            } else {
                l.objects[{o.type, o.key}] = RemoteObject{o.type, o.key, next, o.deleted, o.data};
                if (o.deleted && o.type == "item")
                    l.files.erase(o.key);
                wrote = true;
            }
            r.results.push_back(e);
        }
        if (wrote)
            l.info.version = next;
        r.version = l.info.version;
        return r;
    }
    std::vector<FileInfo> files(const std::string& uid) override
    {
        std::vector<FileInfo> out;
        for (auto& [k, f] : libs.at(uid).files)
            out.push_back(f.first);
        return out;
    }
    FileInfo download(const std::string& uid, const std::string& key, const fs::path& dest,
                      const ProgressFn&) override
    {
        auto& f = libs.at(uid).files.at(key);
        fs::create_directories(dest.parent_path());
        std::ofstream(dest, std::ios::binary) << f.second;
        return f.first;
    }
    FileInfo upload(const std::string& uid, const std::string& key, const fs::path& src, const FileInfo& meta,
                    const ProgressFn&) override
    {
        auto content = readFile(src);
        FileInfo info = meta;
        info.key = key;
        info.md5 = md5Hex(content);
        info.size = (int64_t)content.size();
        libs.at(uid).files[key] = {info, content};
        return info;
    }
};

SyncReport syncWith(Library& lib, Transport& t)
{
    SyncEngine engine(lib, t, "fake");
    auto r = engine.syncAll();
    for (auto& e : r.errors)
        MESSAGE("sync error: " << e);
    return r;
}

std::string titleOf(Library& lib, int64_t id, const std::string& key)
{
    auto d = lib.item(id, key);
    return d ? jstr(*d, "title") : "<missing>";
}

} // namespace

TEST_CASE("two clients converge through a server")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    auto key = jstr(a.createItem(la, Json{{"itemType", "book"}, {"title", "Original"}}), "key");
    auto coll = jstr(a.createCollection(la, "Reading"), "key");
    a.addToCollection(la, coll, {key});
    a.setSetting(la, "tagColors", Json::array({Json{{"name", "x"}, {"color", "#ff6666"}}}));

    auto r1 = syncWith(a, server);
    CHECK(r1.errors.empty());
    CHECK(r1.pushed == 3);
    CHECK(a.dirtyObjects(la).empty());

    // b starts empty and receives the library
    auto r2 = syncWith(b, server);
    CHECK(r2.errors.empty());
    auto libsB = b.libraries();
    REQUIRE(libsB.size() == 1);
    auto lb = libsB[0].id;
    CHECK(titleOf(b, lb, key) == "Original");
    CHECK(b.collectionItemKeys(lb, coll).count(key));
    CHECK(b.tagColors(lb).at("x") == "#ff6666");
    CHECK(b.dirtyObjects(lb).empty());

    // edit on b, sync both ways
    b.updateItem(lb, key, [](Json& d) { d["title"] = "Edited on B"; });
    syncWith(b, server);
    syncWith(a, server);
    CHECK(titleOf(a, la, key) == "Edited on B");

    // deletion propagates
    b.eraseItems(lb, {key});
    syncWith(b, server);
    auto r3 = syncWith(a, server);
    CHECK(r3.deletedLocally == 1);
    CHECK(!a.item(la, key));
}

TEST_CASE("library names are synced: the side that renamed wins")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    a.createItem(la, Json{{"itemType", "book"}, {"title", "Book"}});
    syncWith(a, server);
    syncWith(b, server);
    auto lb = b.libraries().at(0).id;
    auto uid = a.library(la)->uid;
    auto nameOf = [](Library& l, int64_t id) { return l.library(id)->name; };

    a.renameLibrary(la, "Main");
    syncWith(a, server);
    CHECK(server.libs.at(uid).info.name == "Main");
    syncWith(b, server);
    CHECK(nameOf(b, lb) == "Main");
    syncWith(a, server);
    CHECK(nameOf(a, la) == "Main");

    // renamed on both: the one the target has already wins
    b.renameLibrary(lb, "Papers");
    syncWith(b, server);
    a.renameLibrary(la, "Reading");
    syncWith(a, server);
    CHECK(nameOf(a, la) == "Papers");
    CHECK(server.libs.at(uid).info.name == "Papers");

    // a library shared with us keeps its owner's name
    server.libs.at(uid).info.role = "write";
    b.renameLibrary(lb, "Mine");
    syncWith(b, server);
    CHECK(nameOf(b, lb) == "Papers");
}

TEST_CASE("library names before they were synced: a renamed library keeps its name")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    a.createItem(la, Json{{"itemType", "book"}, {"title", "Book"}});
    syncWith(a, server);
    syncWith(b, server);
    auto lb = b.libraries().at(0).id;
    auto uid = a.library(la)->uid;
    // as left by a version that did not sync names: A renamed it, B still has the default
    a.renameLibrary(la, "Main");
    a.setSyncName(la, "");
    b.setSyncName(lb, "");
    syncWith(b, server);
    CHECK(b.library(lb)->name == Library::kDefaultName);
    syncWith(a, server);
    CHECK(server.libs.at(uid).info.name == "Main");
    syncWith(b, server);
    CHECK(b.library(lb)->name == "Main");
}

TEST_CASE("deleting a library: only empty ones, on the target and then on every device")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    a.createItem(la, Json{{"itemType", "book"}, {"title", "Book"}});
    auto extraA = a.createLibrary("Extra", "group");
    auto keepA = a.createLibrary("Keep", "group");
    syncWith(a, server);
    syncWith(b, server);
    REQUIRE(b.libraries().size() == 3);
    auto uidOf = [](Library& l, int64_t id) { return l.library(id)->uid; };
    auto extraUid = uidOf(a, extraA);
    auto keepUid = uidOf(a, keepA);
    CHECK(a.libraryContents(extraA).empty());
    CHECK(!a.libraryContents(la).empty());
    CHECK(a.libraryContents(la).items == 1);

    // the target refuses a library with entries
    SyncEngine engineA(a, server, "fake");
    CHECK_THROWS_AS(engineA.deleteRemoteLibrary(uidOf(a, la)), SyncError);
    CHECK(server.libs.count(uidOf(a, la)));

    engineA.deleteRemoteLibrary(extraUid);
    a.deleteLibrary(extraA);
    CHECK(!server.libs.count(extraUid));
    engineA.deleteRemoteLibrary(extraUid);  // gone already: done
    auto ra = syncWith(a, server);
    CHECK(ra.errors.empty());
    CHECK(!server.libs.count(extraUid));  // not uploaded again

    // B drops its empty copy; one that got entries here meanwhile is uploaded again
    auto keepB = b.libraryByUid(keepUid)->id;
    b.createItem(keepB, Json{{"itemType", "book"}, {"title", "Written offline"}});
    engineA.deleteRemoteLibrary(keepUid);
    a.deleteLibrary(keepA);
    auto rb = syncWith(b, server);
    CHECK(rb.librariesRemoved == 1);
    CHECK(!b.libraryByUid(extraUid));
    REQUIRE(b.libraryByUid(keepUid));
    REQUIRE(server.libs.count(keepUid));
    syncWith(a, server);
    REQUIRE(a.libraryByUid(keepUid));
    CHECK(a.allItems(a.libraryByUid(keepUid)->id).size() == 1);

    // a server that cannot delete libraries says so
    server.canDelete = false;
    CHECK_THROWS_AS(engineA.deleteRemoteLibrary(uidOf(a, la)), SyncError);
}

TEST_CASE("a fresh install's empty default library is not uploaded next to the user's own")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    a.createItem(la, Json{{"itemType", "book"}, {"title", "Book"}});
    syncWith(a, server);

    b.defaultLibrary();  // "don't import" at first start
    auto group = b.createLibrary("Empty group", "group");
    auto rb = syncWith(b, server);
    CHECK(rb.librariesRemoved == 1);
    auto libs = b.libraries();
    REQUIRE(libs.size() == 2);
    CHECK(b.libraryByUid(a.library(la)->uid));
    CHECK(b.library(group));  // made on purpose: uploaded
    CHECK(server.libs.size() == 2);

    // with nothing on the target, it is the user's library and goes up
    FakeTransport empty;
    Library c(tmp.path / "c");
    c.defaultLibrary();
    syncWith(c, empty);
    CHECK(c.libraries().size() == 1);
    CHECK(empty.libs.size() == 1);
}

TEST_CASE("concurrent edits: last writer wins and the loser is kept as a conflict")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    auto key = jstr(a.createItem(la, Json{{"itemType", "book"}, {"title", "Base"}}), "key");
    syncWith(a, server);
    syncWith(b, server);
    auto lb = b.libraries()[0].id;

    a.updateItem(la, key, [](Json& d) { d["title"] = "From A"; });
    // make B's edit strictly newer
    auto data = *b.item(lb, key);
    data["title"] = "From B";
    b.saveItem(lb, data);
    b.updateItem(lb, key, [](Json& d) { d["dateModified"] = "2999-01-01T00:00:00Z"; });
    {
        // updateItem re-stamps dateModified; force a far-future timestamp directly
        auto d = *b.item(lb, key);
        d["dateModified"] = "2999-01-01T00:00:00Z";
        b.importObject(lb, "item", d);
    }

    syncWith(a, server);
    auto rb = syncWith(b, server);
    CHECK(rb.conflicts == 1);
    CHECK(titleOf(b, lb, key) == "From B");
    syncWith(a, server);
    CHECK(titleOf(a, la, key) == "From B");
    CHECK(b.conflicts(lb).size() == 1);
    CHECK(a.dirtyObjects(la).empty());
    CHECK(b.dirtyObjects(lb).empty());
}

TEST_CASE("edit vs delete keeps the edit")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    auto key = jstr(a.createItem(la, Json{{"itemType", "book"}, {"title", "Base"}}), "key");
    syncWith(a, server);
    syncWith(b, server);
    auto lb = b.libraries()[0].id;

    a.eraseItems(la, {key});
    b.updateItem(lb, key, [](Json& d) { d["title"] = "Still needed"; });
    syncWith(a, server);
    syncWith(b, server);
    syncWith(a, server);
    CHECK(titleOf(a, la, key) == "Still needed");
    CHECK(titleOf(b, lb, key) == "Still needed");
}

TEST_CASE("attachment files are uploaded and downloaded")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    auto src = tmp.path / "doc.pdf";
    std::ofstream(src, std::ios::binary) << "%PDF-1.5 hello";
    auto att = a.importAttachment(la, src);
    auto key = jstr(att, "key");
    auto ra = syncWith(a, server);
    CHECK(ra.uploaded == 1);
    auto rb = syncWith(b, server);
    CHECK(rb.downloaded == 1);
    auto lb = b.libraries()[0].id;
    auto path = b.attachmentFile(lb, *b.item(lb, key));
    CHECK(readFile(path) == "%PDF-1.5 hello");

    // modify on b → uploaded → downloaded by a
    std::ofstream(path, std::ios::binary | std::ios::app) << " v2";
    auto rb2 = syncWith(b, server);
    CHECK(rb2.uploaded == 1);
    auto ra2 = syncWith(a, server);
    CHECK(ra2.downloaded == 1);
    CHECK(readFile(a.attachmentFile(la, *a.item(la, key))) == "%PDF-1.5 hello v2");
    CHECK(jstr(*a.item(la, key), "md5") == md5Hex("%PDF-1.5 hello v2"));
}

TEST_CASE("pagination and large pushes")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    Library b(tmp.path / "b");
    auto la = a.defaultLibrary();
    {
        Library::BatchGuard batch(a);
        sql::Transaction tx(a.db());
        for (int i = 0; i < 2600; ++i)
            a.createItem(la, Json{{"itemType", "book"}, {"title", "Book " + std::to_string(i)}});
        tx.commit();
    }
    auto ra = syncWith(a, server);
    CHECK(ra.pushed == 2600);
    auto rb = syncWith(b, server);
    CHECK(rb.pulled == 2600);
    CHECK(b.itemRows(b.libraries()[0].id).size() == 2600);
}

TEST_CASE("server reset triggers re-upload")
{
    TempDir tmp;
    FakeTransport server;
    Library a(tmp.path / "a");
    auto la = a.defaultLibrary();
    a.createItem(la, Json{{"itemType", "book"}, {"title", "X"}});
    syncWith(a, server);
    server.libs.clear();  // server lost its data
    auto r = syncWith(a, server);
    CHECK(r.errors.empty());
    CHECK(r.pushed == 1);
    CHECK(server.libs.begin()->second.objects.size() == 1);
}
