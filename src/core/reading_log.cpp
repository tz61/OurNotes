#include "core/reading_log.h"

#include <algorithm>

#include "core/library.h"
#include "core/util.h"

namespace on {

std::optional<int64_t> readingLogLibrary(Library& lib)
{
    for (auto& l : lib.libraries())
        if (l.type == "user")
            return l.id;
    return std::nullopt;
}

std::string readingLogKey(const std::string& day, const std::string& deviceId)
{
    return kReadingLogPrefix + day + "_" + deviceId;
}

Json toJson(const ReadingSession& s)
{
    return Json{{"start", epochToIso(s.start)}, {"end", epochToIso(s.end)}, {"library", s.library},
                {"item", s.item},           {"attachment", s.attachment}, {"title", s.title}};
}

std::optional<ReadingSession> readingSessionFromJson(const Json& j)
{
    if (!j.is_object())
        return std::nullopt;
    ReadingSession s;
    s.start = isoToEpoch(jstr(j, "start"));
    s.end = isoToEpoch(jstr(j, "end"));
    s.library = jstr(j, "library");
    s.item = jstr(j, "item");
    s.attachment = jstr(j, "attachment");
    s.title = jstr(j, "title");
    if (s.start <= 0 || s.end < s.start)
        return std::nullopt;
    return s;
}

void mergeReadingSession(std::vector<ReadingSession>& sessions, const ReadingSession& session)
{
    for (auto& s : sessions)
        if (s.continues(session)) {
            auto device = s.device;
            s = session;
            if (s.device.empty())
                s.device = device;
            return;
        }
    sessions.push_back(session);
}

void recordReadingSession(Library& lib, int64_t libraryId, const std::string& day, const std::string& deviceId,
                          const std::string& deviceName, const ReadingSession& session)
{
    auto key = readingLogKey(day, deviceId);
    auto record = lib.setting(libraryId, key);
    if (!record.is_object())
        record = Json::object();
    std::vector<ReadingSession> sessions;
    if (record.contains("sessions") && record["sessions"].is_array())
        for (auto& j : record["sessions"])
            if (auto s = readingSessionFromJson(j))
                sessions.push_back(*s);
    mergeReadingSession(sessions, session);
    std::stable_sort(sessions.begin(), sessions.end(),
                     [](const ReadingSession& a, const ReadingSession& b) { return a.start < b.start; });
    Json list = Json::array();
    for (auto& s : sessions)
        list.push_back(toJson(s));
    record["device"] = deviceName;
    record["sessions"] = std::move(list);
    lib.setSetting(libraryId, key, record);
}

std::vector<ReadingSession> readingSessions(Library& lib, int64_t libraryId, const std::string& firstDay,
                                            const std::string& lastDay)
{
    std::vector<ReadingSession> out;
    // the names of a day's records are readingLog_<day>_<device>, and '~' comes after '_'
    for (auto& [key, record] : lib.settingsBetween(libraryId, kReadingLogPrefix + firstDay,
                                                   kReadingLogPrefix + lastDay + "~")) {
        if (!record.is_object() || !record.contains("sessions") || !record["sessions"].is_array())
            continue;
        auto device = jstr(record, "device");
        for (auto& j : record["sessions"])
            if (auto s = readingSessionFromJson(j)) {
                s->device = device;
                out.push_back(std::move(*s));
            }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const ReadingSession& a, const ReadingSession& b) { return a.start < b.start; });
    return out;
}

int64_t readingSeconds(const std::vector<ReadingSession>& sessions, int64_t from, int64_t to)
{
    std::vector<std::pair<int64_t, int64_t>> spans;
    for (auto& s : sessions) {
        auto a = std::max(s.start, from), b = std::min(s.end, to);
        if (b > a)
            spans.emplace_back(a, b);
    }
    std::sort(spans.begin(), spans.end());
    int64_t total = 0, reached = from;
    for (auto [a, b] : spans) {
        a = std::max(a, reached);
        if (b > a) {
            total += b - a;
            reached = b;
        }
    }
    return total;
}

} // namespace on
