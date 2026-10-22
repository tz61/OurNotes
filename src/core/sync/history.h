#pragma once

#include <string>
#include <utility>

#include "core/json.h"
#include "core/sync/engine.h"
#include "core/util.h"

namespace on::sync {

// The last syncs, kept in the data directory (sync-history.json) only: what each
// brought in and sent out. An object is listed once, under the last sync that
// changed it, so a note typed over several syncs is one entry. The app and
// ournotes-cli both add to it; each takes in what the other added first.
class SyncHistory {
public:
    static constexpr int kMaxRuns = 30;
    static constexpr int kMaxChanges = 200;  // per sync; the rest are counted

    // by: the program that syncs, when not the app ("ournotes-cli"); kept with its syncs
    explicit SyncHistory(fs::path file, std::string by = {});

    // Newest first: {time, target, by?, ok, summary, errors[], changes[], more}; a change
    // sent out has "by" when another program than the app made it (see Library::changedBy)
    const Json& runs() const { return runs_; }
    void add(const std::string& target, const SyncReport& report, bool ok, const std::string& summary);
    void addFailure(const std::string& target, const std::string& error);
    // Read the file again if another program wrote to it; true if it did.
    bool reload();

private:
    void add(Json run);
    void load();
    void save();
    std::pair<fs::file_time_type, uintmax_t> stamp() const;

    fs::path file_;
    std::string by_;
    Json runs_ = Json::array();
    // the file as last read or written: its time, and its size (the time may be the same
    // for two writes close together)
    std::pair<fs::file_time_type, uintmax_t> loaded_{};
};

} // namespace on::sync
