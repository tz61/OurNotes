#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace on {

class Library;

// Reading time: the stretches of time PDFs were read in the app. They are kept as synced
// settings of the user's own library (never a group's, whose members would see them), one
// for each day and computer, readingLog_<YYYYMMDD>_<device>; only that computer writes it,
// so two never conflict. docs/sync-protocol.md describes them.
struct ReadingSession {
    int64_t start = 0;  // seconds since the epoch
    int64_t end = 0;
    std::string library;     // the uid of the PDF's library
    std::string attachment;  // the PDF's attachment
    std::string item;        // its top-level item (the attachment itself when it has none)
    std::string title;
    std::string device;  // the computer's name, from its record

    int64_t seconds() const { return end > start ? end - start : 0; }
    // The same session, gone on longer since it was written.
    bool continues(const ReadingSession& o) const
    {
        return start == o.start && attachment == o.attachment && library == o.library;
    }
};

inline constexpr const char* kReadingLogPrefix = "readingLog_";

// The library reading time is kept in: the first of type "user"; none without one.
std::optional<int64_t> readingLogLibrary(Library& lib);

// A day's record of a computer: day as YYYYMMDD, the date the sessions started on there.
std::string readingLogKey(const std::string& day, const std::string& deviceId);

// Writes a session into its day's record, in place of the one it continues.
void recordReadingSession(Library& lib, int64_t libraryId, const std::string& day, const std::string& deviceId,
                          const std::string& deviceName, const ReadingSession& session);

// Every computer's sessions of the days from firstDay to lastDay (YYYYMMDD, both counted),
// in the order they started.
std::vector<ReadingSession> readingSessions(Library& lib, int64_t libraryId, const std::string& firstDay,
                                            const std::string& lastDay);

// `session` in place of the one it continues in `sessions`, or added.
void mergeReadingSession(std::vector<ReadingSession>& sessions, const ReadingSession& session);

// The seconds of reading from `from` to `to`; a time read on two computers at once counts once.
int64_t readingSeconds(const std::vector<ReadingSession>& sessions, int64_t from, int64_t to);

Json toJson(const ReadingSession& s);
std::optional<ReadingSession> readingSessionFromJson(const Json& j);

} // namespace on
