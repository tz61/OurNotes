// End-to-end: real Library instances synchronising through the real HTTP API and
// through a local folder, using SyncEngine.

#include <doctest.h>
#include <httplib.h>

#include <fstream>
#include <thread>

#include "core/library.h"
#include "core/sync/engine.h"
#include "sync/http_transport.h"
#include "sync/local_transport.h"
#include "sync/server_api.h"
#include "sync/store.h"
#include "sync_test_util.h"

using namespace on;
using namespace on::sync;
using namespace on::test;

namespace {

struct Server {
    TempDir dir;
    Store store;
    httplib::Server http;
    ApiServer api;
    std::thread thread;
    std::string url;

    Server() : store(dir / "server"), api(store)
    {
        api.mount(http);
        int port = http.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        thread = std::thread([this] { http.listen_after_bind(); });
        http.wait_until_ready();
        url = "http://127.0.0.1:" + std::to_string(port);
    }
    ~Server()
    {
        api.stop();
        http.stop();
        thread.join();
    }
};

SyncReport run(Library& lib, Transport& t, const std::string& target)
{
    SyncEngine engine(lib, t, target);
    auto r = engine.syncAll();
    for (auto& e : r.errors)
        MESSAGE("sync error: " << e);
    return r;
}

} // namespace

TEST_CASE("e2e: two clients through ournotesd over HTTP")
{
    Server srv;
    srv.store.createUser("alice", "secret");
    TempDir tmp;
    Library a(tmp / "a"), b(tmp / "b");

    auto la = a.defaultLibrary();
    auto item = a.createItem(la, Json{{"itemType", "journalArticle"}, {"title", "Shared paper"}});
    auto key = jstr(item, "key");
    auto pdf = tmp / "paper.pdf";
    writeBytes(pdf, "%PDF-1.4 " + pseudoRandom(300000, 7));
    auto att = a.importAttachment(la, pdf, key);
    a.createNote(la, "<p>Notes</p>", key);
    auto coll = a.createCollection(la, "Reading list");
    a.addToCollection(la, jstr(coll, "key"), {key});

    auto tokenA = HttpTransport::login(srv.url, "alice", "secret", "laptop");
    auto tokenB = HttpTransport::login(srv.url, "alice", "secret", "desktop");
    HttpTransport ta(srv.url, tokenA), tb(srv.url, tokenB);

    auto r1 = run(a, ta, "server");
    CHECK(r1.errors.empty());
    CHECK(r1.pushed == 4);  // item, attachment, note, collection
    CHECK(r1.uploaded == 1);

    auto r2 = run(b, tb, "server");
    CHECK(r2.errors.empty());
    CHECK(r2.downloaded == 1);
    auto lb = b.libraryByUid(a.library(la)->uid)->id;
    CHECK(jstr(*b.item(lb, key), "title") == "Shared paper");
    auto attB = b.item(lb, jstr(att, "key"));
    REQUIRE(attB);
    CHECK(readFile(b.attachmentFile(lb, *attB)) == readFile(pdf));
    CHECK(b.collectionItemKeys(lb, jstr(coll, "key")).count(key));

    // edits flow back; a second sync with nothing to do transfers nothing
    b.updateItem(lb, key, [](Json& d) { d["title"] = "Edited on desktop"; });
    run(b, tb, "server");
    auto r3 = run(a, ta, "server");
    CHECK(jstr(*a.item(la, key), "title") == "Edited on desktop");
    auto r4 = run(a, ta, "server");
    CHECK(r4.pulled == 0);
    CHECK(r4.pushed == 0);
    CHECK(r4.uploaded == 0);
    CHECK(r4.downloaded == 0);

    // erase propagates, including the file on the server
    a.eraseItems(la, {key});
    run(a, ta, "server");
    run(b, tb, "server");
    CHECK(!b.item(lb, key));
    CHECK(!b.item(lb, jstr(att, "key")));
    CHECK(srv.store.files(a.library(la)->uid).empty());
}

TEST_CASE("e2e: sync through a local folder")
{
    TempDir tmp;
    Library a(tmp / "a"), b(tmp / "b");
    auto la = a.defaultLibrary();
    auto key = jstr(a.createItem(la, Json{{"itemType", "book"}, {"title", "Folder sync"}}), "key");
    {
        LocalTransport t(tmp / "usb-drive" / "ournotes");
        CHECK(run(a, t, "usb").errors.empty());
    }
    {
        LocalTransport t(tmp / "usb-drive" / "ournotes");
        CHECK(run(b, t, "usb").errors.empty());
    }
    auto lb = b.libraries()[0].id;
    CHECK(jstr(*b.item(lb, key), "title") == "Folder sync");
}

TEST_CASE("e2e: switching targets re-uploads everything")
{
    Server srv;
    srv.store.createUser("u", "pw");
    TempDir tmp;
    Library a(tmp / "a");
    auto la = a.defaultLibrary();
    a.createItem(la, Json{{"itemType", "book"}, {"title", "One"}});
    {
        LocalTransport t(tmp / "folder");
        run(a, t, "folder");
    }
    HttpTransport http(srv.url, HttpTransport::login(srv.url, "u", "pw", "x"));
    auto r = run(a, http, "server");
    CHECK(r.errors.empty());
    CHECK(r.pushed == 1);
}
