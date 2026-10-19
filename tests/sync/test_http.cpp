#include <doctest.h>

#include <set>
#include <thread>

#include <httplib.h>

#include "ournotes/version.h"
#include "sync/http_transport.h"
#include "sync/server_api.h"
#include "sync_test_util.h"

using namespace on;
using namespace on::sync;
using namespace on::test;

namespace {

#ifdef CPPHTTPLIB_SSL_ENABLED
// Self-signed, valid until 2126, for CN=localhost / IP 127.0.0.1.
const char kTestCert[] = R"(-----BEGIN CERTIFICATE-----
MIIBmjCCAUGgAwIBAgIUbQ+EqKIXNAMIpPTnAyrYd4rtXfMwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAxOTAwNDEzOFoYDzIxMjYwOTI1
MDA0MTM4WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAAS/hr/bpWXc1tJhB4dhdkcBLqmYZ21Kgey4IF6SR3z8EdAe6Zw4MPot
u9UpTgqr63yhNM+ktDhsADiSxkuyaIZGo28wbTAdBgNVHQ4EFgQU5AWCSWfjmY0z
ttLUSdf560kDb1swHwYDVR0jBBgwFoAU5AWCSWfjmY0zttLUSdf560kDb1swDwYD
VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARhwR/AAABgglsb2NhbGhvc3QwCgYIKoZI
zj0EAwIDRwAwRAIgKS3svwAuh/OvSQeSbedKHWHyJz8Nc4faiKviMh3LRSQCIEWp
Jh2ZbMaWPivOY0OWH7EhK8bC87irWWrWR0EYcT3Y
-----END CERTIFICATE-----
)";
const char kTestKey[] = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgm1HVpTJUnYoEuwv4
a1BzNZ8inZwo5MfhNI2KA7Qby2mhRANCAAS/hr/bpWXc1tJhB4dhdkcBLqmYZ21K
gey4IF6SR3z8EdAe6Zw4MPotu9UpTgqr63yhNM+ktDhsADiSxkuyaIZG
-----END PRIVATE KEY-----
)";
#endif

std::unique_ptr<httplib::Server> makeServer(bool tls)
{
#ifdef CPPHTTPLIB_SSL_ENABLED
    if (tls) {
        httplib::SSLServer::PemMemory pem{kTestCert, sizeof kTestCert - 1, kTestKey, sizeof kTestKey - 1,
                                          nullptr,   0,                    nullptr};
        return std::make_unique<httplib::SSLServer>(pem);
    }
#endif
    (void)tls;
    return std::make_unique<httplib::Server>();
}

// An in-process ournotesd on 127.0.0.1 with a random port.
struct TestServer {
    TempDir dir;
    Store store;
    std::unique_ptr<httplib::Server> http;
    ApiServer api;
    std::thread thread;
    std::string url;

    explicit TestServer(ServerOptions opts = {}, bool tls = false)
        : store(dir / "data"), http(makeServer(tls)), api(store, std::move(opts))
    {
        REQUIRE(http->is_valid());
        api.mount(*http);
        int port = http->bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread([this] { http->listen_after_bind(); });
        http->wait_until_ready();
        url = std::string(tls ? "https" : "http") + "://127.0.0.1:" + std::to_string(port);
    }
    ~TestServer()
    {
        http->stop();
        thread.join();
    }
};

Json parseBody(const httplib::Result& res)
{
    REQUIRE(res);
    return Json::parse(res->body, nullptr, false);
}

} // namespace

