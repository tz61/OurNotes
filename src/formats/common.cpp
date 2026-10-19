#include "formats/common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "core/schema.h"
#include "core/util.h"

namespace fp::formats {

namespace {

const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

struct DateToken {
    bool number = false;
    int value = 0;     // number value or month (1-12, 0 = other word)
    size_t length = 0; // digits in a number token
    char sepBefore = 0;
};

std::vector<DateToken> dateTokens(std::string_view s)
{
    std::vector<DateToken> out;
    char sep = 0;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (isDigit(c)) {
            DateToken t;
            t.number = true;
            t.sepBefore = sep;
            size_t start = i;
            while (i < s.size() && isDigit(s[i])) {
                if (i - start < 9)
                    t.value = t.value * 10 + (s[i] - '0');
                ++i;
            }
            t.length = i - start;
            // ordinal suffixes: 1st, 2nd, 3rd, 4th
            if (i + 1 < s.size() && isAlpha(s[i]) && isAlpha(s[i + 1]) &&
                (i + 2 >= s.size() || !isAlpha(s[i + 2]))) {
                auto suf = toLower(s.substr(i, 2));
                if (suf == "st" || suf == "nd" || suf == "rd" || suf == "th")
                    i += 2;
            }
            out.push_back(t);
            sep = 0;
        } else if (isAlpha(c)) {
            size_t start = i;
            while (i < s.size() && isAlpha(s[i]))
                ++i;
            DateToken t;
            t.value = monthFromName(s.substr(start, i - start));
            t.sepBefore = sep;
            out.push_back(t);
            sep = 0;
        } else {
            if (c == '-' || c == '/' || c == '.')
                sep = c;
            ++i;
        }
    }
    return out;
}

DateParts validated(DateParts d)
{
    if (d.month < 1 || d.month > 12) {
        d.month = 0;
        d.day = 0;
    }
    if (d.day < 1 || d.day > 31)
        d.day = 0;
    return d;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool isLowerWord(const std::string& w)
{
    return !w.empty() && w[0] >= 'a' && w[0] <= 'z';
}

bool isSuffix(const std::string& w)
{
    static const char* const kSuffixes[] = {"jr", "jr.", "sr", "sr.", "ii", "iii", "iv"};
    auto l = toLower(w);
    return std::any_of(std::begin(kSuffixes), std::end(kSuffixes), [&](const char* s) { return l == s; });
}

} // namespace

// --- dates ------------------------------------------------------------------------

int monthFromName(std::string_view name)
{
    std::string n = toLower(name);
    while (!n.empty() && n.back() == '.')
        n.pop_back();
    if (n.size() < 3)
        return 0;
    for (int m = 0; m < 12; ++m)
        if (startsWith(toLower(kMonths[m]), n))
            return m + 1;
    if (n == "sept")
        return 9;
    return 0;
}

const char* monthName(int month) { return month >= 1 && month <= 12 ? kMonths[month - 1] : ""; }

DateParts parseDate(std::string_view text)
{
    auto toks = dateTokens(text);
    DateParts d;
    if (toks.empty())
        return d;

    // Year first: ISO, RIS, PubMed ("2015 May 28")
    if (toks[0].number && toks[0].length == 4) {
        d.year = toks[0].value;
        if (toks.size() > 1) {
            const auto& t = toks[1];
            bool numericMonth = t.number && t.length <= 2;
            if (numericMonth || (!t.number && t.value)) {
                d.month = t.value;
                if (toks.size() > 2 && toks[2].number && toks[2].length <= 2)
                    d.day = toks[2].value;
            }
        }
        return validated(d);
    }

    int month = 0;
    std::vector<const DateToken*> small;
    char sep = 0;  // separator after the first small number ("5/12" vs "12.5")
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t.number && t.length == 4 && !d.year) {
            d.year = t.value;
        } else if (t.number && t.length <= 2 && small.size() < 2 && (!d.year || month)) {
            if (small.empty() && i + 1 < toks.size())
                sep = toks[i + 1].sepBefore;
            small.push_back(&t);
        } else if (!t.number && t.value && !month) {
            month = t.value;
        }
    }
    if (!d.year)
        return {};
    if (month) {
        d.month = month;
        if (!small.empty())
            d.day = small[0]->value;
    } else if (small.size() == 2) {
        int a = small[0]->value, b = small[1]->value;
        if (sep == '.' || a > 12) {
            d.day = a;
            d.month = b;
        } else {
            d.month = a;
            d.day = b;
        }
    } else if (small.size() == 1) {
        d.month = small[0]->value;
    }
    return validated(d);
}

