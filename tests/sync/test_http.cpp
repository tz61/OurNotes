#include <doctest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <httplib.h>

#include "ournotes/version.h"
#include "sync/change_watcher.h"
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
        api.stop();
        http->stop();
        thread.join();
    }
};

// Polls cond for up to five seconds.
bool waitUntil(const std::function<bool()>& cond)
{
    for (int i = 0; i < 100; ++i) {
        if (cond())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return cond();
}

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

    // Only the owner deletes a library, and only an empty one.
    CHECK(statusOf([&] { bob.deleteLibrary("LIB1"); }) == 403);
    CHECK(statusOf([&] { alice.deleteLibrary("LIB1"); }) == 409);
    CHECK(statusOf([&] { bob.deleteLibrary("PRIVATE1"); }) == 404);
    alice.deleteLibrary("PRIVATE1");
    CHECK(statusOf([&] { alice.deleteLibrary("PRIVATE1"); }) == 404);
    CHECK(statusOf([&] { alice.changes("PRIVATE1", 0, "", 0); }) == 404);

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

TEST_CASE("http: a library list held until another client pushes")
{
    using namespace std::chrono;
    TestServer srv;
    srv.store.createUser("alice", "alice-pw");
    auto token = HttpTransport::login(srv.url, "alice", "alice-pw", "laptop");
    HttpTransport mine(srv.url, token), other(srv.url, token);
    mine.ensureLibrary("LIB1", "Mine", "user");
    auto now = mine.awaitLibraries({}, 0);
    REQUIRE(now);
    REQUIRE(now->libraries.size() == 1);
    CHECK(now->libraries[0].uid == "LIB1");
    CHECK(mine.awaitLibraries({}, 0)->state == now->state);

    // unchanged: answered when the wait is over
    auto start = steady_clock::now();
    CHECK(mine.awaitLibraries(now->state, 1)->state == now->state);
    CHECK(steady_clock::now() - start >= milliseconds(900));

    // another client pushes, renames, adds a library: answered at once
    auto awaitWhile = [&](const std::function<void()>& change) {
        std::thread t([&] {
            std::this_thread::sleep_for(milliseconds(300));
            change();
        });
        auto start = steady_clock::now();
        auto next = mine.awaitLibraries(now->state, 30);
        t.join();
        CHECK(steady_clock::now() - start < seconds(5));
        REQUIRE(next);
        CHECK(next->state != now->state);
        now = next;
    };
    awaitWhile([&] { other.push("LIB1", {put("item", "ITEM2345", 0, item("New"))}); });
    CHECK(now->libraries[0].version == 1);
    awaitWhile([&] { other.ensureLibrary("LIB1", "Renamed", "user"); });
    CHECK(now->libraries[0].name == "Renamed");
    awaitWhile([&] { other.ensureLibrary("LIB2", "Second", "user"); });
    CHECK(now->libraries.size() == 2);

    // ended from another thread at once, and later calls fail
    std::thread aborter([&] {
        std::this_thread::sleep_for(milliseconds(300));
        mine.abort();
    });
    start = steady_clock::now();
    CHECK(statusOf([&] { mine.awaitLibraries(now->state, 30); }) == 0);
    aborter.join();
    CHECK(steady_clock::now() - start < seconds(5));
    CHECK(statusOf([&] { mine.awaitLibraries({}, 0); }) == 0);
}

TEST_CASE("http: a server stops at once with requests held")
{
    using namespace std::chrono;
    auto srv = std::make_unique<TestServer>();
    srv->store.createUser("alice", "alice-pw");
    auto token = HttpTransport::login(srv->url, "alice", "alice-pw", "laptop");
    HttpTransport t(srv->url, token);
    t.ensureLibrary("LIB1", "Mine", "user");
    auto state = t.awaitLibraries({}, 0)->state;
    std::thread waiting([&] {
        try {
            t.awaitLibraries(state, 60);
        } catch (const SyncError&) {
        }
    });
    std::this_thread::sleep_for(milliseconds(300));
    auto start = steady_clock::now();
    srv.reset();
    CHECK(steady_clock::now() - start < seconds(5));
    waiting.join();
}

TEST_CASE("http: a server before 0.1.10 cannot hold the list")
{
    httplib::Server old;  // its GET /libraries, without a state
    old.Get("/api/v1/libraries", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"libraries": []})", "application/json");
    });
    int port = old.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { old.listen_after_bind(); });
    old.wait_until_ready();
    auto url = "http://127.0.0.1:" + std::to_string(port);
    CHECK(!HttpTransport(url, "token").awaitLibraries({}, 0));

    std::atomic<int> changed{0};
    {
        ChangeWatcher watcher({[&] { return std::make_unique<HttpTransport>(url, "token"); },
                               [] { return std::map<std::string, int64_t>(); }, [] { return uint64_t(0); },
                               [&] { ++changed; }});
        CHECK(waitUntil([&] { return watcher.unsupported(); }));
    }  // (and stops at once, though waiting a long time to ask again)
    CHECK(changed == 0);
    old.stop();
    thread.join();
}

