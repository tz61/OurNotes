#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace on {

namespace fs = std::filesystem;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Object keys use Zotero's alphabet so imported keys and new keys look alike.
std::string newKey();

// The disk `path` is on, in bytes: its size, what is free, and what is free to this user
// ({capacity, free, available}); null when it cannot be told.
Json diskSpace(const fs::path& path);
// The bytes of a file, 0 when it is not there.
int64_t fileSizeOr0(const fs::path& path);
bool isValidKey(std::string_view key);

std::string nowIso();                        // 2026-01-31T12:00:00Z
std::string sqlToIso(const std::string& s);  // Zotero "YYYY-MM-DD HH:MM:SS" -> ISO
int64_t isoToEpoch(const std::string& iso);  // seconds, 0 on failure
std::string epochToIso(int64_t seconds);
std::string isoToLocalDisplay(const std::string& iso);
int64_t nowMillis();

std::string yearOf(const std::string& date);

void appendUtf8(std::string& out, uint32_t codepoint);

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

// Split a quick-search string into words, keeping "quoted phrases" together; folded
// (lowercase, no accents: as the stored search text is) unless fold is false.
std::vector<std::string> searchTokens(std::string_view query, bool fold = true);

std::string htmlEscape(std::string_view s);
std::string htmlUnescape(std::string_view s);
std::string htmlToText(std::string_view html);
std::string noteTitle(std::string_view html);
std::string textToNoteHtml(std::string_view text);
// Markdown as a note (Zotero's note HTML): paragraphs, # headings (### and deeper as
// level 3), - and 1. lists, > quotes, ``` code blocks, --- rules, | tables |, **bold**,
// *italic*, `code`, [links](url), and $…$ / $$…$$ formulas as the app keeps them.
std::string markdownToNoteHtml(std::string_view markdown);
// A row of a Markdown table (GitHub's): its cells, trimmed, "\|" in them as "|". Empty
// when the line has no |.
std::vector<std::string> markdownTableCells(std::string_view line);
// A Markdown table's delimiter row ("|---|:-:|--:|"): each column's alignment, 'l', 'c'
// or 'r'. Empty when the line is not one.
std::string markdownTableAlignments(std::string_view line);

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

} // namespace on
