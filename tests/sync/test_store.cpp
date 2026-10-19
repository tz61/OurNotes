#include <doctest.h>

#include <map>
#include <set>

#include "sync/store.h"
#include "sync_test_util.h"

using namespace on;
using namespace on::sync;
using namespace on::test;

TEST_CASE("store: users, passwords and tokens")
{
    TempDir dir;
    Store store(dir.path);

    store.createUser("alice", "s3cret", true);
    store.createUser("bob.b-2_", "pw");
    CHECK(statusOf([&] { store.createUser("alice", "x"); }) == 409);
    CHECK(statusOf([&] { store.createUser("bad name", "x"); }) == 400);
    CHECK(statusOf([&] { store.createUser("", "x"); }) == 400);
    CHECK(statusOf([&] { store.createUser("carol", ""); }) == 400);
    CHECK(store.userExists("alice"));
    CHECK_FALSE(store.userExists("carol"));
    CHECK(store.isAdmin("alice"));
    CHECK_FALSE(store.isAdmin("bob.b-2_"));

    auto users = store.users();
    REQUIRE(users.size() == 2);
    CHECK(users[0]["username"] == "alice");
    CHECK(users[0]["admin"] == true);

    CHECK(store.checkPassword("alice", "s3cret"));
    CHECK_FALSE(store.checkPassword("alice", "S3cret"));
    CHECK_FALSE(store.checkPassword("nobody", "s3cret"));

    auto token = store.createToken("alice", "laptop");
    CHECK(token.size() == 64);
    CHECK(store.userForToken(token) == "alice");
    CHECK_FALSE(store.userForToken("not-a-token"));
    CHECK_FALSE(store.userForToken(""));
    auto tokens = store.tokens("alice");
    REQUIRE(tokens.size() == 1);
    CHECK(tokens[0]["device"] == "laptop");
    CHECK(tokens[0].dump().find(token) == std::string::npos);  // only a hash is stored
    CHECK(statusOf([&] { store.createToken("nobody", "x"); }) == 404);

    auto second = store.createToken("alice", "phone");
    store.revokeToken(token);
    CHECK_FALSE(store.userForToken(token));
    CHECK(store.userForToken(second) == "alice");

    // A new password revokes all tokens.
    store.setPassword("alice", "n3w");
    CHECK_FALSE(store.checkPassword("alice", "s3cret"));
    CHECK(store.checkPassword("alice", "n3w"));
    CHECK_FALSE(store.userForToken(second));
    CHECK(statusOf([&] { store.setPassword("nobody", "x"); }) == 404);

    // Deleting a user that owns libraries needs an explicit flag.
    auto bobToken = store.createToken("bob.b-2_", "");
    store.ensureLibrary("BOBLIB", "Bob", "user", "bob.b-2_");
    store.ensureLibrary("ALICELIB", "Alice", "user", "alice");
    store.share("ALICELIB", "bob.b-2_", "write");
    CHECK(statusOf([&] { store.deleteUser("bob.b-2_"); }) == 409);
    CHECK(store.userExists("bob.b-2_"));
    store.deleteUser("bob.b-2_", true);
    CHECK_FALSE(store.userExists("bob.b-2_"));
    CHECK_FALSE(store.userForToken(bobToken));
    CHECK(store.allLibraries().size() == 1);
    CHECK(store.members("ALICELIB").empty());
    CHECK(statusOf([&] { store.deleteUser("bob.b-2_"); }) == 404);
}