std::string isoDate(const DateParts& d)
{
    if (!d.valid())
        return {};
    char buf[16];
    if (d.month && d.day)
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", d.year, d.month, d.day);
    else if (d.month)
        std::snprintf(buf, sizeof buf, "%04d-%02d", d.year, d.month);
    else
        std::snprintf(buf, sizeof buf, "%04d", d.year);
    return buf;
}

// --- items ------------------------------------------------------------------------

bool isExportable(const Json& item)
{
    auto type = jstr(item, "itemType");
    return schema::isValidType(type) && schema::isRegularType(type);
}

Json newItem(const std::string& itemType)
{
    return Json{{"itemType", itemType},
                {"creators", Json::array()},
                {"tags", Json::array()},
                {"collections", Json::array()},
                {"relations", Json::object()}};
}

bool setField(Json& item, const std::string& field, const std::string& value)
{
    auto v = trim(value);
    if (v.empty())
        return false;
    auto type = jstr(item, "itemType");
    if (schema::hasField(type, field)) {
        item[field] = v;
        return true;
    }
    for (auto& [specific, base] : schema::baseFieldMap(type)) {
        if (base == field) {
            item[specific] = v;
            return true;
        }
    }
    return false;
}

Json personName(const std::string& first, const std::string& last)
{
    return Json{{"firstName", trim(first)}, {"lastName", trim(last)}};
}

Json literalName(const std::string& name) { return Json{{"name", trim(name)}}; }

bool addCreator(Json& item, const std::string& creatorType, const Json& creator)
{
    auto itemType = jstr(item, "itemType");
    auto allowed = schema::creatorTypesFor(itemType);
    std::string type = creatorType;
    if (!contains(allowed, type)) {
        if (type == "author")
            type = schema::primaryCreatorType(itemType);
        else if (contains(allowed, "contributor"))
            type = "contributor";
        else
            return false;
    }
    Json c{{"creatorType", type}};
    if (!jstr(creator, "name").empty()) {
        c["name"] = jstr(creator, "name");
    } else {
        if (jstr(creator, "lastName").empty() && jstr(creator, "firstName").empty())
            return false;
        c["firstName"] = jstr(creator, "firstName");
        c["lastName"] = jstr(creator, "lastName");
    }
    if (!item.contains("creators") || !item["creators"].is_array())
        item["creators"] = Json::array();
    item["creators"].push_back(std::move(c));
    return true;
}

Json splitName(std::string_view name)
{
    auto full = collapseWhitespace(name);
    auto comma = full.find(',');
    if (comma != std::string::npos) {
        auto parts = split(full, ',');
        for (auto& p : parts)
            p = trim(p);
        std::string last = parts.empty() ? std::string() : parts[0];
        std::string first;
        if (parts.size() >= 3)
            first = parts[2] + ", " + parts[1];  // "King, Jr., Martin" -> "Martin, Jr."
        else if (parts.size() == 2)
            first = parts[1];
        return personName(first, last);
    }
    auto words = split(full, ' ');
    if (words.empty())
        return personName("", "");
    std::string suffix;
    if (words.size() > 2 && isSuffix(words.back())) {
        suffix = words.back();
        words.pop_back();
    }
    if (words.size() == 1)
        return personName(suffix.empty() ? "" : ", " + suffix, words[0]);
    // "von" part: the first lower-case word that is not the last one
    size_t lastStart = words.size() - 1;
    for (size_t i = 1; i + 1 < words.size(); ++i) {
        if (isLowerWord(words[i])) {
            lastStart = i;
            break;
        }
    }
    std::vector<std::string> first(words.begin(), words.begin() + (long)lastStart);
    std::vector<std::string> last(words.begin() + (long)lastStart, words.end());
    auto given = join(first, " ");
    if (!suffix.empty())
        given += ", " + suffix;
    return personName(given, join(last, " "));
}

void addTag(Json& item, const std::string& tag)
{
    auto t = trim(tag);
    if (t.empty())
        return;
    if (!item.contains("tags") || !item["tags"].is_array())
        item["tags"] = Json::array();
    for (auto& existing : item["tags"])
        if (jstr(existing, "tag") == t)
            return;
    item["tags"].push_back(Json{{"tag", t}, {"type", 0}});
}

void appendExtra(Json& item, const std::string& line)
{
    auto l = trim(line);
    if (l.empty())
        return;
    auto extra = jstr(item, "extra");
    item["extra"] = extra.empty() ? l : extra + "\n" + l;
}

