#include "core/config.h"

#include <cstdlib>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fp {

static std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

fs::path homeDir()
{
#ifdef _WIN32
    auto h = env("USERPROFILE");
#else
    auto h = env("HOME");
#endif
    return h.empty() ? fs::current_path() : u8path(h);
}

fs::path configDir()
{
#if defined(_WIN32)
    auto appdata = env("APPDATA");
    return (appdata.empty() ? homeDir() : u8path(appdata)) / "freePen";
#elif defined(__APPLE__)
    return homeDir() / "Library" / "Application Support" / "freePen";
#else
    auto xdg = env("XDG_CONFIG_HOME");
    return (xdg.empty() ? homeDir() / ".config" : u8path(xdg)) / "freepen";
#endif
}

fs::path defaultDataDir()
{
    auto e = env("FREEPEN_DATA_DIR");
    if (!e.empty())
        return u8path(e);
    return homeDir() / "freePen";
}

// --- SyncTarget -------------------------------------------------------------------

Json SyncTarget::toJson() const
{
    return Json{{"id", id},     {"name", name},         {"kind", kind},   {"url", url},
                {"path", path}, {"username", username}, {"token", token}, {"verifyTls", verifyTls}};
}

SyncTarget SyncTarget::fromJson(const Json& j)
{
    SyncTarget t;
    t.id = jstr(j, "id");
    t.name = jstr(j, "name");
    t.kind = jstr(j, "kind", "remote");
    t.url = jstr(j, "url");
    t.path = jstr(j, "path");
    t.username = jstr(j, "username");
    t.token = jstr(j, "token");
    t.verifyTls = jbool(j, "verifyTls", true);
    return t;
}

std::string SyncTarget::describe() const
{
    if (kind == "local")
        return name + " (folder " + path + ")";
    return name + " (" + (username.empty() ? "" : username + "@") + url + ")";
}

// --- Config -------------------------------------------------------------------------

static Json defaults()
{
    return Json{
        {"dataDir", nullptr},
        {"sync", {{"targets", Json::array()}, {"active", nullptr}, {"auto", true}, {"intervalMinutes", 15},
                  {"files", "on_sync"}}},
        {"ui", {{"openPdfIn", "tab"}, {"citationStyle", "apa"}, {"readerZoom", "fit_width"}}},
        {"fulltext", {{"enabled", true}}},
    };
}

static void mergeInto(Json& base, const Json& over)
{
    for (auto& [k, v] : over.items()) {
        if (v.is_object() && base.contains(k) && base[k].is_object())
            mergeInto(base[k], v);
        else
            base[k] = v;
    }
}

Config::Config(fs::path path) : path_(path.empty() ? configDir() / "config.json" : std::move(path))
{
    data_ = defaults();
    std::error_code ec;
    if (fs::exists(path_, ec)) {
        try {
            mergeInto(data_, Json::parse(readFile(path_)));
        } catch (const std::exception&) {
            // keep defaults; a broken config must not prevent startup
        }
    }
}

void Config::save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    writeFileAtomic(path_, data_.dump(2));
#ifndef _WIN32
    ::chmod(pathString(path_).c_str(), 0600);
#endif
}

fs::path Config::dataDir() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto d = data_.value("dataDir", Json());
    if (d.is_string() && !d.get<std::string>().empty())
        return u8path(d.get<std::string>());
    return defaultDataDir();
}

void Config::setDataDir(const fs::path& dir)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_["dataDir"] = pathString(dir);
}

Json Config::get(const std::string& pointer, const Json& def) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto ptr = Json::json_pointer(pointer);
    return data_.contains(ptr) ? data_.at(ptr) : def;
}

void Config::set(const std::string& pointer, const Json& value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_[Json::json_pointer(pointer)] = value;
}

std::vector<SyncTarget> Config::targets() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SyncTarget> out;
    for (auto& t : data_["sync"]["targets"])
        out.push_back(SyncTarget::fromJson(t));
    return out;
}

std::optional<SyncTarget> Config::target(const std::string& id) const
{
    for (auto& t : targets())
        if (t.id == id)
            return t;
    return std::nullopt;
}

std::optional<SyncTarget> Config::activeTarget() const
{
    auto active = get("/sync/active");
    if (active.is_string())
        if (auto t = target(active.get<std::string>()))
            return t;
    auto all = targets();
    if (!all.empty())
        return all.front();
    return std::nullopt;
}

void Config::upsertTarget(const SyncTarget& t)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& arr = data_["sync"]["targets"];
    for (auto& existing : arr)
        if (jstr(existing, "id") == t.id) {
            existing = t.toJson();
            return;
        }
    arr.push_back(t.toJson());
}

void Config::removeTarget(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& arr = data_["sync"]["targets"];
    Json kept = Json::array();
    for (auto& t : arr)
        if (jstr(t, "id") != id)
            kept.push_back(t);
    arr = kept;
    if (data_["sync"]["active"] == id)
        data_["sync"]["active"] = nullptr;
}

void Config::setActiveTarget(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_["sync"]["active"] = id;
}

} // namespace fp