TEST_CASE("store: library access rules")
{
    TempDir dir;
    Store store(dir.path);
    store.createUser("alice", "a");
    store.createUser("bob", "b");

    auto lib = store.ensureLibrary("LIBA", "Alice's library", "user", "alice");
    CHECK(lib.uid == "LIBA");
    CHECK(lib.role == "owner");
    CHECK(lib.owner == "alice");
    CHECK(lib.version == 0);
    CHECK(store.ensureLibrary("LIBA", "Alice's library", "user", "alice").role == "owner");

    // Existing library without access.
    CHECK(statusOf([&] { store.ensureLibrary("LIBA", "Mine", "user", "bob"); }) == 403);
    CHECK_FALSE(store.library("LIBA", "bob"));
    CHECK(store.role("LIBA", "bob").empty());
    CHECK(store.libraries("bob").empty());

    // Validation.
    CHECK(statusOf([&] { store.ensureLibrary("bad/uid", "x", "user", "alice"); }) == 400);
    CHECK(statusOf([&] { store.ensureLibrary("LIBB", "x", "nonsense", "alice"); }) == 400);
    CHECK(statusOf([&] { store.ensureLibrary("LIBB", "x", "user", "nobody"); }) == 404);
    CHECK(statusOf([&] { store.share("LIBA", "bob", "admin"); }) == 400);
    CHECK(statusOf([&] { store.share("LIBA", "alice", "read"); }) == 400);
    CHECK(statusOf([&] { store.share("LIBA", "nobody", "read"); }) == 404);
    CHECK(statusOf([&] { store.share("NOPE", "bob", "read"); }) == 404);

    // Sharing.
    store.share("LIBA", "bob", "read");
    CHECK(store.role("LIBA", "bob") == "read");
    auto shared = store.library("LIBA", "bob");
    REQUIRE(shared);
    CHECK(shared->role == "read");
    CHECK(shared->owner == "alice");
    auto bobs = store.libraries("bob");
    REQUIRE(bobs.size() == 1);
    CHECK(bobs[0].role == "read");
    // Members get the library but cannot rename it.
    auto viaBob = store.ensureLibrary("LIBA", "Renamed by Bob", "user", "bob");
    CHECK(viaBob.role == "read");
    CHECK(viaBob.name == "Alice's library");
    store.share("LIBA", "bob", "write");
    CHECK(store.role("LIBA", "bob") == "write");
    CHECK(store.members("LIBA").size() == 1);

    // The owner renames through ensureLibrary or renameLibrary.
    CHECK(store.ensureLibrary("LIBA", "Shared", "user", "alice").name == "Shared");
    store.renameLibrary("LIBA", "Shared 2");
    CHECK(store.library("LIBA", "alice")->name == "Shared 2");
    CHECK(statusOf([&] { store.renameLibrary("NOPE", "x"); }) == 404);
    CHECK(store.ensureLibrary("LIBC", "", "group", "alice").name == "LIBC");

    CHECK(store.unshare("LIBA", "bob"));
    CHECK_FALSE(store.unshare("LIBA", "bob"));
    CHECK(store.role("LIBA", "bob").empty());

    store.deleteLibrary("LIBA");
    CHECK_FALSE(store.library("LIBA", "alice"));
    CHECK(statusOf([&] { store.deleteLibrary("LIBA"); }) == 404);
    // The uid is free again.
    CHECK(store.ensureLibrary("LIBA", "Bob's now", "user", "bob").owner == "bob");
}

TEST_CASE("store: deleting a library when asked to keep one with entries")
{
    TempDir dir;
    Store store(dir.path);
    store.createUser("alice", "a");
    store.ensureLibrary("LIB", "Main", "user", "alice");
    store.push("LIB", {put("item", "AAAA1111", 0, item("First")), put("setting", "tagColors", 0, Json::array())});
    CHECK(statusOf([&] { store.deleteLibrary("LIB", true); }) == 409);
    store.push("LIB", {del("item", "AAAA1111", 1)});
    store.deleteLibrary("LIB", true);  // deleted entries and settings do not count
    CHECK_FALSE(store.library("LIB", "alice"));
}