std::string extraValue(const std::string& extra, std::string_view key)
{
    for (auto& line : split(extra, '\n')) {
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        if (iequals(trim(std::string_view(line).substr(0, colon)), key))
            return trim(std::string_view(line).substr(colon + 1));
    }
    return {};
}

Json finishItem(const Json& item)
{
    auto type = jstr(item, "itemType");
    Json out = Json::object();
    out["itemType"] = type;
    auto fields = schema::fieldsFor(type);
    bool creatorsDone = false;
    auto putCreators = [&] {
        out["creators"] = item.contains("creators") && item["creators"].is_array() ? item["creators"] : Json::array();
        creatorsDone = true;
    };
    for (auto& f : fields) {
        auto it = item.find(f);
        if (it != item.end() && it->is_string()) {
            auto v = trim(it->get<std::string>());
            if (!v.empty())
                out[f] = v;
        }
        if (!creatorsDone)
            putCreators();
    }
    if (!creatorsDone)
        putCreators();
    out["tags"] = item.contains("tags") && item["tags"].is_array() ? item["tags"] : Json::array();
    out["collections"] = item.contains("collections") ? item["collections"] : Json::array();
    out["relations"] = item.contains("relations") ? item["relations"] : Json::object();
    for (auto& [k, v] : item.items()) {
        if (out.contains(k) || (!k.empty() && k[0] != '_' && k != "key" && k != "version" && k != "dateAdded" &&
                                k != "dateModified"))
            continue;
        if (v.is_array() && v.empty())
            continue;
        out[k] = v;
    }
    return out;
}

// --- text -------------------------------------------------------------------------

std::string collapseWhitespace(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) {
            space = !out.empty();
        } else {
            if (space)
                out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}

std::string cleanDoi(std::string_view s)
{
    std::string t = trim(s);
    auto lower = toLower(t);
    auto host = lower.find("doi.org/");
    if (host != std::string::npos)
        t = urlDecode(t.substr(host + 8));
    else if (startsWith(lower, "doi:"))
        t = trim(t.substr(4));
    auto pos = t.find("10.");
    if (pos == std::string::npos)
        return {};
    t = t.substr(pos);
    size_t i = 3;
    while (i < t.size() && isDigit(t[i]))
        ++i;
    if (i < 7 || i >= t.size() || t[i] != '/' || i + 1 >= t.size())
        return {};
    auto end = t.find_first_of(" \t\r\n");
    return end == std::string::npos ? t : t.substr(0, end);
}

std::string stripArxivVersion(std::string_view id)
{
    auto v = id.rfind('v');
    if (v != std::string_view::npos && v + 1 < id.size() && v > 0 && isDigit(id[v - 1]) &&
        std::all_of(id.begin() + (long)v + 1, id.end(), isDigit))
        return std::string(id.substr(0, v));
    return std::string(id);
}

bool isArxivId(std::string_view id)
{
    auto base = stripArxivVersion(id);
    auto allDigits = [](std::string_view s) { return !s.empty() && std::all_of(s.begin(), s.end(), isDigit); };
    // new style: YYMM.NNNN(N)
    auto dot = base.find('.');
    if (dot == 4 && allDigits(std::string_view(base).substr(0, 4))) {
        auto num = std::string_view(base).substr(5);
        int mm = (base[2] - '0') * 10 + (base[3] - '0');
        return allDigits(num) && (num.size() == 4 || num.size() == 5) && mm >= 1 && mm <= 12;
    }
    // old style: archive(.SC)/YYMMNNN
    auto slash = base.find('/');
    if (slash == std::string::npos || slash == 0)
        return false;
    auto archive = std::string_view(base).substr(0, slash);
    auto num = std::string_view(base).substr(slash + 1);
    if (num.size() != 7 || !allDigits(num))
        return false;
    auto sub = archive.find('.');
    auto name = archive.substr(0, sub);
    if (!std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; }) ||
        name.front() == '-')
        return false;
    if (sub != std::string_view::npos) {
        auto cls = archive.substr(sub + 1);
        return cls.size() == 2 && std::all_of(cls.begin(), cls.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    }
    return true;
}

size_t utf8Len(unsigned char lead)
{
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    if (lead >= 0xC0)
        return 2;
    return 1;
}

std::string firstChar(std::string_view s)
{
    if (s.empty())
        return {};
    return std::string(s.substr(0, std::min(s.size(), utf8Len((unsigned char)s[0]))));
}

} // namespace fp::formats
