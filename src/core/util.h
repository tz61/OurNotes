#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace fp {

namespace fs = std::filesystem;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Object keys use Zotero's alphabet so imported keys and new keys look alike.
std::string newKey();
bool isValidKey(std::string_view key);

std::string nowIso();                        // 2026-01-31T12:00:00Z
std::string sqlToIso(const std::string& s);  // Zotero "YYYY-MM-DD HH:MM:SS" -> ISO
int64_t isoToEpoch(const std::string& iso);  // seconds, 0 on failure
std::string epochToIso(int64_t seconds);
std::string isoToLocalDisplay(const std::string& iso);
int64_t nowMillis();

std::string yearOf(const std::string& date);

// Lowercased, accent-stripped form used for sorting and searching.
std::string normalize(std::string_view text);

std::string trim(std::string_view s);
std::string toLower(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool iequals(std::string_view a, std::string_view b);
std::vector<std::string> split(std::string_view s, char sep, bool skipEmpty = true);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

// Split a quick-search string into words, keeping "quoted phrases" together.
std::vector<std::string> searchTokens(std::string_view query);

std::string htmlEscape(std::string_view s);
std::string htmlUnescape(std::string_view s);
std::string htmlToText(std::string_view html);
std::string noteTitle(std::string_view html);
std::string textToNoteHtml(std::string_view text);

std::string safeFilename(std::string_view name, std::string_view fallback = "file");
std::string humanSize(uint64_t bytes);
std::string urlEncode(std::string_view s);
std::string urlDecode(std::string_view s);

std::string creatorName(const Json& creator);
std::string creatorSummary(const Json& creators, const std::string& primaryType = "author");

// UTF-8 aware path conversion (std::filesystem uses wide strings on Windows).
fs::path u8path(const std::string& utf8);
std::string pathString(const fs::path& p);

std::string readFile(const fs::path& path);
void writeFileAtomic(const fs::path& path, std::string_view data);
std::string guessContentType(const fs::path& path);

} // namespace fp
