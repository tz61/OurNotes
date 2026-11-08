#include <doctest.h>

#include "core/library.h"
#include "core/reading_log.h"
#include "test_helpers.h"

using namespace on;

namespace {

ReadingSession session(int64_t start, int64_t end, const std::string& attachment = "ATTACH01")
{
    ReadingSession s;
    s.start = start;
    s.end = end;
    s.library = "LIB";
    s.attachment = attachment;
    s.item = "PARENT01";
    s.title = "A Paper";
    return s;
}

} // namespace

TEST_CASE("reading log: kept in the user's own library, never a group's")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto group = lib.createLibrary("Shared", "group");
    CHECK_FALSE(readingLogLibrary(lib));  // (only a group's)
    auto own = lib.createLibrary("Mine", "user");
    REQUIRE(readingLogLibrary(lib));
    CHECK(*readingLogLibrary(lib) == own);
    CHECK(group != own);
    CHECK(readingLogKey("20261107", "DEVICE01") == "readingLog_20261107_DEVICE01");
}

TEST_CASE("reading log: a day's record of a computer, a session gone on written in place of itself")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto t = isoToEpoch("2026-11-07T09:00:00Z");
    recordReadingSession(lib, id, "20261107", "DEVICE01", "desk", session(t + 3600, t + 3700, "LATER001"));
    recordReadingSession(lib, id, "20261107", "DEVICE01", "desk", session(t, t + 300));
    recordReadingSession(lib, id, "20261107", "DEVICE01", "desk", session(t, t + 900));  // the same, longer

    auto record = lib.setting(id, "readingLog_20261107_DEVICE01");
    CHECK(record["device"] == "desk");
    REQUIRE(record["sessions"].size() == 2);
    CHECK(record["sessions"][0] == Json{{"start", "2026-11-07T09:00:00Z"}, {"end", "2026-11-07T09:15:00Z"},
                                        {"library", "LIB"},  {"item", "PARENT01"},
                                        {"attachment", "ATTACH01"}, {"title", "A Paper"}});
    CHECK(record["sessions"][1]["attachment"] == "LATER001");
    // a synced setting, sent at the next sync
    bool dirty = false;
    for (auto& o : lib.dirtyObjects(id))
        dirty = dirty || (o.type == "setting" && o.key == "readingLog_20261107_DEVICE01");
    CHECK(dirty);
}

TEST_CASE("reading log: every computer's sessions of some days; a time read on two at once counts once")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto t = isoToEpoch("2026-11-07T09:00:00Z");
    recordReadingSession(lib, id, "20261106", "DEVICE01", "desk", session(t - 86400, t - 86400 + 600));
    recordReadingSession(lib, id, "20261107", "DEVICE01", "desk", session(t, t + 600));
    recordReadingSession(lib, id, "20261107", "DEVICE02", "laptop", session(t + 300, t + 1200, "OTHER001"));
    recordReadingSession(lib, id, "20261108", "DEVICE01", "desk", session(t + 86400, t + 86400 + 60));
    lib.setSetting(id, "readingLogs", Json{{"sessions", Json::array({toJson(session(t, t + 5))})}});  // (not one)

    auto day = readingSessions(lib, id, "20261107", "20261107");
    REQUIRE(day.size() == 2);
    CHECK(day[0].device == "desk");
    CHECK(day[0].seconds() == 600);
    CHECK(day[1].device == "laptop");
    CHECK(day[1].attachment == "OTHER001");
    CHECK(readingSeconds(day, t, t + 86400) == 1200);  // 09:00 to 09:20, the overlap once
    CHECK(readingSeconds(day, t + 900, t + 86400) == 300);  // (cut where asked)
    CHECK(readingSessions(lib, id, "20261106", "20261108").size() == 4);
    CHECK(readingSessions(lib, id, "20261109", "20261130").empty());

    // the session going on, with those written
    auto merged = day;
    mergeReadingSession(merged, session(t, t + 700));
    CHECK(merged.size() == 2);
    CHECK(merged[0].seconds() == 700);
    CHECK(merged[0].device == "desk");
    mergeReadingSession(merged, session(t + 5000, t + 5060));
    CHECK(merged.size() == 3);

    // what is no session is passed over
    CHECK_FALSE(readingSessionFromJson(Json{{"start", "2026-11-07T09:00:00Z"}, {"end", "2026-11-07T08:00:00Z"}}));
    CHECK_FALSE(readingSessionFromJson(Json::array()));
}
