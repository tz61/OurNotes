#pragma once

#include <json.hpp>

namespace on {

// Ordered so item fields keep the order they were written in (matches schema order
// and keeps diffs/conflict views readable).
using Json = nlohmann::ordered_json;

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
