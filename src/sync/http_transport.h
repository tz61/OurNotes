#pragma once

#include <memory>

#include "core/sync/protocol.h"

namespace httplib {
class Client;
}

namespace on::sync {

// Client for an ournotesd server. baseUrl is "http://host:port" or, behind a reverse
// proxy, e.g. "https://example.org/ournotes". Not thread-safe: use one instance
// from one thread at a time. Errors are SyncError with the HTTP status, or
// status 0 for network/IO errors and cancellation.
class HttpTransport : public Transport {
public:
    HttpTransport(const std::string& baseUrl, const std::string& token, bool verifyTls = true);
    ~HttpTransport() override;
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    // Exchanges credentials for a token (store it; the password is not needed again).
    static std::string login(const std::string& baseUrl, const std::string& username, const std::string& password,
                             const std::string& device, bool verifyTls = true);
    void logout();   // revokes this transport's token
    Json me();       // {"username", "admin"}

    Json info() override;
    std::vector<RemoteLibrary> libraries() override;
    RemoteLibrary ensureLibrary(const std::string& uid, const std::string& name, const std::string& type) override;
    void deleteLibrary(const std::string& uid) override;
    ChangesPage changes(const std::string& uid, int64_t since, const std::string& cursor, int limit) override;
    PushResult push(const std::string& uid, const std::vector<PushObject>& objects) override;

    std::vector<FileInfo> files(const std::string& uid) override;
    FileInfo download(const std::string& uid, const std::string& key, const fs::path& dest,
                      const ProgressFn& progress = {}) override;
    FileInfo upload(const std::string& uid, const std::string& key, const fs::path& src, const FileInfo& meta,
                    const ProgressFn& progress = {}) override;

private:
    std::string apiPath(const std::string& path) const;
    std::string libraryPath(const std::string& uid, const std::string& rest = {}) const;
    Json request(const char* method, const std::string& path, const Json* body = nullptr);

    std::string baseUrl_;
    std::string prefix_;  // reverse-proxy path prefix without trailing '/', may be empty
    std::unique_ptr<httplib::Client> client_;
};

} // namespace on::sync