TEST_CASE("store: push and changes round trip")
{
    TempDir dir;
    Store store(dir.path);
    store.createUser("alice", "a");
    store.ensureLibrary("LIB", "Main", "user", "alice");
    CHECK(statusOf([&] { store.push("NOPE", {}); }) == 404);
    CHECK(statusOf([&] { store.changes("NOPE", 0); }) == 404);

    auto r = store.push("LIB", {put("item", "AAAA1111", 0, item("First")), put("item", "BBBB2222", 0, item("Second")),
                                put("collection", "CCCC3333", 0, Json{{"name", "Coll"}})});
    CHECK(r.version == 1);
    REQUIRE(r.results.size() == 3);
    for (auto& e : r.results) {
        CHECK(e.status == "ok");
        CHECK(e.version == 1);
    }
    CHECK(store.library("LIB", "alice")->version == 1);

    auto page = store.changes("LIB", 0);
    CHECK(page.version == 1);
    CHECK(page.next.empty());
    REQUIRE(page.objects.size() == 3);
    // Ordered by (version, type, key).
    CHECK(page.objects[0].type == "collection");
    CHECK(page.objects[1].key == "AAAA1111");
    CHECK(page.objects[1].data["title"] == "First");
    CHECK(page.objects[2].key == "BBBB2222");

    // Update one object: only it shows up as changed.
    r = store.push("LIB", {put("item", "AAAA1111", 1, item("First, edited"))});
    CHECK(r.version == 2);
    CHECK(r.results[0].status == "ok");
    CHECK(r.results[0].version == 2);
    page = store.changes("LIB", 1);
    CHECK(page.version == 2);
    REQUIRE(page.objects.size() == 1);
    CHECK(page.objects[0].data["title"] == "First, edited");
    CHECK(store.changes("LIB", 2).objects.empty());

    // One version bump per batch, however many objects it writes.
    std::vector<PushObject> batch;
    for (int i = 0; i < 5; ++i)
        batch.push_back(put("search", "SRCH000" + std::to_string(i), 0, Json{{"n", i}}));
    r = store.push("LIB", batch);
    CHECK(r.version == 3);
    for (auto& e : r.results)
        CHECK(e.version == 3);
    CHECK(store.library("LIB", "alice")->version == 3);
    CHECK(store.changes("LIB", 2).objects.size() == 5);

    // Data round-trips with its key order.
    Json ordered = Json::object();
    ordered["zeta"] = 1;
    ordered["alpha"] = Json{{"nested", Json::array({1, 2, 3})}};
    store.push("LIB", {put("setting", "SETTING1", 0, ordered)});
    CHECK(store.changes("LIB", 3).objects.at(0).data.dump() == ordered.dump());

    // An empty push changes nothing.
    r = store.push("LIB", {});
    CHECK(r.version == 4);
    CHECK(r.results.empty());
}

TEST_CASE("store: conflicts and invalid objects")
{
    TempDir dir;
    Store store(dir.path);
    store.createUser("alice", "a");
    store.ensureLibrary("LIB", "Main", "user", "alice");

    store.push("LIB", {put("item", "KEY1", 0, item("v1"))});              // version 1
    store.push("LIB", {put("item", "KEY1", 1, item("other device"))});    // version 2
    auto r = store.push("LIB", {put("item", "KEY1", 1, item("stale")), put("item", "KEY2", 0, item("new"))});
    REQUIRE(r.results.size() == 2);
    CHECK(r.results[0].status == "conflict");
    CHECK(r.results[0].version == 2);
    REQUIRE(r.results[0].current);
    CHECK(r.results[0].current->version == 2);
    CHECK(r.results[0].current->data["title"] == "other device");
    CHECK(r.results[1].status == "ok");
    CHECK(r.results[1].version == 3);
    CHECK(r.version == 3);
    CHECK(store.changes("LIB", 0).objects.at(0).data["title"] == "other device");

    // Only conflicts: no version bump.
    r = store.push("LIB", {put("item", "KEY1", 0, item("stale"))});
    CHECK(r.results[0].status == "conflict");
    CHECK(r.version == 3);
    CHECK(store.library("LIB", "alice")->version == 3);

    // Resolving: push again with the current version as base.
    r = store.push("LIB", {put("item", "KEY1", 2, item("merged"))});
    CHECK(r.results[0].status == "ok");
    CHECK(r.version == 4);

    // The same key twice in one batch: the second sees the first.
    r = store.push("LIB", {put("item", "KEY3", 0, item("a")), put("item", "KEY3", 0, item("b"))});
    CHECK(r.results[0].status == "ok");
    CHECK(r.results[1].status == "conflict");
    CHECK(r.results[1].current->data["title"] == "a");

    // Invalid objects are reported per entry; the rest of the batch goes through.
    r = store.push("LIB", {put("widget", "KEY4", 0, item("x")), put("item", "bad key!", 0, item("x")),
                           put("item", "KEY5", 0, Json::array()), put("item", "KEY6", 0, item("fine"))});
    CHECK(r.results[0].status == "error");
    CHECK(r.results[1].status == "error");
    CHECK(r.results[2].status == "error");
    CHECK_FALSE(r.results[2].message.empty());
    CHECK(r.results[3].status == "ok");

    std::vector<PushObject> tooMany(kMaxPushObjects + 1, put("item", "KEY7", 0, item("x")));
    CHECK(statusOf([&] { store.push("LIB", tooMany); }) == 400);
}

