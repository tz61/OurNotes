#include "sync/server_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>

#include <httplib.h>

#include "ournotes/version.h"

namespace on::sync {

namespace {

using httplib::ContentReader;
using httplib::Request;
using httplib::Response;
using HandlerResponse = httplib::Server::HandlerResponse;

constexpr size_t kMaxJsonBody = 64u << 20;    // push batches
constexpr size_t kMaxSmallBody = 64u << 10;   // login, library metadata
constexpr int64_t kMaxWaitSeconds = 120;       // GET /libraries?state=&wait=
const char* const kUserKey = "ournotes.user";  // Response::user_data slot

// In-memory throttle for failed logins, per client address.
class LoginLimiter {
public:
    // Seconds until the address may try again, 0 if it is not blocked.
    int64_t retryAfter(const std::string& addr)
    {
        std::lock_guard lock(mutex_);
        auto it = failures_.find(addr);
        if (it == failures_.end())
            return 0;
        auto now = Clock::now();
        prune(it->second, now);
        if ((int)it->second.size() < kMaxFailures)
            return 0;
        auto wait = std::chrono::duration_cast<std::chrono::seconds>(it->second.front() + kWindow - now).count();
        return std::max<int64_t>(wait, 1);
    }

    void failed(const std::string& addr)
    {
        std::lock_guard lock(mutex_);
        auto now = Clock::now();
        failures_[addr].push_back(now);
        if (failures_.size() > 10000) {  // bound memory under a distributed attack
            for (auto it = failures_.begin(); it != failures_.end();) {
                prune(it->second, now);
                it = it->second.empty() ? failures_.erase(it) : std::next(it);
            }
        }
    }

    void succeeded(const std::string& addr)
    {
        std::lock_guard lock(mutex_);
        failures_.erase(addr);
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxFailures = 10;
    static constexpr std::chrono::minutes kWindow{10};

    static void prune(std::deque<Clock::time_point>& q, Clock::time_point now)
    {
        while (!q.empty() && q.front() + kWindow <= now)
            q.pop_front();
    }

    std::mutex mutex_;
    std::map<std::string, std::deque<Clock::time_point>> failures_;
};

void sendJson(Response& res, const Json& j, int status = 200)
{
    res.status = status;
    // Invalid UTF-8 (e.g. in a stored file name) must not make a response fail.
    res.set_content(j.dump(-1, ' ', false, Json::error_handler_t::replace), "application/json");
}

void sendError(Response& res, int status, const std::string& message)
{
    sendJson(res, Json{{"error", message}}, status);
}

// Runs a handler, turning exceptions into JSON error responses.
template <typename F>
void guarded(Response& res, F&& fn)
{
    try {
        fn();
    } catch (const SyncError& e) {
        if (e.status >= 400 && e.status < 600) {
            sendError(res, e.status, e.what());
        } else {
            std::cerr << "ournotesd: error: " << e.what() << std::endl;
            sendError(res, 500, "internal server error");
        }
    } catch (const Json::exception& e) {
        sendError(res, 400, std::string("invalid request: ") + e.what());
    } catch (const std::exception& e) {
        std::cerr << "ournotesd: internal error: " << e.what() << std::endl;
        sendError(res, 500, "internal server error");
    }
}

std::string pathParam(const Request& req, const char* name)
{
    auto it = req.path_params.find(name);
    return it == req.path_params.end() ? std::string() : it->second;
}

int64_t intParam(const Request& req, const char* name, int64_t def)
{
    auto v = req.get_param_value(name);
    if (v.empty())
        return def;
    try {
        size_t used = 0;
        auto n = std::stoll(v, &used);
        if (used == v.size())
            return n;
    } catch (const std::exception&) {
    }
    throw SyncError(std::string("invalid parameter '") + name + "'", 400);
}

Json readJson(const ContentReader& reader, size_t limit)
{
    std::string body;
    bool tooLarge = false;
    bool ok = reader([&](const char* data, size_t len) {
        if (body.size() + len > limit) {
            tooLarge = true;
            return false;
        }
        body.append(data, len);
        return true;
    });
    if (tooLarge)
        throw SyncError("request body too large", 413);
    if (!ok)
        throw SyncError("could not read request body", 400);
    auto j = Json::parse(body, nullptr, false);
    if (!j.is_object())
        throw SyncError("request body must be a JSON object", 400);
    return j;
}

std::string bearerToken(const Request& req)
{
    auto h = req.get_header_value("Authorization");
    if (h.size() > 7 && iequals(std::string_view(h).substr(0, 7), "Bearer "))
        return trim(std::string_view(h).substr(7));
    return {};
}

bool isFileRoute(const std::string& path)
{
    return startsWith(path, "/api/v1/libraries/") && path.find("/files/") != std::string::npos;
}

class TempFile {
public:
    explicit TempFile(fs::path path) : path_(std::move(path)) {}
    ~TempFile()
    {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }
    const fs::path& path() const { return path_; }
    void release() { path_.clear(); }

private:
    fs::path path_;
};

} // namespace

struct ApiServer::State {
    Store& store;
    ServerOptions opts;
    LoginLimiter limiter;
    // Requests waiting for the libraries to change, woken by every change and by stop().
    std::mutex changeMutex;
    std::condition_variable changeSignal;
    uint64_t changeCount = 0;
    std::atomic<int> waiting{0};
    std::atomic<bool> stopping{false};

