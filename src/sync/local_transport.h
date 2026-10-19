#pragma once

#include "core/sync/protocol.h"
#include "sync/store.h"

namespace on::sync {

// Sync with a folder (USB drive, NAS share, cloud-drive folder). The folder holds
// a Store; this transport acts as its implicit user "local", which owns every
// library created through it.
class LocalTransport : public Transport {
public:
    static constexpr const char* kUser = "local";

    explicit LocalTransport(const fs::path& folder);

    Store& store() { return store_; }

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
    RemoteLibrary requireLibrary(const std::string& uid, bool write);

    fs::path folder_;
    Store store_;
};

} // namespace on::sync
