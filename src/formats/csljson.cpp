#include "formats/csljson.h"

#include <map>

#include "core/schema.h"
#include "core/util.h"
#include "formats/common.h"

namespace fp::formats {

namespace {

const Json& section(const Json& j, const char* key)
{
    static const Json empty = Json::object();
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? *it : empty;
}

const Json& textFields() { return section(section(schema::cslMappings(), "fields"), "text"); }
const Json& dateFields() { return section(section(schema::cslMappings(), "fields"), "date"); }

std::string asString(const Json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    if (v.is_number())
        return v.dump();
    return {};
}

Json dateToCsl(const std::string& value)
{
    auto d = parseDate(value);
    if (!d.valid())
        return Json{{"raw", value}};
    Json parts = Json::array({d.year});
    if (d.month) {
        parts.push_back(d.month);
        if (d.day)
            parts.push_back(d.day);
    }
    return Json{{"date-parts", Json::array({parts})}};
}

std::string dateFromCsl(const Json& v)
{
    if (v.is_string() || v.is_number()) {
        auto s = asString(v);
        auto d = parseDate(s);
        return d.valid() ? isoDate(d) : s;
    }
    if (!v.is_object())
        return {};
    auto dp = v.find("date-parts");
    if (dp != v.end() && dp->is_array() && !dp->empty() && (*dp)[0].is_array()) {
        const auto& p = (*dp)[0];
        auto num = [&](size_t i) {
            if (i >= p.size())
                return 0;
            auto s = asString(p[i]);
            try {
                return s.empty() ? 0 : std::stoi(s);
            } catch (...) {
                return 0;
            }
        };
        DateParts d{num(0), num(1), num(2)};
        if (d.month < 1 || d.month > 12)
            d.month = d.day = 0;
        if (d.day < 1 || d.day > 31)
            d.day = 0;
        if (d.valid())
            return isoDate(d);
    }
    for (const char* key : {"raw", "literal"}) {
        auto s = jstr(v, key);
        if (!s.empty()) {
            auto d = parseDate(s);
            return d.valid() ? isoDate(d) : s;
        }
    }
    return {};
}

std::string zoteroType(const std::string& cslType)
{
    const auto& types = section(schema::cslMappings(), "types");
    auto it = types.find(cslType);
    if (it != types.end() && it->is_array() && !it->empty())
        return (*it)[0].get<std::string>();
    static const std::map<std::string, std::string> extra = {
        {"review", "journalArticle"},  {"review-book", "journalArticle"}, {"entry", "encyclopediaArticle"},
        {"collection", "book"},        {"pamphlet", "book"},              {"classic", "book"},
        {"periodical", "journalArticle"}, {"event", "presentation"},      {"performance", "presentation"},
        {"regulation", "statute"},     {"treaty", "statute"}};
    auto e = extra.find(cslType);
    return e == extra.end() ? "document" : e->second;
}

Json cslName(const Json& n)
{
    auto literal = jstr(n, "literal");
    if (!literal.empty())
        return literalName(literal);
    auto family = jstr(n, "family");
    auto given = jstr(n, "given");
    auto particle = jstr(n, "non-dropping-particle");
    if (!particle.empty())
        family = particle + " " + family;
    auto dropping = jstr(n, "dropping-particle");
    if (!dropping.empty())
        given = given.empty() ? dropping : given + " " + dropping;
    auto suffix = jstr(n, "suffix");
    if (!suffix.empty())
        given += ", " + suffix;
    if (family.empty() && given.empty())
        return nullptr;
    return personName(given, family);
}

} // namespace

Json toCsl(const Json& item)
{
    const auto& map = schema::cslMappings();
    auto type = jstr(item, "itemType");
    Json csl = Json::object();
    auto id = jstr(item, "key");
    if (id.empty())
        id = jstr(item, "citationKey");
    csl["id"] = id.empty() ? newKey() : id;

    std::string cslType = "document";
    for (auto& [t, list] : section(map, "types").items())
        for (auto& z : list)
            if (z == type)
                cslType = t;
    csl["type"] = cslType;

    for (auto& [var, zFields] : textFields().items()) {
        if (var == "shortTitle")  // same field as "title-short"
            continue;
        for (auto& zf : zFields) {
            auto v = schema::getField(item, zf.get<std::string>());
            if (!v.empty()) {
                csl[var] = v;
                break;
            }
        }
    }
    for (auto& [var, zf] : dateFields().items()) {
        auto v = schema::getField(item, zf.get<std::string>());
        if (!v.empty())
            csl[var] = dateToCsl(v);
    }

    const auto& names = section(map, "names");
    for (auto& c : item.value("creators", Json::array())) {
        auto role = jstr(names, jstr(c, "creatorType").c_str());
        if (role.empty())
            continue;
        Json n = Json::object();
        if (!jstr(c, "name").empty()) {
            n["literal"] = jstr(c, "name");
        } else {
            n["family"] = jstr(c, "lastName");
            n["given"] = jstr(c, "firstName");
        }
        csl[role].push_back(n);
    }

    std::vector<std::string> tags;
    for (auto& t : item.value("tags", Json::array()))
        if (!jstr(t, "tag").empty())
            tags.push_back(jstr(t, "tag"));
    if (!tags.empty())
        csl["keyword"] = join(tags, ", ");
    return csl;
}

Json fromCsl(const Json& csl)
{
    auto type = zoteroType(jstr(csl, "type"));
    if (!schema::isValidType(type) || !schema::isRegularType(type))
        type = "document";
    Json item = newItem(type);

    for (auto& [var, zFields] : textFields().items()) {
        auto it = csl.find(var);
        if (it == csl.end())
            continue;
        auto v = asString(*it);
        for (auto& zf : zFields)
            if (setField(item, zf.get<std::string>(), v))
                break;
    }
    if (jstr(item, "journalAbbreviation").empty())
        setField(item, "journalAbbreviation", jstr(csl, "container-title-short"));
    if (!csl.contains("event-title")) {  // CSL 1.0.1 name
        if (!setField(item, "conferenceName", jstr(csl, "event")))
            setField(item, "meetingName", jstr(csl, "event"));
    }
    for (auto& [var, zf] : dateFields().items()) {
        auto it = csl.find(var);
        if (it != csl.end())
            setField(item, zf.get<std::string>(), dateFromCsl(*it));
    }

    std::map<std::string, std::string> roles;  // csl role -> creator type
    for (auto& [ztype, role] : section(schema::cslMappings(), "names").items())
        roles.emplace(role.get<std::string>(), ztype);
    for (auto& [key, value] : csl.items()) {
        auto role = roles.find(key);
        if (role == roles.end() || !value.is_array())
            continue;
        for (auto& n : value) {
            auto c = cslName(n);
            if (!c.is_null())
                addCreator(item, role->second, c);
        }
    }

    for (auto& tag : split(jstr(csl, "keyword"), ','))
        addTag(item, tag);
    return finishItem(item);
}

std::string exportCslJson(const std::vector<Json>& items)
{
    Json out = Json::array();
    for (auto& item : items)
        if (isExportable(item))
            out.push_back(toCsl(item));
    return out.dump(2) + "\n";
}

std::vector<Json> importCslJson(const std::string& text)
{
    Json doc;
    try {
        doc = Json::parse(text);
    } catch (const std::exception& e) {
        throw Error(std::string("Not valid CSL JSON: ") + e.what());
    }
    if (doc.is_object() && doc.contains("items") && doc["items"].is_array())
        doc = doc["items"];
    if (doc.is_object())
        doc = Json::array({doc});
    if (!doc.is_array())
        throw Error("Not valid CSL JSON: expected an array of items");
    std::vector<Json> out;
    for (auto& entry : doc)
        if (entry.is_object())
            out.push_back(fromCsl(entry));
    return out;
}

} // namespace fp::formats