    State(Store& s, ServerOptions o) : store(s), opts(std::move(o)) {}

    void changed()
    {
        {
            std::lock_guard lock(changeMutex);
            ++changeCount;
        }
        changeSignal.notify_all();
    }

    std::string authenticate(const Request& req, Response& res)
    {
        auto token = bearerToken(req);
        if (token.empty()) {
            res.set_header("WWW-Authenticate", "Bearer");
            throw SyncError("authentication required", 401);
        }
        auto user = store.userForToken(token);
        if (!user) {
            res.set_header("WWW-Authenticate", "Bearer error=\"invalid_token\"");
            throw SyncError("invalid or expired token", 401);
        }
        res.user_data.set(kUserKey, *user);
        return *user;
    }

    RemoteLibrary requireLibrary(const std::string& uid, const std::string& user, bool write)
    {
        auto lib = isValidKey(uid) ? store.library(uid, user) : std::nullopt;
        if (!lib)
            throw SyncError("library not found", 404);
        if (write && lib->role == "read")
            throw SyncError("read-only access to this library", 403);
        return *lib;
    }

    void login(const Request& req, Response& res, const ContentReader& reader)
    {
        auto& addr = req.remote_addr;
        if (auto wait = limiter.retryAfter(addr); wait > 0) {
            res.set_header("Retry-After", std::to_string(wait));
            throw SyncError("too many failed login attempts; try again later", 429);
        }
        auto body = readJson(reader, kMaxSmallBody);
        auto username = jstr(body, "username");
        auto password = jstr(body, "password");
        if (username.empty() || password.empty())
            throw SyncError("username and password are required", 400);
        if (!store.checkPassword(username, password)) {
            limiter.failed(addr);
            throw SyncError("invalid username or password", 401);
        }
        limiter.succeeded(addr);
        auto device = jstr(body, "device");
        if (device.empty())
            device = req.get_header_value("User-Agent");
        auto token = store.createToken(username, device);
        res.user_data.set(kUserKey, username);
        sendJson(res, Json{{"token", token}, {"username", username}});
    }

    void logout(const Request& req, Response& res)
    {
        authenticate(req, res);
        store.revokeToken(bearerToken(req));
        sendJson(res, Json{{"ok", true}});
    }

    void me(const Request& req, Response& res)
    {
        auto user = authenticate(req, res);
        sendJson(res, Json{{"username", user}, {"admin", store.isAdmin(user)}});
    }

    // With ?state=S&wait=N: held while the libraries' state is still S, at most N seconds,
    // so that a client learns at once that another one pushed.
    void listLibraries(const Request& req, Response& res)
    {
        using namespace std::chrono;
        auto user = authenticate(req, res);
        auto known = req.get_param_value("state");
        auto wait = std::clamp<int64_t>(intParam(req, "wait", 0), 0, kMaxWaitSeconds);
        bool hold = !known.empty() && wait > 0;
        int holding = hold ? ++waiting : 0;
        struct Release {
            std::atomic<int>* n;
            ~Release()
            {
                if (n)
                    --*n;
            }
        } release{hold ? &waiting : nullptr};
        hold = hold && holding <= ApiServer::kMaxWaiting;
        auto deadline = steady_clock::now() + seconds(wait);
        std::vector<RemoteLibrary> libs;
        std::string state;
        for (;;) {
            uint64_t seen;
            {
                std::lock_guard lock(changeMutex);
                seen = changeCount;
            }
            libs = store.libraries(user);
            state = librariesState(libs);
            if (!hold || state != known)
                break;
            {
                // every second, also to see whether the client is gone
                std::unique_lock lock(changeMutex);
                changeSignal.wait_until(lock, std::min(deadline, steady_clock::now() + seconds(1)),
                                        [&] { return changeCount != seen || stopping; });
            }
            if (steady_clock::now() >= deadline || stopping || req.is_connection_closed())
                break;
        }
        Json list = Json::array();
        for (auto& l : libs)
            list.push_back(toJson(l));
        sendJson(res, Json{{"libraries", list}, {"state", state}});
    }

    void getLibrary(const Request& req, Response& res)
    {
        auto user = authenticate(req, res);
        sendJson(res, toJson(requireLibrary(pathParam(req, "uid"), user, false)));
    }

