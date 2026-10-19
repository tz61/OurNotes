#pragma once

#include <fstream>

#include "core/hash.h"
#include "core/sync/protocol.h"
#include "core/util.h"

namespace on::test {

// A fresh directory under the system temp dir, removed with its contents.
struct TempDir {
    fs::path path;

    TempDir() : path(fs::temp_directory_path() / ("ournotes-sync-test-" + randomToken(8)))
    {
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    fs::path operator/(const std::string& name) const { return path / u8path(name); }
};

inline void writeBytes(const fs::path& path, const std::string& data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), (std::streamsize)data.size());
}

// Deterministic pseudo-random bytes (xorshift32).
inline std::string pseudoRandom(size_t n, uint32_t seed)
{
    std::string out(n, '\0');
    uint32_t x = seed ? seed : 1;
    for (auto& c : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = (char)(x & 0xFF);
    }
    return out;
}

inline sync::PushObject put(const std::string& type, const std::string& key, int64_t baseVersion, Json data)
{
    sync::PushObject o;
    o.type = type;
    o.key = key;
    o.baseVersion = baseVersion;
    o.data = std::move(data);
    return o;
}

inline sync::PushObject del(const std::string& type, const std::string& key, int64_t baseVersion)
{
    sync::PushObject o;
    o.type = type;
    o.key = key;
    o.baseVersion = baseVersion;
    o.deleted = true;
    return o;
}

inline Json item(const std::string& title) { return Json{{"itemType", "book"}, {"title", title}}; }

// HTTP-like status of the SyncError thrown by fn, -1 if it does not throw.
template <typename F>
int statusOf(F&& fn)
{
    try {
        fn();
    } catch (const sync::SyncError& e) {
        return e.status;
    }
    return -1;
}

} // namespace on::test