TEST_CASE("http: full round trip")
{
    TestServer srv;
    srv.store.createUser("alice", "alice-pw");
    srv.store.createUser("bob", "bob-pw");

    // Unauthenticated access.
    HttpTransport anon(srv.url, "");
    auto info = anon.info();
    CHECK(info["server"] == "ournotesd");
    CHECK(info["api"] == kApiVersion);
    CHECK(info["version"] == OURNOTES_VERSION);
    CHECK(statusOf([&] { anon.libraries(); }) == 401);
    CHECK(statusOf([&] { HttpTransport(srv.url, "bogus-token").libraries(); }) == 401);
    CHECK(statusOf([&] { HttpTransport::login(srv.url, "alice", "wrong", "test"); }) == 401);
    CHECK(statusOf([&] { HttpTransport::login(srv.url, "nobody", "x", "test"); }) == 401);

    // Login (a trailing slash in the base URL is fine).
    auto token = HttpTransport::login(srv.url + "/", "alice", "alice-pw", "laptop");
    CHECK(srv.store.userForToken(token) == "alice");
    CHECK(srv.store.tokens("alice").at(0)["device"] == "laptop");
    HttpTransport alice(srv.url, token);
    CHECK(alice.me()["username"] == "alice");
    CHECK(alice.libraries().empty());

    // Libraries.
    auto lib = alice.ensureLibrary("LIB1", "Main", "user");
    CHECK(lib.uid == "LIB1");
    CHECK(lib.role == "owner");
    CHECK(lib.owner == "alice");
    REQUIRE(alice.libraries().size() == 1);
    CHECK(statusOf([&] { alice.changes("NOSUCHLIB", 0, "", 0); }) == 404);

    // Push and changes.
    auto r = alice.push("LIB1", {put("item", "ITEM0001", 0, item("Paper")), put("item", "ATTACH01", 0, item("PDF")),
                                 put("collection", "COLL0001", 0, Json{{"name", "Reading"}})});
    CHECK(r.version == 1);
    REQUIRE(r.results.size() == 3);
    for (auto& e : r.results)
        CHECK(e.status == "ok");
    auto page = alice.changes("LIB1", 0, "", 0);
    CHECK(page.version == 1);
    CHECK(page.objects.size() == 3);
    CHECK(page.next.empty());

    std::set<std::string> keys;
    int pages = 0;
    std::string cursor;
    do {
        auto p = alice.changes("LIB1", 0, cursor, 2);
        for (auto& o : p.objects)
            keys.insert(o.key);
        cursor = p.next;
        ++pages;
    } while (!cursor.empty() && pages < 10);
    CHECK(keys.size() == 3);
    CHECK(pages == 2);

    r = alice.push("LIB1", {put("item", "ITEM0001", 1, item("Paper, edited"))});
    CHECK(r.results.at(0).status == "ok");
    CHECK(r.version == 2);
    r = alice.push("LIB1", {put("item", "ITEM0001", 1, item("Paper, stale edit"))});
    REQUIRE(r.results.at(0).status == "conflict");
    REQUIRE(r.results[0].current);
    CHECK(r.results[0].current->data["title"] == "Paper, edited");
    CHECK(r.results[0].current->version == 2);

    // File upload and download (multi-MB, non-ASCII name).
    auto content = pseudoRandom(5 * 1024 * 1024 + 123, 99);
    auto src = srv.dir / "R\xC3\xA9sum\xC3\xA9 draft #1.pdf";
    writeBytes(src, content);
    int64_t upDone = 0;
    auto up = alice.upload("LIB1", "ATTACH01", src, FileInfo{}, [&](int64_t done, int64_t) {
        upDone = done;
        return true;
    });
    CHECK(upDone == (int64_t)content.size());
    CHECK(up.md5 == md5Hex(content));
    CHECK(up.size == (int64_t)content.size());
    CHECK(up.filename == "R\xC3\xA9sum\xC3\xA9 draft #1.pdf");
    CHECK(up.contentType == "application/pdf");
    REQUIRE(alice.files("LIB1").size() == 1);

    auto dest = srv.dir / "downloads" / "copy.pdf";
    int64_t downDone = 0, downTotal = 0;
    auto down = alice.download("LIB1", "ATTACH01", dest, [&](int64_t done, int64_t total) {
        downDone = done;
        downTotal = total;
        return true;
    });
    CHECK(downDone == (int64_t)content.size());
    CHECK(downTotal == (int64_t)content.size());
    CHECK(down.md5 == up.md5);
    CHECK(down.size == up.size);
    CHECK(down.filename == up.filename);
    CHECK(down.contentType == "application/pdf");
    CHECK(md5File(dest) == md5Hex(content));
    CHECK(readFile(dest) == content);

    auto missing = srv.dir / "downloads" / "missing.pdf";
    CHECK(statusOf([&] { alice.download("LIB1", "NOFILE01", missing); }) == 404);
    CHECK_FALSE(fs::exists(missing));
    CHECK_FALSE(fs::exists(fs::path(missing).concat(".part")));
    CHECK(statusOf([&] { alice.download("LIB1", "ATTACH01", missing, [](int64_t, int64_t) { return false; }); }) == 0);
    CHECK_FALSE(fs::exists(missing));
    FileInfo wrongMd5;
    wrongMd5.md5 = md5Hex("not the content");
    CHECK(statusOf([&] { alice.upload("LIB1", "ATTACH02", src, wrongMd5); }) == 400);
    CHECK(alice.files("LIB1").size() == 1);

    // A read-only member can read but not write.
    srv.store.share("LIB1", "bob", "read");
    HttpTransport bob(srv.url, HttpTransport::login(srv.url, "bob", "bob-pw", "phone"));
    auto bobLibs = bob.libraries();
    REQUIRE(bobLibs.size() == 1);
    CHECK(bobLibs[0].role == "read");
    CHECK(bobLibs[0].owner == "alice");
    CHECK(bob.changes("LIB1", 0, "", 0).objects.size() == 3);
    CHECK(statusOf([&] { bob.push("LIB1", {put("item", "ITEM0002", 0, item("Bob's"))}); }) == 403);
    CHECK(statusOf([&] { bob.upload("LIB1", "ATTACH01", src, FileInfo{}); }) == 403);
    auto bobCopy = srv.dir / "bob.pdf";
    CHECK(bob.download("LIB1", "ATTACH01", bobCopy).md5 == up.md5);

    // Libraries without access look like missing ones; claiming them is refused.
    alice.ensureLibrary("PRIVATE1", "Alice only", "user");
    CHECK(statusOf([&] { bob.changes("PRIVATE1", 0, "", 0); }) == 404);
    CHECK(statusOf([&] { bob.files("PRIVATE1"); }) == 404);
    CHECK(statusOf([&] { bob.ensureLibrary("PRIVATE1", "Mine now", "user"); }) == 403);

    // Deleting the attachment item deletes its file.
    r = alice.push("LIB1", {del("item", "ATTACH01", 1)});
    CHECK(r.results.at(0).status == "ok");
    CHECK(alice.files("LIB1").empty());

    // Logout revokes the token.
    alice.logout();
    CHECK(statusOf([&] { alice.libraries(); }) == 401);
    CHECK_FALSE(srv.store.userForToken(token));
}