    void putLibrary(const Request& req, Response& res, const ContentReader& reader)
    {
        auto user = authenticate(req, res);
        auto body = readJson(reader, kMaxSmallBody);
        auto lib = store.ensureLibrary(pathParam(req, "uid"), jstr(body, "name"), jstr(body, "type", "user"), user);
        changed();
        sendJson(res, toJson(lib));
    }

    void deleteLibrary(const Request& req, Response& res)
    {
        auto user = authenticate(req, res);
        auto uid = pathParam(req, "uid");
        if (requireLibrary(uid, user, false).role != "owner")
            throw SyncError("only the owner of the library can delete it", 403);
        store.deleteLibrary(uid, true);
        changed();
        sendJson(res, Json{{"deleted", true}});
    }

    void changes(const Request& req, Response& res)
    {
        auto user = authenticate(req, res);
        auto uid = pathParam(req, "uid");
        requireLibrary(uid, user, false);
        auto since = intParam(req, "since", 0);
        auto limit = std::clamp<int64_t>(intParam(req, "limit", 0), 0, kMaxChangesLimit);
        sendJson(res, toJson(store.changes(uid, since, req.get_param_value("cursor"), (int)limit)));
    }

    void push(const Request& req, Response& res, const ContentReader& reader)
    {
        auto user = authenticate(req, res);
        auto uid = pathParam(req, "uid");
        requireLibrary(uid, user, true);
        auto body = readJson(reader, kMaxJsonBody);
        auto it = body.find("objects");
        if (it == body.end() || !it->is_array())
            throw SyncError("'objects' must be an array", 400);
        std::vector<PushObject> objects;
        objects.reserve(it->size());
        for (auto& o : *it) {
            if (!o.is_object())
                throw SyncError("each pushed object must be a JSON object", 400);
            objects.push_back(pushObjectFromJson(o));
        }
        auto result = store.push(uid, objects);
        changed();
        sendJson(res, toJson(result));
    }

    void listFiles(const Request& req, Response& res)
    {
        auto user = authenticate(req, res);
        auto uid = pathParam(req, "uid");
        requireLibrary(uid, user, false);
        Json files = Json::array();
        for (auto& f : store.files(uid))
            files.push_back(toJson(f));
        sendJson(res, Json{{"files", files}});
    }

    void getFile(const Request& req, Response& res)
    {
        auto user = authenticate(req, res);
        auto uid = pathParam(req, "uid");
        auto key = pathParam(req, "key");
        requireLibrary(uid, user, false);
        auto info = isValidKey(key) ? store.file(uid, key) : std::nullopt;
        if (!info)
            throw SyncError("file not found", 404);
        auto path = store.blobPath(uid, key);
        auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
        std::error_code ec;
        auto size = fs::file_size(path, ec);
        if (!*in || ec)
            throw SyncError("file data is missing on the server", 404);

        res.set_header("X-OurNotes-MD5", info->md5);
        res.set_header("X-OurNotes-Mtime", std::to_string(info->mtime));
        res.set_header("X-OurNotes-Filename", urlEncode(info->filename));
        auto buf = std::make_shared<std::vector<char>>(256 * 1024);
        res.set_content_provider(
            (size_t)size, info->contentType.empty() ? "application/octet-stream" : info->contentType,
            [in, buf](size_t offset, size_t length, httplib::DataSink& sink) {
                if (in->tellg() != (std::streamoff)offset) {  // range requests
                    in->clear();
                    in->seekg((std::streamoff)offset);
                }
                in->read(buf->data(), (std::streamsize)std::min(length, buf->size()));
                auto n = in->gcount();
                return n > 0 && sink.write(buf->data(), (size_t)n);
            });
    }

    void putFile(const Request& req, Response& res, const ContentReader& reader)
    {
        auto user = authenticate(req, res);
        auto uid = pathParam(req, "uid");
        auto key = pathParam(req, "key");
        requireLibrary(uid, user, true);
        if (!isValidKey(key))
            throw SyncError("invalid file key", 400);

        TempFile tmp(store.newTempPath());
        int64_t received = 0;
        bool tooLarge = false, writeFailed = false;
        std::ofstream out(tmp.path(), std::ios::binary | std::ios::trunc);
        if (!out)
            throw SyncError("cannot create " + pathString(tmp.path()));
        bool ok = reader([&](const char* data, size_t len) {
            received += (int64_t)len;
            if (received > opts.maxUploadBytes) {
                tooLarge = true;
                return false;
            }
            out.write(data, (std::streamsize)len);
            writeFailed = !out;
            return !writeFailed;
        });
        out.close();
        if (tooLarge)
            throw SyncError("file is larger than the server's upload limit", 413);
        if (writeFailed || !out)
            throw SyncError("cannot store upload (disk full?)", 507);
        if (!ok)
            throw SyncError("upload interrupted", 400);
        if (req.has_header("Content-Length") && (uint64_t)received != req.get_header_value_u64("Content-Length"))
            throw SyncError("incomplete upload", 400);

        FileInfo meta;
        meta.key = key;
        meta.md5 = req.get_param_value("md5");
        meta.filename = req.get_param_value("filename");
        meta.contentType = req.get_param_value("contentType");
        meta.mtime = intParam(req, "mtime", 0);
        auto info = store.putFile(uid, key, tmp.path(), meta);
        tmp.release();
        sendJson(res, toJson(info));
    }

