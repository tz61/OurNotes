#include "sync/http_transport.h"

#include <fstream>

#include <httplib.h>

#include "core/hash.h"

namespace on::sync {

namespace {

struct BaseUrl {
    std::string scheme;    // "http" | "https"
    std::string hostPort;  // "host", "host:port", "[::1]:8642"
    std::string prefix;    // "" or "/path" (no trailing '/')
};

BaseUrl parseBaseUrl(const std::string& url)
{
    auto u = trim(url);
    auto bad = [&]() {
        return SyncError("invalid server URL '" + u + "' (expected http://host:port or https://host/path)", 400);
    };
    auto sep = u.find("://");
    if (sep == std::string::npos)
        throw bad();
    BaseUrl b;
    b.scheme = toLower(u.substr(0, sep));
    if (b.scheme != "http" && b.scheme != "https")
        throw bad();
    auto rest = u.substr(sep + 3);
    rest = rest.substr(0, rest.find_first_of("?#"));
    auto slash = rest.find('/');
    b.hostPort = rest.substr(0, slash);
    if (slash != std::string::npos)
        b.prefix = rest.substr(slash);
    while (!b.prefix.empty() && b.prefix.back() == '/')
        b.prefix.pop_back();
    if (endsWith(b.prefix, "/api/v1"))  // tolerate a full API URL
        b.prefix.resize(b.prefix.size() - 7);
    if (b.hostPort.empty() || b.hostPort.find('@') != std::string::npos)
        throw bad();
#ifndef CPPHTTPLIB_SSL_ENABLED
    if (b.scheme == "https")
        throw SyncError("HTTPS support is not compiled in");
#endif
    return b;
}

std::string errorMessage(int status, const std::string& body)
{
    auto j = Json::parse(body, nullptr, false);
    if (j.is_object()) {
        auto msg = jstr(j, "error");
        if (!msg.empty())
            return msg;
    }
    return "HTTP " + std::to_string(status) + " " + httplib::status_message(status);
}

[[noreturn]] void throwTransportError(httplib::Error err, const std::string& baseUrl)
{
    if (err == httplib::Error::Canceled)
        throw SyncError("cancelled");
    throw SyncError("cannot reach " + baseUrl + ": " + httplib::to_string(err));
}

void check(const httplib::Result& res, const std::string& baseUrl)
{
    if (!res)
        throwTransportError(res.error(), baseUrl);
    if (res->status >= 300 && res->status < 400 && res->has_header("Location"))
        throw SyncError("the server redirects to " + res->get_header_value("Location") + "; check the server URL",
                        res->status);
    if (res->status < 200 || res->status >= 300)
        throw SyncError(errorMessage(res->status, res->body), res->status);
}

int64_t toInt64(const std::string& s)
{
    try {
        return s.empty() ? 0 : std::stoll(s);
    } catch (const std::exception&) {
        return 0;
    }
}

class FileGuard {
public:
    explicit FileGuard(fs::path path) : path_(std::move(path)) {}
    ~FileGuard()
    {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }
    void release() { path_.clear(); }

private:
    fs::path path_;
};

} // namespace

HttpTransport::HttpTransport(const std::string& baseUrl, const std::string& token, bool verifyTls)
{
    auto b = parseBaseUrl(baseUrl);
    baseUrl_ = b.scheme + "://" + b.hostPort + b.prefix;
    prefix_ = b.prefix;
    client_ = std::make_unique<httplib::Client>(b.scheme + "://" + b.hostPort);
    if (!client_->is_valid())
        throw SyncError("invalid server URL '" + baseUrl + "'", 400);
    client_->set_connection_timeout(10);
    client_->set_read_timeout(300);
    client_->set_write_timeout(300);
    client_->set_follow_location(false);
    client_->set_keep_alive(true);
    client_->set_path_encode(false);  // paths and queries are encoded here
#ifdef CPPHTTPLIB_SSL_ENABLED
    if (b.scheme == "https") {
        client_->enable_server_certificate_verification(verifyTls);
        client_->enable_server_hostname_verification(verifyTls);
    }
#endif
    if (!token.empty())
        client_->set_bearer_token_auth(token);
}

HttpTransport::~HttpTransport() = default;

std::string HttpTransport::apiPath(const std::string& path) const { return prefix_ + "/api/v1" + path; }

std::string HttpTransport::libraryPath(const std::string& uid, const std::string& rest) const
{
    return apiPath("/libraries/" + urlEncode(uid) + rest);
}

Json HttpTransport::request(const char* method, const std::string& path, const Json* body)
{
    std::string payload = body ? body->dump(-1, ' ', false, Json::error_handler_t::replace) : std::string("{}");
    std::string m = method;
    httplib::Result res;
    if (m == "GET")
        res = client_->Get(path);
    else if (m == "POST")
        res = client_->Post(path, payload, "application/json");
    else if (m == "PUT")
        res = client_->Put(path, payload, "application/json");
    else
        res = client_->Delete(path);
    check(res, baseUrl_);
    if (res->body.empty())
        return Json::object();
    auto j = Json::parse(res->body, nullptr, false);
    if (j.is_discarded())
        throw SyncError("invalid response from " + baseUrl_ + " (not JSON)");
    return j;
}

std::string HttpTransport::login(const std::string& baseUrl, const std::string& username,
                                 const std::string& password, const std::string& device, bool verifyTls)
{
    HttpTransport t(baseUrl, "", verifyTls);
    Json body{{"username", username}, {"password", password}, {"device", device}};
    auto token = jstr(t.request("POST", t.apiPath("/login"), &body), "token");
    if (token.empty())
        throw SyncError("login failed: no token in response");
    return token;
}

void HttpTransport::logout() { request("POST", apiPath("/logout")); }

Json HttpTransport::me() { return request("GET", apiPath("/me")); }

Json HttpTransport::info() { return request("GET", apiPath("/info")); }

std::vector<RemoteLibrary> HttpTransport::libraries()
{
    std::vector<RemoteLibrary> out;
    for (auto& l : request("GET", apiPath("/libraries")).value("libraries", Json::array()))
        out.push_back(remoteLibraryFromJson(l));
    return out;
}

RemoteLibrary HttpTransport::ensureLibrary(const std::string& uid, const std::string& name, const std::string& type)
{
    Json body{{"name", name}, {"type", type}};
    return remoteLibraryFromJson(request("PUT", libraryPath(uid), &body));
}

ChangesPage HttpTransport::changes(const std::string& uid, int64_t since, const std::string& cursor, int limit)
{
    std::string q = "/changes?since=" + std::to_string(since);
    if (limit > 0)
        q += "&limit=" + std::to_string(limit);
    if (!cursor.empty())
        q += "&cursor=" + urlEncode(cursor);
    return changesPageFromJson(request("GET", libraryPath(uid, q)));
}

PushResult HttpTransport::push(const std::string& uid, const std::vector<PushObject>& objects)
{
    Json arr = Json::array();
    for (auto& o : objects)
        arr.push_back(toJson(o));
    Json body{{"objects", std::move(arr)}};
    return pushResultFromJson(request("POST", libraryPath(uid, "/push"), &body));
}

std::vector<FileInfo> HttpTransport::files(const std::string& uid)
{
    std::vector<FileInfo> out;
    for (auto& f : request("GET", libraryPath(uid, "/files")).value("files", Json::array()))
        out.push_back(fileInfoFromJson(f));
    return out;
}

FileInfo HttpTransport::download(const std::string& uid, const std::string& key, const fs::path& dest,
                                 const ProgressFn& progress)
{
    std::error_code ec;
    if (dest.has_parent_path())
        fs::create_directories(dest.parent_path(), ec);
    auto part = dest;
    part += ".part";
    FileGuard guard(part);

    FileInfo info;
    info.key = key;
    int status = 0;
    bool writeFailed = false;
    std::string errorBody;
    std::ofstream out;
    auto ok = [&]() { return status >= 200 && status < 300; };

    auto res = client_->Get(
        libraryPath(uid, "/files/" + urlEncode(key)),
        [&](const httplib::Response& r) {
            status = r.status;
            if (!ok())
                return true;  // keep reading: the body carries the error message
            info.md5 = toLower(r.get_header_value("X-OurNotes-MD5"));
            info.mtime = toInt64(r.get_header_value("X-OurNotes-Mtime"));
            info.filename = urlDecode(r.get_header_value("X-OurNotes-Filename"));
            info.contentType = r.get_header_value("Content-Type");
            if (out.is_open())
                out.close();
            out.open(part, std::ios::binary | std::ios::trunc);
            writeFailed = !out;
            return !writeFailed;
        },
        [&](const char* data, size_t len) {
            if (!ok()) {
                if (errorBody.size() < 65536)
                    errorBody.append(data, len);
                return true;
            }
            out.write(data, (std::streamsize)len);
            writeFailed = !out;
            return !writeFailed;
        },
        [&](size_t done, size_t total) { return !progress || progress((int64_t)done, (int64_t)total); });
    out.close();

    if (writeFailed)
        throw SyncError("cannot write " + pathString(part));
    if (!res)
        throwTransportError(res.error(), baseUrl_);
    if (!ok())
        throw SyncError(errorMessage(status, errorBody), status);

    // The response handler created the file, so an empty body yields an empty file.
    auto md5 = md5File(part);
    if (!info.md5.empty() && md5 != info.md5)
        throw SyncError("checksum mismatch downloading file '" + key + "'");
    info.md5 = md5;
    info.size = (int64_t)fs::file_size(part);
    fs::rename(part, dest, ec);
    if (ec)
        throw SyncError("cannot write " + pathString(dest) + ": " + ec.message());
    guard.release();
    return info;
}

FileInfo HttpTransport::upload(const std::string& uid, const std::string& key, const fs::path& src,
                               const FileInfo& meta, const ProgressFn& progress)
{
    std::error_code ec;
    auto size = fs::file_size(src, ec);
    if (ec)
        throw SyncError("cannot read " + pathString(src) + ": " + ec.message());
    std::ifstream in(src, std::ios::binary);
    if (!in)
        throw SyncError("cannot read " + pathString(src));

    auto md5 = meta.md5.empty() ? md5File(src) : meta.md5;
    auto filename = meta.filename.empty() ? pathString(src.filename()) : meta.filename;
    auto contentType = meta.contentType.empty() ? guessContentType(u8path(filename)) : meta.contentType;
    std::string query = "?md5=" + urlEncode(md5) + "&filename=" + urlEncode(filename) +
                        "&mtime=" + std::to_string(meta.mtime) + "&contentType=" + urlEncode(contentType);

    std::vector<char> buf(256 * 1024);
    bool readFailed = false;
    auto provider = [&](size_t offset, size_t length, httplib::DataSink& sink) {
        if (in.tellg() != (std::streamoff)offset) {  // a retried request starts over
            in.clear();
            in.seekg((std::streamoff)offset);
        }
        in.read(buf.data(), (std::streamsize)std::min(length, buf.size()));
        auto n = (size_t)in.gcount();
        if (n == 0) {
            readFailed = true;  // the file shrank or became unreadable
            return false;
        }
        return sink.write(buf.data(), n);
    };
    auto onProgress = [&](size_t done, size_t total) { return !progress || progress((int64_t)done, (int64_t)total); };
    auto res = client_->Put(libraryPath(uid, "/files/" + urlEncode(key)) + query, (size_t)size, provider,
                            "application/octet-stream", onProgress);
    if (readFailed)
        throw SyncError("read failed: " + pathString(src));
    check(res, baseUrl_);
    auto j = Json::parse(res->body, nullptr, false);
    if (!j.is_object())
        throw SyncError("invalid response from " + baseUrl_ + " (not JSON)");
    return fileInfoFromJson(j);
}

} // namespace on::sync