TEST_CASE("change watcher: another client's push is told at once, this one's own are not")
{
    using namespace std::chrono;
    TestServer srv;
    srv.store.createUser("alice", "alice-pw");
    auto token = HttpTransport::login(srv.url, "alice", "alice-pw", "laptop");
    HttpTransport other(srv.url, token), mine(srv.url, token);
    other.ensureLibrary("LIB1", "Mine", "user");
    std::mutex mutex;
    std::map<std::string, int64_t> pulled{{"LIB1", 0}};
    std::atomic<uint64_t> syncs{0};
    std::atomic<int> changed{0};
    auto pull = [&](int64_t version) {
        std::lock_guard lock(mutex);
        pulled["LIB1"] = version;
    };
    ChangeWatcher watcher({[&] { return std::make_unique<HttpTransport>(srv.url, token); },
                           [&] {
                               std::lock_guard lock(mutex);
                               return pulled;
                           },
                           [&] { return syncs.load(); }, [&] { ++changed; }});
    std::this_thread::sleep_for(milliseconds(500));
    CHECK(changed == 0);  // nothing new there

    // another computer pushes: told at once; this one syncs
    other.push("LIB1", {put("item", "ITEM2345", 0, item("There"))});
    CHECK(waitUntil([&] { return changed == 1; }));
    ++syncs;
    pull(1);
    ++syncs;

    // this one pushes, in its sync: not told
    ++syncs;
    pull(mine.push("LIB1", {put("item", "ITEM3456", 0, item("Here"))}).version);
    ++syncs;
    std::this_thread::sleep_for(seconds(1));
    CHECK(changed == 1);

    // another computer pushes while this one syncs (and does not pull it): told after
    ++syncs;
    other.push("LIB1", {put("item", "ITEM4567", 0, item("Meanwhile"))});
    std::this_thread::sleep_for(milliseconds(500));
    CHECK(changed == 1);
    ++syncs;
    CHECK(waitUntil([&] { return changed == 2; }));
    // that sync fails (pulls nothing): not asked for again until something else changes
    ++syncs;
    ++syncs;
    std::this_thread::sleep_for(seconds(1));
    CHECK(changed == 2);
    other.push("LIB1", {put("item", "ITEM5678", 0, item("Later"))});
    CHECK(waitUntil([&] { return changed == 3; }));
}

TEST_CASE("change watcher: a target failing at once is asked later and later, one that held the request a while soon again")
{
    using namespace std::chrono;
    // Each tells its libraries' state at once, and fails a request held for a change: one at
    // once (as a broken proxy might), one after holding it (a proxy's timeout, a laptop asleep).
    struct Target {
        httplib::Server http;
        std::thread thread;
        std::string url;
        std::mutex mutex;
        std::vector<steady_clock::time_point> tries;  // when each try's first request came
        explicit Target(milliseconds held)
        {
            http.Get("/api/v1/libraries", [this, held](const httplib::Request& req, httplib::Response& res) {
                if (!req.has_param("state")) {
                    std::lock_guard lock(mutex);
                    tries.push_back(steady_clock::now());
                    res.set_content(R"({"libraries": [], "state": "s1"})", "application/json");
                    return;
                }
                std::this_thread::sleep_for(held);
                res.status = 502;
            });
            int port = http.bind_to_any_port("127.0.0.1");
            thread = std::thread([this] { http.listen_after_bind(); });
            http.wait_until_ready();
            url = "http://127.0.0.1:" + std::to_string(port);
        }
        ~Target()
        {
            http.stop();
            thread.join();
        }
        size_t count()
        {
            std::lock_guard lock(mutex);
            return tries.size();
        }
        double secondsBefore(size_t n)  // between try n - 1 and try n
        {
            std::lock_guard lock(mutex);
            return duration<double>(tries[n] - tries[n - 1]).count();
        }
    };
    Target atOnce(milliseconds(0)), held(milliseconds(1200));
    ChangeWatcher::Timing timing;
    timing.waitSeconds = 2;  // so a request held over a second was held a while
    timing.retrySeconds = 1;
    timing.maxRetrySeconds = 4;
    auto hooks = [](Target& target) {
        return ChangeWatcher::Hooks{[&target] { return std::make_unique<HttpTransport>(target.url, "token"); },
                                    [] { return std::map<std::string, int64_t>(); }, [] { return uint64_t(0); },
                                    [] {}};
    };
    ChangeWatcher watchingAtOnce(hooks(atOnce), timing), watchingHeld(hooks(held), timing);
    for (int i = 0; i < 200 && (atOnce.count() < 3 || held.count() < 3); ++i)
        std::this_thread::sleep_for(milliseconds(50));
    REQUIRE(atOnce.count() >= 3);
    REQUIRE(held.count() >= 3);
    // a second, then two
    CHECK(atOnce.secondsBefore(1) < 1.9);
    CHECK(atOnce.secondsBefore(2) >= 1.9);
    // held 1.2 s, then a second again (not two)
    CHECK(held.secondsBefore(2) < 2.9);
}