    void deleteFile(const Request& req, Response& res)
    {
        auto user = authenticate(req, res);
        auto uid = pathParam(req, "uid");
        auto key = pathParam(req, "key");
        requireLibrary(uid, user, true);
        if (!isValidKey(key))
            throw SyncError("invalid file key", 400);
        sendJson(res, Json{{"deleted", store.deleteFile(uid, key)}});
    }
};

ApiServer::ApiServer(Store& store, ServerOptions options)
    : state_(std::make_shared<State>(store, std::move(options)))
{
}

ApiServer::~ApiServer() = default;

void ApiServer::stop()
{
    state_->stopping = true;
    state_->changed();
}

std::string ApiServer::requestUser(const httplib::Response& res)
{
    auto user = res.user_data.get<std::string>(kUserKey);
    return user ? *user : std::string();
}

void ApiServer::mount(httplib::Server& svr)
{
    auto s = state_;
    const int64_t maxUpload = s->opts.maxUploadBytes;
    svr.set_payload_max_length((size_t)std::max<int64_t>(maxUpload, (int64_t)kMaxJsonBody));

    // Refuse oversized bodies up front: GET handlers would buffer a body in memory,
    // and only file uploads may be large.
    svr.set_pre_routing_handler([maxUpload](const Request& req, Response& res) {
        uint64_t length = req.get_header_value_u64("Content-Length");
        bool chunked = req.has_header("Transfer-Encoding");
        if ((req.method == "GET" || req.method == "HEAD") && (chunked || length > 0)) {
            sendError(res, 400, "unexpected request body");
            return HandlerResponse::Handled;
        }
        uint64_t limit = req.method == "PUT" && isFileRoute(req.path) ? (uint64_t)maxUpload : kMaxJsonBody;
        if (length > limit) {
            sendError(res, 413, "request body too large");
            return HandlerResponse::Handled;
        }
        return HandlerResponse::Unhandled;
    });

    svr.set_error_handler([](const Request&, Response& res) {
        if (!res.body.empty())
            return HandlerResponse::Unhandled;
        sendError(res, res.status, httplib::status_message(res.status));
        return HandlerResponse::Handled;
    });

    // Plain handler / handler that receives the request body as a stream.
    auto get = [s](void (State::*fn)(const Request&, Response&)) {
        return [s, fn](const Request& req, Response& res) { guarded(res, [&] { ((*s).*fn)(req, res); }); };
    };
    auto withBody = [s](void (State::*fn)(const Request&, Response&, const ContentReader&)) {
        return [s, fn](const Request& req, Response& res, const ContentReader& reader) {
            guarded(res, [&] { ((*s).*fn)(req, res, reader); });
        };
    };
    auto ignoreBody = [s](void (State::*fn)(const Request&, Response&)) {
        return [s, fn](const Request& req, Response& res, const ContentReader&) {
            guarded(res, [&] { ((*s).*fn)(req, res); });
        };
    };

    const std::string api = "/api/v1";
    const std::string lib = api + "/libraries/:uid";
    svr.Get(api + "/info", [s](const Request&, Response& res) {
        sendJson(res, Json{{"server", s->opts.serverName}, {"version", OURNOTES_VERSION}, {"api", kApiVersion}});
    });
    svr.Post(api + "/login", withBody(&State::login));
    svr.Post(api + "/logout", ignoreBody(&State::logout));
    svr.Get(api + "/me", get(&State::me));
    svr.Get(api + "/libraries", get(&State::listLibraries));
    svr.Get(lib, get(&State::getLibrary));
    svr.Put(lib, withBody(&State::putLibrary));
    svr.Delete(lib, ignoreBody(&State::deleteLibrary));
    svr.Get(lib + "/changes", get(&State::changes));
    svr.Post(lib + "/push", withBody(&State::push));
    svr.Get(lib + "/files", get(&State::listFiles));
    svr.Get(lib + "/files/:key", get(&State::getFile));
    svr.Put(lib + "/files/:key", withBody(&State::putFile));
    svr.Delete(lib + "/files/:key", ignoreBody(&State::deleteFile));
}

} // namespace on::sync