TEST_CASE("store: tombstones")
{
    TempDir dir;
    Store store(dir.path);
    store.createUser("alice", "a");
    store.ensureLibrary("LIB", "Main", "user", "alice");
    store.push("LIB", {put("item", "GONE", 0, item("doomed")), put("item", "KEPT", 0, item("kept"))});  // v1

    auto r = store.push("LIB", {del("item", "GONE", 1)});  // v2
    CHECK(r.results[0].status == "ok");
    CHECK(r.version == 2);

    // A full download (since=0) omits tombstones, an incremental one reports them.
    auto full = store.changes("LIB", 0);
    REQUIRE(full.objects.size() == 1);
    CHECK(full.objects[0].key == "KEPT");
    auto inc = store.changes("LIB", 1);
    REQUIRE(inc.objects.size() == 1);
    CHECK(inc.objects[0].key == "GONE");
    CHECK(inc.objects[0].deleted);
    CHECK(inc.objects[0].data.is_null());

    // Deleting again is a no-op that does not bump the version.
    r = store.push("LIB", {del("item", "GONE", 2)});
    CHECK(r.results[0].status == "ok");
    CHECK(r.results[0].version == 2);
    CHECK(r.version == 2);

    // Editing a deleted object with a stale base conflicts with the tombstone.
    r = store.push("LIB", {put("item", "GONE", 1, item("edited offline"))});
    CHECK(r.results[0].status == "conflict");
    REQUIRE(r.results[0].current);
    CHECK(r.results[0].current->deleted);

    // Restoring with the tombstone's version as base works.
    r = store.push("LIB", {put("item", "GONE", 2, item("restored"))});
    CHECK(r.results[0].status == "ok");
    CHECK(store.changes("LIB", 0).objects.size() == 2);
}

TEST_CASE("store: changes pagination")
{
    TempDir dir;
    Store store(dir.path);
    store.createUser("alice", "a");
    store.ensureLibrary("LIB", "Main", "user", "alice");

    // 60 objects over several versions, with some rewritten later.
    std::set<std::string> expected;
    for (int batch = 0; batch < 6; ++batch) {
        std::vector<PushObject> objs;
        for (int i = 0; i < 10; ++i) {
            auto key = "K" + std::to_string(batch) + "X" + std::to_string(i);
            objs.push_back(put(i % 3 ? "item" : "collection", key, 0, Json{{"i", i}}));
            expected.insert(key);
        }
        store.push("LIB", objs);
    }
    auto lastVersion = store.library("LIB", "alice")->version;
    store.push("LIB", {put("item", "K0X1", 1, Json{{"i", 100}}), put("item", "K2X2", 3, Json{{"i", 200}})});

    for (int limit : {1, 7, 10, 59, 60, 61}) {
        std::map<std::string, int> seen;
        std::string cursor;
        int pages = 0;
        int64_t version = 0;
        do {
            auto page = store.changes("LIB", 0, cursor, limit);
            CHECK((int)page.objects.size() <= limit);
            for (auto& o : page.objects)
                ++seen[o.key];
            cursor = page.next;
            version = page.version;
            ++pages;
        } while (!cursor.empty() && pages < 1000);
        CHECK(seen.size() == expected.size());
        for (auto& [key, count] : seen) {
            CHECK(expected.count(key) == 1);
            CHECK(count == 1);
        }
        CHECK(version == lastVersion + 1);
    }

    // Incremental pagination only returns newer objects.
    auto page = store.changes("LIB", lastVersion, "", 1);
    REQUIRE(page.objects.size() == 1);
    CHECK_FALSE(page.next.empty());
    page = store.changes("LIB", lastVersion, page.next, 1);
    REQUIRE(page.objects.size() == 1);
    CHECK(page.next.empty());

    // Changes made between pages show up in a later page.
    auto first = store.changes("LIB", 0, "", 5);
    store.push("LIB", {put(first.objects[0].type, first.objects[0].key, first.objects[0].version, Json{{"i", -1}})});
    bool resent = false;
    for (auto cursor = first.next; !cursor.empty();) {
        auto p = store.changes("LIB", 0, cursor, 5);
        for (auto& o : p.objects)
            resent = resent || (o.key == first.objects[0].key && o.data["i"] == -1);
        cursor = p.next;
    }
    CHECK(resent);

    CHECK(statusOf([&] { store.changes("LIB", 0, "garbage"); }) == 400);
    CHECK(statusOf([&] { store.changes("LIB", 0, "1.item.bad/key"); }) == 400);
    CHECK(store.changes("LIB", 0, "", 0).objects.size() == expected.size());       // 0 = default limit
    CHECK(store.changes("LIB", 0, "", 100000).objects.size() == expected.size());  // clamped
}

