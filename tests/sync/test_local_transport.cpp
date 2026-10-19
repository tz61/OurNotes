#include <doctest.h>

#include "sync/local_transport.h"
#include "sync_test_util.h"

using namespace on;
using namespace on::sync;
using namespace on::test;

TEST_CASE("local transport: two devices sharing a folder")
{
    TempDir dir;
    auto folder = dir / "sync folder";
    LocalTransport a(folder);
    CHECK(a.info()["server"] == "local");
    CHECK(a.info()["api"] == kApiVersion);
    CHECK(a.libraries().empty());

    auto lib = a.ensureLibrary("LIBRARY1", "My Library", "user");
    CHECK(lib.role == "owner");
    CHECK(lib.version == 0);

    auto r = a.push("LIBRARY1", {put("item", "ITEMKEY1", 0, item("Paper")), put("item", "ATTACHM1", 0, item("PDF"))});
    CHECK(r.version == 1);
    CHECK(r.results.at(0).status == "ok");

    // Upload a file with progress reporting.
    auto content = pseudoRandom(3 * 1024 * 1024 + 17, 42);
    auto src = dir / "Paper (2024).pdf";
    writeBytes(src, content);
    int64_t lastDone = -1, lastTotal = -1;
    auto info = a.upload("LIBRARY1", "ATTACHM1", src, FileInfo{}, [&](int64_t done, int64_t total) {
        CHECK(done >= lastDone);
        lastDone = done;
        lastTotal = total;
        return true;
    });
    CHECK(lastDone == (int64_t)content.size());
    CHECK(lastTotal == (int64_t)content.size());
    CHECK(info.md5 == md5Hex(content));
    CHECK(info.filename == "Paper (2024).pdf");
    CHECK(info.contentType == "application/pdf");
    CHECK(fs::exists(src));  // the source is copied, not moved

    // A second device opens the same folder.
    LocalTransport b(folder);
    auto libs = b.libraries();
    REQUIRE(libs.size() == 1);
    CHECK(libs[0].name == "My Library");
    CHECK(libs[0].version == 1);
    auto page = b.changes("LIBRARY1", 0, "", 0);
    CHECK(page.objects.size() == 2);
    auto files = b.files("LIBRARY1");
    REQUIRE(files.size() == 1);
    CHECK(files[0].key == "ATTACHM1");

    auto dest = dir / "device b" / "downloaded.pdf";
    auto got = b.download("LIBRARY1", "ATTACHM1", dest);
    CHECK(got.md5 == info.md5);
    CHECK(readFile(dest) == content);
    CHECK_FALSE(fs::exists(fs::path(dest).concat(".part")));

    // Device b edits; device a sees it and its stale edit conflicts.
    b.push("LIBRARY1", {put("item", "ITEMKEY1", 1, item("Paper, edited on b"))});
    auto inc = a.changes("LIBRARY1", 1, "", 0);
    REQUIRE(inc.objects.size() == 1);
    CHECK(inc.objects[0].data["title"] == "Paper, edited on b");
    r = a.push("LIBRARY1", {put("item", "ITEMKEY1", 1, item("Paper, edited on a"))});
    CHECK(r.results.at(0).status == "conflict");

    // Cancelled transfers leave nothing behind.
    auto cancelled = dir / "cancelled.pdf";
    CHECK_THROWS_AS(a.download("LIBRARY1", "ATTACHM1", cancelled, [](int64_t, int64_t) { return false; }), SyncError);
    CHECK_FALSE(fs::exists(cancelled));
    CHECK_FALSE(fs::exists(fs::path(cancelled).concat(".part")));
    CHECK_THROWS_AS(a.upload("LIBRARY1", "ATTACHM2", src, FileInfo{}, [](int64_t, int64_t) { return false; }),
                    SyncError);
    CHECK(a.files("LIBRARY1").size() == 1);
    CHECK(fs::is_empty(folder / "tmp"));

    // Errors.
    try {
        a.changes("UNKNOWN1", 0, "", 0);
        FAIL("expected an error");
    } catch (const SyncError& e) {
        CHECK(e.status == 404);
    }
    try {
        a.download("LIBRARY1", "NOFILE01", dir / "x");
        FAIL("expected an error");
    } catch (const SyncError& e) {
        CHECK(e.status == 404);
    }
    FileInfo wrong;
    wrong.md5 = md5Hex("something else");
    try {
        a.upload("LIBRARY1", "ATTACHM2", src, wrong);
        FAIL("expected an error");
    } catch (const SyncError& e) {
        CHECK(e.status == 400);
    }

    // Libraries shared read-only (e.g. by an administrator) cannot be written.
    a.store().createUser("other", "pw");
    a.store().ensureLibrary("FOREIGN1", "Foreign", "user", "other");
    a.store().share("FOREIGN1", LocalTransport::kUser, "read");
    CHECK(a.libraries().size() == 2);
    try {
        a.push("FOREIGN1", {put("item", "ITEMKEY9", 0, item("x"))});
        FAIL("expected an error");
    } catch (const SyncError& e) {
        CHECK(e.status == 403);
    }
    CHECK(statusOf([&] { a.deleteLibrary("FOREIGN1"); }) == 403);
    a.ensureLibrary("EMPTY001", "Empty", "group");
    a.deleteLibrary("EMPTY001");
    CHECK(a.libraries().size() == 2);
    CHECK(statusOf([&] { a.deleteLibrary("LIBRARY1"); }) == 409);
}
