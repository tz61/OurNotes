#include "core/links.h"

#include <cmath>
#include <cstdio>

#include "core/library.h"
#include "core/schema.h"
#include "core/util.h"

namespace on {

namespace {

std::string number(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", std::round(v * 10) / 10);
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0)
        s.resize(s.size() - 2);
    return s == "-0" ? "0" : s;
}

// "(Smith and Jones, 2020, p. 3)"; an item with neither: its title
std::string citation(const Json& item, const std::string& page)
{
    std::vector<std::string> parts;
    auto type = jstr(item, "itemType");
    if (auto who = creatorSummary(item.value("creators", Json::array()), schema::primaryCreatorType(type)); !who.empty())
        parts.push_back(who);
    if (auto year = yearOf(jstr(item, "date")); !year.empty())
        parts.push_back(year);
    if (parts.empty()) {
        auto title = jstr(item, "title");
        if (title.empty())
            title = jstr(item, "filename");
        if (title.empty())
            title = "Untitled";
        parts.push_back(title);
    }
    if (!page.empty())
        parts.push_back("p. " + page);
    std::string out = "(";
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i ? ", " : "") + parts[i];
    return out + ")";
}

} // namespace

std::string itemLinkUrl(const ItemLink& link)
{
    std::string url = std::string(kItemLinkPrefix) + urlEncode(link.library) + "/" + urlEncode(link.key);
    std::string query;
    auto add = [&](const std::string& name, const std::string& value) {
        query += (query.empty() ? "?" : "&") + name + "=" + value;
    };
    if (link.page > 0)
        add("page", std::to_string(link.page));
    if (link.rect)
        add("rect", number((*link.rect)[0]) + "," + number((*link.rect)[1]) + "," + number((*link.rect)[2]) + "," +
                        number((*link.rect)[3]));
    if (!link.annotation.empty())
        add("annotation", urlEncode(link.annotation));
    return url + query;
}

std::optional<ItemLink> parseItemLink(std::string_view url)
{
    if (url.size() < kItemLinkPrefix.size() || toLower(std::string(url.substr(0, kItemLinkPrefix.size()))) != kItemLinkPrefix)
        return std::nullopt;
    url.remove_prefix(kItemLinkPrefix.size());
    std::string_view query;
    if (auto q = url.find_first_of("?#"); q != std::string_view::npos) {
        query = url[q] == '?' ? url.substr(q + 1) : std::string_view();
        query = query.substr(0, query.find('#'));
        url = url.substr(0, q);
    }
    auto slash = url.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    ItemLink link;
    link.library = urlDecode(url.substr(0, slash));
    auto key = url.substr(slash + 1);
    if (!key.empty() && key.back() == '/')
        key.remove_suffix(1);
    link.key = urlDecode(key);
    if (link.library.empty() || !isValidKey(link.key))
        return std::nullopt;
    while (!query.empty()) {
        auto amp = query.find('&');
        auto pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        auto eq = pair.find('=');
        auto name = pair.substr(0, eq);
        auto value = eq == std::string_view::npos ? std::string() : urlDecode(pair.substr(eq + 1));
        try {
            if (name == "page") {
                link.page = std::max(0, std::stoi(value));
            } else if (name == "rect") {
                std::array<double, 4> r{};
                size_t from = 0;
                for (int i = 0; i < 4; ++i) {
                    auto comma = value.find(',', from);
                    if ((i < 3) == (comma == std::string::npos))
                        throw std::invalid_argument("rect");
                    r[i] = std::stod(value.substr(from, comma - from));
                    from = comma + 1;
                }
                link.rect = r;
            } else if (name == "annotation" && isValidKey(value)) {
                link.annotation = value;
            }
        } catch (const std::exception&) {
            // (a part that does not read: without it)
        }
    }
    return link;
}

std::optional<LinkTarget> linkTarget(Library& lib, const ItemLink& link)
{
    std::vector<int64_t> ids;
    for (auto& l : lib.libraries())
        if (l.uid == link.library)
            ids = {l.id};
    if (ids.empty())
        for (auto& l : lib.libraries())
            ids.push_back(l.id);
    for (auto id : ids)
        if (auto item = lib.item(id, link.key))
            return LinkTarget{id, *item};
    return std::nullopt;
}

Reference referenceTo(Library& lib, int64_t libraryId, const std::string& key, int page, const std::string& pageLabel,
                      const std::optional<std::array<double, 4>>& rect)
{
    auto info = lib.library(libraryId);
    auto item = lib.item(libraryId, key);
    if (!info || !item)
        throw Error("no item " + key);
    ItemLink link{info->uid, key, 0, std::nullopt, {}};
    auto type = jstr(*item, "itemType");
    if (type == "note")
        return {itemLinkUrl(link), noteTitle(jstr(*item, "note")).empty() ? "Note" : noteTitle(jstr(*item, "note"))};
    std::string label = pageLabel;
    if (type == "annotation") {  // its PDF, at its page
        link.key = jstr(*item, "parentItem");
        link.annotation = key;
        auto position = item->value("annotationPosition", Json::object());
        if (position.is_object() && position.value("pageIndex", Json()).is_number_integer())
            page = position["pageIndex"].get<int>() + 1;
        if (label.empty())
            label = jstr(*item, "annotationPageLabel");
        item = lib.item(libraryId, link.key);
        if (!item)
            throw Error("no PDF of annotation " + key);
        type = jstr(*item, "itemType");
    }
    if (type == "attachment") {
        link.page = page;
        link.rect = page > 0 ? rect : std::nullopt;
        if (label.empty() && page > 0)
            label = std::to_string(page);
        auto parent = jstr(*item, "parentItem");
        if (auto p = parent.empty() ? std::nullopt : lib.item(libraryId, parent))
            return {itemLinkUrl(link), citation(*p, label)};
        return {itemLinkUrl(link), citation(*item, label)};
    }
    return {itemLinkUrl(link), citation(*item, {})};
}

namespace {

// “quote” and then the reference; either alone
std::string quoted(const std::string& quote, const std::string& reference)
{
    if (quote.empty())
        return reference;
    return "“" + quote + "”" + (reference.empty() ? std::string() : " " + reference);
}

} // namespace

std::string referenceHtml(const Reference& ref, const std::string& quote)
{
    if (ref.url.empty())  // (one that could not be made: the words alone)
        return quoted(htmlEscape(quote), htmlEscape(ref.text));
    return quoted(htmlEscape(quote), "<a href=\"" + htmlEscape(ref.url) + "\">" + htmlEscape(ref.text) + "</a>");
}

std::string referenceMarkdown(const Reference& ref, const std::string& quote)
{
    if (ref.url.empty())
        return quoted(quote, ref.text);
    std::string text;
    for (char c : ref.text)  // (a bracket would end the link's words)
        text += c == '[' ? '(' : c == ']' ? ')' : c;
    return quoted(quote, "[" + text + "](" + ref.url + ")");
}

std::string referenceText(const Reference& ref, const std::string& quote) { return quoted(quote, ref.text); }

} // namespace on