TEST_CASE("store: files")
{
    TempDir dir;
    Store store(dir.path);
    store.createUser("alice", "a");
    store.ensureLibrary("LIB", "Main", "user", "alice");
    store.push("LIB", {put("item", "ATTACH1", 0, Json{{"itemType", "attachment"}})});

    auto content = pseudoRandom(100000, 7);
    auto tmp = store.newTempPath();
    writeBytes(tmp, content);
    FileInfo meta;
    meta.md5 = "00000000000000000000000000000000";
    meta.filename = "paper.pdf";
    CHECK(statusOf([&] { store.putFile("LIB", "ATTACH1", tmp, meta); }) == 400);
    CHECK(fs::exists(tmp));  // left for the caller on failure
    CHECK_FALSE(store.file("LIB", "ATTACH1"));

    meta.md5 = md5Hex(content);
    meta.mtime = 1700000000000;
    auto info = store.putFile("LIB", "ATTACH1", tmp, meta);
    CHECK_FALSE(fs::exists(tmp));
    CHECK(info.md5 == md5Hex(content));
    CHECK(info.size == (int64_t)content.size());
    CHECK(info.contentType == "application/pdf");
    CHECK(info.mtime == 1700000000000);
    CHECK(readFile(store.blobPath("LIB", "ATTACH1")) == content);
    REQUIRE(store.files("LIB").size() == 1);
    CHECK(store.file("LIB", "ATTACH1")->filename == "paper.pdf");

    // Replacing a file; md5 may be omitted.
    tmp = store.newTempPath();
    writeBytes(tmp, "second version");
    info = store.putFile("LIB", "ATTACH1", tmp, FileInfo{});
    CHECK(info.md5 == md5Hex("second version"));
    CHECK(info.contentType == "application/octet-stream");
    CHECK(info.mtime > 0);
    CHECK(readFile(store.blobPath("LIB", "ATTACH1")) == "second version");

    // Keys that differ only in case do not collide on case-insensitive filesystems.
    CHECK(store.blobPath("LIB", "abc") != store.blobPath("LIB", "ABC"));
    tmp = store.newTempPath();
    writeBytes(tmp, "lower");
    store.putFile("LIB", "abc", tmp, FileInfo{});
    tmp = store.newTempPath();
    writeBytes(tmp, "upper");
    store.putFile("LIB", "ABC", tmp, FileInfo{});
    CHECK(readFile(store.blobPath("LIB", "abc")) == "lower");
    CHECK(readFile(store.blobPath("LIB", "ABC")) == "upper");
    CHECK(store.blobPath("LIB", "NUL").filename() == ".NUL");  // Windows device names
    CHECK(store.deleteFile("LIB", "abc"));
    CHECK_FALSE(store.deleteFile("LIB", "abc"));
    CHECK_FALSE(fs::exists(store.blobPath("LIB", "abc")));

    // Deleting the item deletes its file; uploading for a deleted item is refused.
    store.push("LIB", {del("item", "ATTACH1", 1)});
    CHECK_FALSE(store.file("LIB", "ATTACH1"));
    CHECK_FALSE(fs::exists(store.blobPath("LIB", "ATTACH1")));
    tmp = store.newTempPath();
    writeBytes(tmp, "late upload");
    CHECK(statusOf([&] { store.putFile("LIB", "ATTACH1", tmp, FileInfo{}); }) == 409);
    CHECK(statusOf([&] { store.putFile("NOPE", "ATTACH2", tmp, FileInfo{}); }) == 404);
    CHECK(statusOf([&] { store.putFile("LIB", "../evil", tmp, FileInfo{}); }) == 400);
    CHECK(statusOf([&] { store.files("NOPE"); }) == 404);

    // Deleting a library removes its blobs.
    auto blob = store.blobPath("LIB", "ABC");
    CHECK(fs::exists(blob));
    store.deleteLibrary("LIB");
    CHECK_FALSE(fs::exists(blob));
}

TEST_CASE("store: data survives reopening")
{
    TempDir dir;
    std::string token;
    {
        Store store(dir.path);
        store.createUser("alice", "a");
        token = store.createToken("alice", "x");
        store.ensureLibrary("LIB", "Main", "user", "alice");
        store.push("LIB", {put("item", "KEY1", 0, item("persisted"))});
    }
    Store store(dir.path);
    CHECK(store.userForToken(token) == "alice");
    CHECK(store.library("LIB", "alice")->version == 1);
    CHECK(store.changes("LIB", 0).objects.at(0).data["title"] == "persisted");
}
