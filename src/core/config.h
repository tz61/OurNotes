#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "core/json.h"
#include "core/util.h"

namespace fp {

fs::path homeDir();
fs::path configDir();       // ~/.config/freepen, ~/Library/Application Support/freePen, %APPDATA%\freePen
fs::path defaultDataDir();  // $FREEPEN_DATA_DIR or ~/freePen

// A place the library can be synced with.
struct SyncTarget {
    std::string id;
    std::string name;
    std::string kind;  // "remote" (freepend server) or "local" (a folder)
    std::string url;   // remote
    std::string path;  // local
    std::string username;
    std::string token;
    bool verifyTls = true;

    Json toJson() const;
    static SyncTarget fromJson(const Json& j);
    std::string describe() const;
};

// User configuration, stored as JSON (mode 0600: it holds sync tokens).
class Config {
public:
    explicit Config(fs::path path = {});
    void save();

    const fs::path& path() const { return path_; }
    fs::path dataDir() const;
    void setDataDir(const fs::path& dir);

    Json& raw() { return data_; }
    Json get(const std::string& pointer, const Json& def = nullptr) const;  // "/ui/zoom"
    void set(const std::string& pointer, const Json& value);

    std::vector<SyncTarget> targets() const;
    std::optional<SyncTarget> target(const std::string& id) const;
    std::optional<SyncTarget> activeTarget() const;
    void upsertTarget(const SyncTarget& t);
    void removeTarget(const std::string& id);
    void setActiveTarget(const std::string& id);

private:
    fs::path path_;
    Json data_;
    mutable std::mutex mutex_;
};

} // namespace fp
