#include "core/schema.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <set>

#include "core/util.h"

extern const unsigned char on_schema_json[];
extern const std::size_t on_schema_json_size;

namespace on::schema {

const std::vector<Color> kAnnotationColors = {
    {"Yellow", "#ffd400"}, {"Red", "#ff6666"},     {"Green", "#5fb236"},  {"Blue", "#2ea8e5"},
    {"Purple", "#a28ae5"}, {"Magenta", "#e56eee"}, {"Orange", "#f19837"}, {"Gray", "#aaaaaa"}};

const char* const kLinkModes[] = {"imported_file", "imported_url", "linked_file", "linked_url", "embedded_image"};

namespace {

struct TypeInfo {
    std::vector<std::string> fields;
    std::map<std::string, std::string> baseMap;
    std::vector<std::string> creatorTypes;
    std::string primary = "author";
};

struct Schema {
    Json raw;
    std::map<std::string, TypeInfo> types;
    std::vector<std::string> order;
};

const Schema& get()
{
    static Schema s;
    static std::once_flag once;
    std::call_once(once, [] {
        s.raw = Json::parse(on_schema_json, on_schema_json + on_schema_json_size);
        for (auto& t : s.raw["itemTypes"]) {
            TypeInfo info;
            for (auto& f : t["fields"]) {
                info.fields.push_back(f["field"]);
                if (f.contains("baseField"))
                    info.baseMap[f["field"]] = f["baseField"];
            }
            for (auto& c : t["creatorTypes"]) {
                info.creatorTypes.push_back(c["creatorType"]);
                if (c.value("primary", false))
                    info.primary = c["creatorType"];
            }
            std::string name = t["itemType"];
            s.order.push_back(name);
            s.types.emplace(name, std::move(info));
        }
    });
    return s;
}

std::string humanize(const std::string& name)
{
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (i && std::isupper((unsigned char)c) && std::islower((unsigned char)name[i - 1]))
            out += ' ';
        out += i == 0 ? (char)std::toupper((unsigned char)c) : c;
    }
    return out;
}

std::string label(const char* section, const std::string& key)
{
    const auto& loc = get().raw["locales"]["en-US"];
    if (loc.contains(section) && loc[section].contains(key))
        return loc[section][key].get<std::string>();
    return humanize(key);
}

const std::set<std::string> kMetaKeys = {"key",         "version", "itemType",  "dateAdded", "dateModified",
                                         "tags",        "collections", "relations", "creators", "deleted",
                                         "parentItem"};

} // namespace

bool isRegularType(const std::string& t) { return t != "note" && t != "attachment" && t != "annotation"; }

bool isStoredLinkMode(const std::string& m)
{
    return m == "imported_file" || m == "imported_url" || m == "embedded_image";
}

std::vector<std::string> itemTypes(bool regularOnly)
{
    std::vector<std::string> out;
    for (auto& name : get().order)
        if (!regularOnly || isRegularType(name))
            out.push_back(name);
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return normalize(typeLabel(a)) < normalize(typeLabel(b)); });
    return out;
}

bool isValidType(const std::string& t) { return get().types.count(t) > 0; }

std::vector<std::string> fieldsFor(const std::string& t)
{
    auto it = get().types.find(t);
    return it == get().types.end() ? std::vector<std::string>{} : it->second.fields;
}

bool hasField(const std::string& t, const std::string& field)
{
    auto it = get().types.find(t);
    return it != get().types.end() &&
           std::find(it->second.fields.begin(), it->second.fields.end(), field) != it->second.fields.end();
}

std::map<std::string, std::string> baseFieldMap(const std::string& t)
{
    auto it = get().types.find(t);
    return it == get().types.end() ? std::map<std::string, std::string>{} : it->second.baseMap;
}

std::string getField(const Json& data, const std::string& base)
{
    auto v = jstr(data, base.c_str());
    if (!v.empty())
        return v;
    for (auto& [field, b] : baseFieldMap(jstr(data, "itemType")))
        if (b == base) {
            v = jstr(data, field.c_str());
            if (!v.empty())
                return v;
        }
    return {};
}

std::vector<std::string> creatorTypesFor(const std::string& t)
{
    auto it = get().types.find(t);
    return it == get().types.end() ? std::vector<std::string>{} : it->second.creatorTypes;
}

std::string primaryCreatorType(const std::string& t)
{
    auto it = get().types.find(t);
    return it == get().types.end() ? "author" : it->second.primary;
}

std::string typeLabel(const std::string& t) { return label("itemTypes", t); }
std::string fieldLabel(const std::string& f) { return label("fields", f); }
std::string creatorLabel(const std::string& c) { return label("creatorTypes", c); }

const Json& cslMappings()
{
    static const Json empty = Json::object();
    const auto& raw = get().raw;
    return raw.contains("csl") ? raw["csl"] : empty;
}

Json convertType(const Json& data, const std::string& newType, Json* dropped)
{
    auto oldMap = baseFieldMap(jstr(data, "itemType"));
    auto newFields = fieldsFor(newType);
    std::map<std::string, std::string> newBaseRev;
    for (auto& [f, b] : baseFieldMap(newType))
        newBaseRev[b] = f;
    auto has = [&](const std::string& f) { return std::find(newFields.begin(), newFields.end(), f) != newFields.end(); };

    Json out = Json::object();
    if (dropped)
        *dropped = Json::object();
    for (auto& [k, v] : data.items()) {
        if (kMetaKeys.count(k)) {
            out[k] = v;
            continue;
        }
        std::string base = oldMap.count(k) ? oldMap[k] : k;
        std::string target;
        if (has(k))
            target = k;
        else if (newBaseRev.count(base))
            target = newBaseRev[base];
        else if (has(base))
            target = base;
        if (!target.empty())
            out[target] = v;
        else if (dropped && v.is_string() && !v.get<std::string>().empty())
            (*dropped)[k] = v;
    }
    out["itemType"] = newType;
    auto allowed = creatorTypesFor(newType);
    auto prim = primaryCreatorType(newType);
    Json creators = Json::array();
    for (auto c : data.value("creators", Json::array())) {
        if (std::find(allowed.begin(), allowed.end(), jstr(c, "creatorType")) == allowed.end())
            c["creatorType"] = prim;
        creators.push_back(c);
    }
    out["creators"] = creators;
    return out;
}

} // namespace on::schema
