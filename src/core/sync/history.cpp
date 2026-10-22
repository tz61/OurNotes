#include "core/sync/history.h"

#include <map>
#include <tuple>

namespace on::sync {

namespace {

using ObjectId = std::tuple<int64_t, std::string, std::string>;  // library, type, key

ObjectId objectId(const Json& change)
{
    return {change.value("libraryId", int64_t(0)), jstr(change, "type"), jstr(change, "key")};
}

bool isEmptySuccess(const Json& run)
{
    return run.value("ok", true) && run.value("changes", Json::array()).empty() && run.value("more", 0) == 0;
}

} // namespace

SyncHistory::SyncHistory(fs::path file, std::string by) : file_(std::move(file)), by_(std::move(by)) { load(); }

std::pair<fs::file_time_type, uintmax_t> SyncHistory::stamp() const
{
    std::error_code ec;
    auto time = fs::last_write_time(file_, ec);
    if (ec)
        return {};
    auto size = fs::file_size(file_, ec);
    return {time, ec ? 0 : size};
}

void SyncHistory::load()
{
    loaded_ = stamp();
    runs_ = Json::array();
    std::error_code ec;
    if (!fs::exists(file_, ec))
        return;
    try {
        auto j = Json::parse(readFile(file_));
        if (j.is_object() && j.value("runs", Json()).is_array())
            runs_ = j["runs"];
    } catch (const std::exception&) {
        // unreadable: start a new history
    }
}

bool SyncHistory::reload()
{
    if (stamp() == loaded_)
        return false;
    load();
    return true;
}

void SyncHistory::add(const std::string& target, const SyncReport& report, bool ok, const std::string& summary)
{
    Json changes = Json::array();
    for (auto it = report.changes.rbegin(); it != report.changes.rend(); ++it) {  // the last change first
        if ((int)changes.size() == kMaxChanges)
            break;
        Json c{{"libraryId", it->libraryId},
               {"library", it->library},
               {"type", it->type},
               {"key", it->key},
               {"kind", it->kind},
               {"title", it->title},
               {"parentKey", it->parentKey},
               {"action", it->action},
               {"incoming", it->incoming}};
        if (!it->by.empty())
            c["by"] = it->by;
        changes.push_back(std::move(c));
    }
    add(Json{{"time", nowIso()},
             {"target", target},
             {"ok", ok},
             {"summary", summary},
             {"errors", report.errors},
             {"changes", changes},
             {"more", (int)report.changes.size() - (int)changes.size()}});
}

void SyncHistory::addFailure(const std::string& target, const std::string& error)
{
    add(Json{{"time", nowIso()},
             {"target", target},
             {"ok", false},
             {"summary", ""},
             {"errors", Json::array({error})},
             {"changes", Json::array()},
             {"more", 0}});
}

void SyncHistory::add(Json run)
{
    if (!by_.empty())
        run["by"] = by_;
    load();  // with what another program added meanwhile
    std::map<ObjectId, std::string> latest;  // the objects of this sync, and their titles
    for (auto& c : run["changes"])
        latest[objectId(c)] = jstr(c, "title");
    for (auto& old : runs_) {
        auto& changes = old["changes"];
        for (size_t i = 0; i < changes.size();) {
            auto it = latest.find(objectId(changes[i]));
            if (it == latest.end()) {
                ++i;
                continue;
            }
            // listed again under this sync; a deletion sent from here knows no title
            if (it->second.empty() && !jstr(changes[i], "title").empty())
                for (auto& c : run["changes"])
                    if (objectId(c) == it->first)
                        c["title"] = changes[i]["title"];
            changes.erase(changes.begin() + i);
        }
    }
    runs_.insert(runs_.begin(), std::move(run));
    // a sync that changed nothing is only worth showing as the last one
    for (size_t i = 1; i < runs_.size();) {
        if (isEmptySuccess(runs_[i]))
            runs_.erase(runs_.begin() + i);
        else
            ++i;
    }
    while (runs_.size() > (size_t)kMaxRuns)
        runs_.erase(runs_.end() - 1);
    save();
}

void SyncHistory::save()
{
    try {
        writeFileAtomic(file_, Json{{"runs", runs_}}.dump(1));
        loaded_ = stamp();
    } catch (const std::exception&) {
        // the history is a convenience; syncing goes on without it
    }
}

} // namespace on::sync
