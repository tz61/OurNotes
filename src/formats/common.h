#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"
#include "core/util.h"

// Helpers shared by the importers, exporters and citation styles.
namespace fp::formats {

struct DateParts {
    int year = 0;
    int month = 0;  // 1-12, 0 = unknown
    int day = 0;    // 1-31, 0 = unknown
    bool valid() const { return year != 0; }
};

// Understands "2023-05-01", "2023-05", "2023", "May 2023", "12 May 2023", "May 12, 2023",
// "2015 May 28", "5/12/2023" (month first unless the first number is > 12), "12.05.2023"
// (day first), RIS "2023/05/01/" and ISO timestamps.
DateParts parseDate(std::string_view text);
std::string isoDate(const DateParts& d);   // "2023-05-01", "2023-05", "2023" or ""
int monthFromName(std::string_view name);  // "may", "Sept.", "January" -> 1..12, else 0
const char* monthName(int month);          // 1 -> "January", "" if out of range

// --- item construction ---------------------------------------------------------

bool isExportable(const Json& item);  // regular item (not note/attachment/annotation)
Json newItem(const std::string& itemType);

// Sets `field`, or the type-specific field whose base field is `field`, if the item
// type has it. Returns false (and leaves the item alone) otherwise or if value is empty.
bool setField(Json& item, const std::string& field, const std::string& value);

// Appends a creator; types the item does not allow fall back to the primary creator
// type (for "author") or "contributor". `creator` holds firstName/lastName or name.
bool addCreator(Json& item, const std::string& creatorType, const Json& creator);
Json personName(const std::string& first, const std::string& last);
Json literalName(const std::string& name);

// "Last, First" or "First von Last" -> {"firstName","lastName"}.
Json splitName(std::string_view name);

void addTag(Json& item, const std::string& tag);
void appendExtra(Json& item, const std::string& line);
// Value of a "Key: value" line in an Extra field (key compared case-insensitively).
std::string extraValue(const std::string& extra, std::string_view key);

// Canonical key order (itemType, title, creators, schema fields, tags, ...), empty and
// invalid fields dropped.
Json finishItem(const Json& item);

// --- text ----------------------------------------------------------------------

std::string collapseWhitespace(std::string_view s);
// Extracts a bare DOI from "10.x/y", "doi:10.x/y" or a doi.org URL; "" if none.
std::string cleanDoi(std::string_view s);
std::string stripArxivVersion(std::string_view id);
bool isArxivId(std::string_view id);  // 2301.12345[v2] or hep-th/9901001[v1]

// UTF-8 helpers
size_t utf8Len(unsigned char lead);
std::string firstChar(std::string_view s);  // first UTF-8 code point
using fp::appendUtf8;

} // namespace fp::formats
