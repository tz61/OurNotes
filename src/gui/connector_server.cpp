#include "gui/connector_server.h"

#include <QMetaObject>
#include <QUrl>

#include <httplib.h>

#include <chrono>
#include <fstream>
#include <future>

#include "core/hash.h"
#include "core/library.h"
#include "core/schema.h"
#include "formats/metadata.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "ournotes/version.h"

namespace on::gui {

namespace {

constexpr size_t kMaxJson = 4u << 20;      // an item, with a long abstract
constexpr size_t kMaxPdf = 400u << 20;     // a PDF
constexpr size_t kMaxField = 20000;        // characters kept of a field (abstracts: more)
constexpr const char* kHeader = "X-OurNotes-Connector";

struct Refused : std::runtime_error {
    int status;
    Refused(const std::string& msg, int s) : std::runtime_error(msg), status(s) {}
};

bool isExtensionOrigin(const std::string& origin)
{
    for (const char* scheme : {"chrome-extension://", "moz-extension://", "safari-web-extension://"})
        if (origin.rfind(scheme, 0) == 0)
            return true;
    return false;
}

void sendJson(httplib::Response& res, const Json& j, int status = 200)
{
    res.status = status;
    res.set_content(j.dump(-1, ' ', false, Json::error_handler_t::replace), "application/json");
}

std::string text(const Json& v, size_t max = kMaxField)
{
    if (!v.is_string())
        return {};
    std::string s = v.get<std::string>();
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    s = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    if (s.size() > max) {
        size_t cut = max;
        while (cut > 0 && (s[cut] & 0xC0) == 0x80)
            --cut;
        s.resize(cut);
    }
    return s;
}

// A file name for a PDF from its address, else from the title.
std::string pdfFilename(const std::string& url, const std::string& title)
{
    std::string path = url.substr(0, url.find_first_of("?#"));
    std::string last = path.substr(path.find_last_of('/') + 1);
    last = ss(QUrl::fromPercentEncoding(qs(last).toUtf8()));
    if (last.size() > 4 && (last.substr(last.size() - 4) == ".pdf" || last.substr(last.size() - 4) == ".PDF"))
        return safeFilename(last, "document.pdf");
    std::string base = title.empty() ? (last.empty() ? std::string("document") : last) : title.substr(0, 120);
    return safeFilename(base + ".pdf", "document.pdf");
}

fs::path newTempDir()
{
    auto dir = fs::temp_directory_path() / ("ournotes-connector-" + randomToken(8));
    fs::create_directories(dir);
    return dir;
}

struct TempDirGuard {
    fs::path dir;
    ~TempDirGuard()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

int64_t number(const std::string& s)
{
    try {
        return s.empty() ? 0 : std::stoll(s);
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

Json connectorItem(const Json& page)
{
    const Json in = page.is_object() ? page : Json::object();
    Json out = Json::object();
    auto type = text(in.value("itemType", Json()));
    if (!schema::isValidType(type) || !schema::isRegularType(type))
        type = "webpage";
    out["itemType"] = type;
    for (auto& [field, value] : in.items()) {
        if (field == "itemType" || field == "creators" || field == "tags" || !schema::hasField(type, field))
            continue;
        auto v = text(value, field == "abstractNote" ? 200000 : kMaxField);
        if (!v.empty())
            out[field] = v;
    }
    auto types = schema::creatorTypesFor(type);
    Json creators = Json::array();
    auto listed = in.value("creators", Json::array());
    for (auto& c : listed.is_array() ? listed : Json::array()) {
        if (!c.is_object() || creators.size() >= 200)
            continue;
        auto creatorType = text(c.value("creatorType", Json()));
        if (std::find(types.begin(), types.end(), creatorType) == types.end())
            creatorType = schema::primaryCreatorType(type);
        Json one{{"creatorType", creatorType}};
        if (auto name = text(c.value("name", Json()), 500); !name.empty()) {
            one["name"] = name;
        } else {
            auto last = text(c.value("lastName", Json()), 500), first = text(c.value("firstName", Json()), 500);
            if (last.empty() && first.empty())
                continue;
            one["lastName"] = last;
            one["firstName"] = first;
        }
        creators.push_back(one);
    }
    out["creators"] = creators;
    Json tags = Json::array();
    auto given = in.value("tags", Json::array());
    for (auto& t : given.is_array() ? given : Json::array()) {
        auto tag = text(t.is_object() ? t.value("tag", Json()) : t, 250);
        if (!tag.empty() && tags.size() < 100)
            tags.push_back(Json{{"tag", tag}});
    }
    out["tags"] = tags;
    if (!out.contains("title"))
        out["title"] = out.contains("url") ? out["url"] : Json("Untitled");
    if (schema::hasField(type, "accessDate") && !out.contains("accessDate"))
        out["accessDate"] = nowIso();
    return out;
}

ConnectorServer::ConnectorServer(QObject* parent) : QObject(parent) {}

ConnectorServer::~ConnectorServer() { stop(); }

template <typename F>
auto ConnectorServer::onGui(F f) -> decltype(f())
{
    using R = decltype(f());
    auto done = std::make_shared<std::promise<R>>();
    auto result = done->get_future();
    // dropped with this object if it goes first
    QMetaObject::invokeMethod(
        this,
        [done, f = std::move(f)]() mutable {
            try {
                done->set_value(f());
            } catch (...) {
                done->set_exception(std::current_exception());
            }
        },
        Qt::QueuedConnection);
    while (result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (stopping_)
            throw Refused("OurNotes is closing", 503);
    return result.get();
}

bool ConnectorServer::start(int port)
{
    stop();
    stopping_ = false;
    server_ = std::make_unique<httplib::Server>();
    auto& svr = *server_;
    svr.set_payload_max_length(kMaxPdf);
    // one OurNotes per port: httplib's default, SO_REUSEPORT, lets a second one (or any
    // program) listen on it too and take some of the requests
    svr.set_socket_options([](socket_t sock) {
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    });
    svr.set_read_timeout(120);
    svr.set_write_timeout(30);

    // only the extension, and only at this address (a page that renamed itself to
    // 127.0.0.1 must not get in, nor a page's own requests)
    svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        auto host = req.get_header_value("Host");
        std::string p = ":" + std::to_string(port_);
        if (host != "127.0.0.1" + p && host != "localhost" + p) {
            sendJson(res, Json{{"error", "wrong host"}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        auto origin = req.get_header_value("Origin");
        if (!origin.empty() && !isExtensionOrigin(origin)) {
            sendJson(res, Json{{"error", "only the OurNotes browser extension may save here"}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!origin.empty()) {
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Vary", "Origin");
        }
        if (req.method == "OPTIONS") {
            res.set_header("Access-Control-Allow-Methods", "GET, POST");
            res.set_header("Access-Control-Allow-Headers", std::string("Content-Type, ") + kHeader);
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.method == "POST" && !req.has_header(kHeader)) {
            sendJson(res, Json{{"error", std::string("missing ") + kHeader}}, 400);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    auto guarded = [](httplib::Response& res, const std::function<void()>& fn) {
        try {
            fn();
        } catch (const Refused& e) {
            sendJson(res, Json{{"error", e.what()}}, e.status);
        } catch (const std::exception& e) {
            sendJson(res, Json{{"error", e.what()}}, 500);
        }
    };
    auto jsonBody = [](const httplib::Request& req) {
        if (req.body.size() > kMaxJson)
            throw Refused("request too large", 413);
        auto j = Json::parse(req.body, nullptr, false);
        if (!j.is_object())
            throw Refused("expected a JSON object", 400);
        return j;
    };

    svr.Get("/connector/ping", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, Json{{"app", "OurNotes"}, {"version", OURNOTES_VERSION}, {"api", 1}});
    });

    svr.Post("/connector/save", [=](const httplib::Request& req, httplib::Response& res) {
        guarded(res, [&] {
            auto item = jsonBody(req).value("item", Json());
            if (!item.is_object())
                throw Refused("no item", 400);
            // a page that names a DOI or arXiv id but says little else: ask Crossref / arXiv
            bool thin = text(item.value("title", Json())).empty() || item.value("creators", Json::array()).empty();
            std::string id = text(item.value("DOI", Json()));
            if (id.empty())
                id = text(item.value("archiveID", Json()));
            if (thin && !id.empty()) {
                auto ids = formats::parseIdentifiers(id);
                if (!ids.empty()) {
                    try {
                        Json found = formats::lookup(ids[0]);
                        for (auto& field : {"url", "accessDate"})
                            if (item.contains(field))
                                found[field] = item[field];
                        item = found;
                    } catch (const std::exception&) {
                        // offline or unknown: what the page said
                    }
                }
            }
            auto saved = onGui([this, item] { return save(item); });
            sendJson(res, saved);
        });
    });

    svr.Post("/connector/attachment", [=](const httplib::Request& req, httplib::Response& res) {
        guarded(res, [&] {
            if (req.body.size() < 5 || req.body.compare(0, 5, "%PDF-") != 0)
                throw Refused("not a PDF", 415);
            TempDirGuard tmp{newTempDir()};
            auto url = req.get_param_value("url"), title = req.get_param_value("title");
            auto file = tmp.dir / u8path(pdfFilename(url, title));
            std::ofstream(file, std::ios::binary).write(req.body.data(), (std::streamsize)req.body.size());
            int64_t lib = number(req.get_param_value("library"));
            auto parent = req.get_param_value("parent");
            // copies: when OurNotes closes, the request may end before the GUI gets to it
            sendJson(res, onGui([this, file, lib, parent, url, title] { return addPdf(file, lib, parent, url, title); }));
        });
    });

    svr.Post("/connector/attachmentUrl", [=](const httplib::Request& req, httplib::Response& res) {
        guarded(res, [&] {
            auto body = jsonBody(req);
            auto url = text(body.value("url", Json()), 4000), title = text(body.value("title", Json()), 500);
            if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
                throw Refused("not a web address", 400);
            TempDirGuard tmp{newTempDir()};
            auto file = tmp.dir / u8path(pdfFilename(url, title));
            try {
                formats::downloadFile(url, file, true);
            } catch (const std::exception& e) {
                throw Refused(std::string("could not download the PDF: ") + e.what(), 502);
            }
            int64_t lib = body.value("library", int64_t(0));
            auto parent = text(body.value("parent", Json()), 64);
            sendJson(res, onGui([this, file, lib, parent, url, title] { return addPdf(file, lib, parent, url, title); }));
        });
    });

    svr.Post("/connector/update", [=](const httplib::Request& req, httplib::Response& res) {
        guarded(res, [&] {
            auto body = jsonBody(req);
            sendJson(res, onGui([this, body] { return update(body); }));
        });
    });

    if (!svr.bind_to_port("127.0.0.1", port)) {
        server_.reset();
        return false;
    }
    port_ = port;
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    return true;
}

void ConnectorServer::stop()
{
    stopping_ = true;
    if (server_)
        server_->stop();
    if (thread_.joinable())
        thread_.join();
    server_.reset();
}

Json ConnectorServer::targets(int64_t libraryId) const
{
    auto& lib = AppContext::get().library();
    auto info = lib.library(libraryId);
    Json out = Json::array();
    if (!info)
        return out;
    out.push_back(Json{{"id", std::to_string(libraryId)}, {"name", info->name}, {"depth", 0}});
    auto all = lib.collections(libraryId);
    std::map<std::string, std::vector<Json>> children;
    for (auto& c : all) {
        auto p = c.value("parentCollection", Json());
        children[p.is_string() ? p.get<std::string>() : std::string()].push_back(c);
    }
    std::function<void(const std::string&, int)> add = [&](const std::string& parent, int depth) {
        auto it = children.find(parent);
        if (it == children.end() || depth > 32)
            return;
        auto list = it->second;
        std::sort(list.begin(), list.end(), [](const Json& a, const Json& b) {
            return QString::localeAwareCompare(jqs(a, "name"), jqs(b, "name")) < 0;
        });
        for (auto& c : list) {
            out.push_back(Json{{"id", std::to_string(libraryId) + "/" + jstr(c, "key")},
                               {"name", jstr(c, "name")},
                               {"depth", depth}});
            add(jstr(c, "key"), depth + 1);
        }
    };
    add({}, 1);
    return out;
}

Json ConnectorServer::save(const Json& page)
{
    auto& ctx = AppContext::get();
    auto& lib = ctx.library();
    Scope scope = targetScope ? targetScope() : Scope{};
    if (!scope.libraryId || !lib.library(scope.libraryId) || !ctx.editable(scope.libraryId))
        scope = Scope{lib.defaultLibrary(), ScopeKind::Library, {}};
    std::string collection = scope.kind == ScopeKind::Collection ? scope.collectionKey : std::string();
    Json data = connectorItem(page);
    if (!collection.empty())
        data["collections"] = Json::array({collection});
    auto item = lib.createItem(scope.libraryId, data);
    auto key = jstr(item, "key");
    emit saved(scope.libraryId, qs(key), jqs(item, "title"));
    return Json{{"key", key},
                {"libraryId", scope.libraryId},
                {"title", jstr(item, "title")},
                {"itemType", jstr(item, "itemType")},
                {"target", collection.empty() ? std::to_string(scope.libraryId)
                                              : std::to_string(scope.libraryId) + "/" + collection},
                {"targets", targets(scope.libraryId)}};
}

Json ConnectorServer::addPdf(const fs::path& file, int64_t libraryId, const std::string& parentKey,
                             const std::string& url, const std::string& title)
{
    auto& ctx = AppContext::get();
    auto& lib = ctx.library();
    Json response;
    std::string collection;
    if (parentKey.empty()) {  // a PDF on its own: where pages go
        Scope scope = targetScope ? targetScope() : Scope{};
        if (!scope.libraryId || !lib.library(scope.libraryId) || !ctx.editable(scope.libraryId))
            scope = Scope{lib.defaultLibrary(), ScopeKind::Library, {}};
        libraryId = scope.libraryId;
        if (scope.kind == ScopeKind::Collection)
            collection = scope.collectionKey;
    } else if (!lib.item(libraryId, parentKey) || !ctx.editable(libraryId)) {
        throw Refused("no such item", 404);
    }
    auto att = lib.importAttachment(libraryId, file, parentKey, false, parentKey.empty() ? title : "Full Text PDF");
    auto key = jstr(att, "key");
    lib.updateItem(libraryId, key, [&](Json& d) {
        if (!url.empty()) {
            d["linkMode"] = "imported_url";  // saved from the web, as Zotero has it
            d["url"] = url;
            d["accessDate"] = nowIso();
        }
        if (!collection.empty())
            d["collections"] = Json::array({collection});
    });
    if (parentKey.empty()) {
        emit saved(libraryId, qs(key), qs(jstr(att, "title")));
        emit metadataWanted(libraryId, {qs(key)});
    }
    response = Json{{"key", key}, {"libraryId", libraryId}};
    if (parentKey.empty()) {
        response["target"] = collection.empty() ? std::to_string(libraryId) : std::to_string(libraryId) + "/" + collection;
        response["targets"] = targets(libraryId);
        response["title"] = jstr(att, "title");
    }
    return response;
}

Json ConnectorServer::update(const Json& body)
{
    auto& ctx = AppContext::get();
    auto& lib = ctx.library();
    int64_t libraryId = body.value("library", int64_t(0));
    auto key = text(body.value("key", Json()), 64);
    if (!lib.item(libraryId, key) || !ctx.editable(libraryId))
        throw Refused("no such item", 404);
    auto target = text(body.value("target", Json()), 200);
    std::string collection;
    if (!target.empty()) {
        auto slash = target.find('/');
        if (number(target.substr(0, slash)) != libraryId)
            throw Refused("the item stays in its library", 400);
        if (slash != std::string::npos) {
            collection = target.substr(slash + 1);
            if (!lib.collection(libraryId, collection))
                throw Refused("no such collection", 404);
        }
    }
    lib.updateItem(libraryId, key, [&](Json& d) {
        if (!target.empty() && jstr(d, "parentItem").empty())
            d["collections"] = collection.empty() ? Json::array() : Json::array({collection});
        if (body.contains("tags") && body["tags"].is_array()) {
            Json tags = Json::array();
            for (auto& t : body["tags"])
                if (auto tag = text(t, 250); !tag.empty())
                    tags.push_back(Json{{"tag", tag}});
            d["tags"] = tags;
        }
    });
    return Json{{"key", key}, {"target", target}};
}

} // namespace on::gui
