#pragma once

#include <json.hpp>

#include <string_view>

namespace on {

// Ordered so item fields keep the order they were written in (matches schema order
// and keeps diffs/conflict views readable).
using Json = nlohmann::ordered_json;

// JSON from elsewhere (a request, a response): discarded when it is not JSON or is nested
// deeper than anything OurNotes writes. Parsed, a hundred thousand nested [ took the
// program down when copied or written out (nlohmann does both recursively): a push to the
// server did, and a response to the app.
inline Json parseExternalJson(std::string_view s, size_t maxDepth = 256)
{
    size_t depth = 0;
    bool inString = false, escaped = false;
    for (char c : s) {
        if (inString) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > maxDepth)
                return Json(Json::value_t::discarded);
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return Json::parse(s, nullptr, false);
}

inline std::string jstr(const Json& j, const char* key, const std::string& def = {})
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return def;
    return it->get<std::string>();
}

inline long long jint(const Json& j, const char* key, long long def = 0)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return def;
    return it->get<long long>();
}

inline bool jbool(const Json& j, const char* key, bool def = false)
{
    auto it = j.find(key);
    if (it == j.end())
        return def;
    if (it->is_boolean())
        return it->get<bool>();
    if (it->is_number())
        return it->get<long long>() != 0;
    return def;
}

} // namespace on