TEST_CASE("http: raw API behaviour, limits and login throttling")
{
    ServerOptions opts;
    opts.maxUploadBytes = 1 << 20;
    TestServer srv(opts);
    srv.store.createUser("alice", "pw");

    httplib::Client cli(srv.url);
    auto res = cli.Get("/api/v1/libraries");
    REQUIRE(res);
    CHECK(res->status == 401);
    CHECK(res->get_header_value("Content-Type") == "application/json");
    CHECK(parseBody(res)["error"] == "authentication required");

    res = cli.Get("/api/v1/no/such/route");
    REQUIRE(res);
    CHECK(res->status == 404);
    CHECK(parseBody(res).contains("error"));

    res = cli.Post("/api/v1/login", "not json", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);

    auto token = HttpTransport::login(srv.url, "alice", "pw", "");
    CHECK(srv.store.tokens("alice").size() == 1);
    HttpTransport alice(srv.url, token);
    alice.ensureLibrary("LIB1", "Main", "user");

    // Base URLs: a full API URL is tolerated; other schemes and bare hosts are not.
    CHECK(HttpTransport(srv.url + "/api/v1/", "").info()["api"] == kApiVersion);
    CHECK(statusOf([&] { HttpTransport("ftp://127.0.0.1", ""); }) == 400);
    CHECK(statusOf([&] { HttpTransport("127.0.0.1:8642", ""); }) == 400);
    CHECK(statusOf([&] { HttpTransport("http://127.0.0.1:1/ournotes", "").info(); }) == 0);  // nothing listens

    httplib::Headers auth{{"Authorization", "Bearer " + token}};
    res = cli.Get("/api/v1/libraries/LIB1/changes?since=abc", auth);
    REQUIRE(res);
    CHECK(res->status == 400);
    res = cli.Get("/api/v1/libraries/LIB1", auth);
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(parseBody(res)["role"] == "owner");
    res = cli.Post("/api/v1/libraries/LIB1/push", auth, R"({"objects": 5})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);
    res = cli.Delete("/api/v1/libraries/LIB1/files/NOFILE01", auth);
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(parseBody(res)["deleted"] == false);

    // A file name that is not valid UTF-8 does not break the file list.
    cli.set_path_encode(false);
    res = cli.Put("/api/v1/libraries/LIB1/files/BADNAME1?filename=%FF%FEx.pdf", auth, "data", "text/plain");
    REQUIRE(res);
    CHECK(res->status == 200);
    res = cli.Get("/api/v1/libraries/LIB1/files", auth);
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(parseBody(res)["files"].size() == 1);
    CHECK(alice.files("LIB1").size() == 1);

    // Uploads above the limit are refused before the body is sent.
    auto big = srv.dir / "big.bin";
    writeBytes(big, pseudoRandom(2 << 20, 5));
    CHECK(statusOf([&] { alice.upload("LIB1", "BIGFILE1", big, FileInfo{}); }) == 413);
    auto small = srv.dir / "small.bin";
    writeBytes(small, pseudoRandom(1000, 6));
    CHECK(alice.upload("LIB1", "SMALLFI1", small, FileInfo{}).size == 1000);
    auto empty = srv.dir / "empty.txt";
    writeBytes(empty, "");
    CHECK(alice.upload("LIB1", "EMPTYFI1", empty, FileInfo{}).size == 0);
    auto emptyCopy = srv.dir / "empty-copy.txt";
    CHECK(alice.download("LIB1", "EMPTYFI1", emptyCopy).md5 == md5Hex(""));
    CHECK(fs::exists(emptyCopy));
    CHECK(fs::file_size(emptyCopy) == 0);

    // Repeated failed logins from one address are throttled, even with the right password.
    for (int i = 0; i < 10; ++i)
        CHECK(statusOf([&] { HttpTransport::login(srv.url, "alice", "wrong", ""); }) == 401);
    CHECK(statusOf([&] { HttpTransport::login(srv.url, "alice", "pw", ""); }) == 429);
    // Existing tokens keep working.
    CHECK(alice.libraries().size() == 1);
}

#ifdef CPPHTTPLIB_SSL_ENABLED
TEST_CASE("http: TLS")
{
    TestServer srv({}, true);
    srv.store.createUser("alice", "pw");

    // The self-signed certificate is rejected unless verification is turned off.
    CHECK(statusOf([&] { HttpTransport(srv.url, "").info(); }) == 0);
    CHECK(HttpTransport(srv.url, "", false).info()["server"] == "ournotesd");

    auto token = HttpTransport::login(srv.url, "alice", "pw", "tls test", false);
    HttpTransport alice(srv.url, token, false);
    alice.ensureLibrary("LIB1", "Main", "user");
    alice.push("LIB1", {put("item", "ATTACH01", 0, item("PDF"))});
    auto content = pseudoRandom(1500000, 3);
    auto src = srv.dir / "tls.bin";
    writeBytes(src, content);
    auto up = alice.upload("LIB1", "ATTACH01", src, FileInfo{});
    auto dest = srv.dir / "tls-copy.bin";
    CHECK(alice.download("LIB1", "ATTACH01", dest).md5 == up.md5);
    CHECK(readFile(dest) == content);
}
#else
TEST_CASE("http: https URLs need TLS support")
{
    CHECK_THROWS_WITH_AS(HttpTransport("https://example.org", ""), "HTTPS support is not compiled in", SyncError);
}
#endif