TEST_CASE("http: the server's status, for the user's libraries")
{
    TestServer srv;
    srv.store.createUser("alice", "alice-pw");
    srv.store.createUser("bob", "bob-pw");
    auto token = HttpTransport::login(srv.url, "alice", "alice-pw", "laptop");
    auto bobToken = HttpTransport::login(srv.url, "bob", "bob-pw", "phone");
    HttpTransport alice(srv.url, token), bob(srv.url, bobToken);
    alice.ensureLibrary("LIB1", "Mine", "user");
    bob.ensureLibrary("LIB2", "Bob's", "user");
    alice.push("LIB1", {put("item", "ITEM2345", 0, item("A book")),
                        put("item", "NOTE2345", 0, Json{{"itemType", "note"}, {"note", "<p>n</p>"}}),
                        put("item", "ATTA2345", 0, Json{{"itemType", "attachment"}, {"linkMode", "imported_file"}})});
    TempDir tmp;
    writeBytes(tmp / "a.pdf", std::string(3000, 'p'));
    FileInfo meta;
    meta.filename = "a.pdf";
    alice.upload("LIB1", "ATTA2345", tmp / "a.pdf", meta);
    auto s = alice.status();
    REQUIRE(s);
    CAPTURE(s->dump(2));
    CHECK((*s)["server"] == "ournotesd");
    CHECK((*s)["version"] == OURNOTES_VERSION);
    CHECK((*s)["api"] == kApiVersion);
    CHECK((*s)["uptime"].get<int64_t>() >= 0);
    CHECK((*s)["disk"]["capacity"].get<int64_t>() > 0);
    CHECK((*s)["database"].get<int64_t>() > 0);
    CHECK((*s)["files"]["count"] == 1);
    REQUIRE((*s)["libraries"].size() == 1);  // hers only
    auto& l = (*s)["libraries"][0];
    CHECK(l["uid"] == "LIB1");
    CHECK(l["role"] == "owner");
    CHECK(l["items"] == 1);
    CHECK(l["notes"] == 1);
    CHECK(l["attachments"] == 1);
    CHECK(l["files"] == 1);
    CHECK(l["fileBytes"] == 3000);
    CHECK(statusOf([&] { HttpTransport(srv.url, "").status(); }) == 401);

    // asked again: as it is now (what is kept, kept for a library's version)
    auto version = alice.push("LIB1", {put("item", "NOTE3456", 0, Json{{"itemType", "note"}, {"note", "<p>m</p>"}})})
                       .version;
    CHECK((*alice.status())["libraries"][0]["notes"] == 2);
    alice.push("LIB1", {del("item", "ITEM2345", version - 1)});
    auto again = (*alice.status())["libraries"][0];
    CHECK(again["items"] == 0);
    CHECK(again["notes"] == 2);
    CHECK(again["attachments"] == 1);
}

TEST_CASE("http: a server before 0.1.11 does not tell its status")
{
    httplib::Server old;
    old.Get("/api/v1/info", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"server": "ournotesd", "version": "0.1.10", "api": 1})", "application/json");
    });
    int port = old.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { old.listen_after_bind(); });
    old.wait_until_ready();
    HttpTransport t("http://127.0.0.1:" + std::to_string(port), "token");
    CHECK(!t.status());
    CHECK(t.info()["version"] == "0.1.10");
    old.stop();
    thread.join();
}
