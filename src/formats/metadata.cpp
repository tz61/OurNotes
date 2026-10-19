#include "formats/metadata.h"

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <thread>

#include "core/schema.h"
#include "formats/common.h"
#include "formats/csljson.h"
#include "ournotes/version.h"

namespace on::formats {

namespace {

constexpr time_t kTimeoutSeconds = 15;
constexpr int kMaxRedirects = 5;
constexpr size_t kMaxBufferedBody = 32u << 20;

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlnum(char c) { return std::isalnum((unsigned char)c) != 0; }

// =================================================================================
// HTTP
// =================================================================================

std::string userAgent()
{
    return std::string("OurNotes/") + OURNOTES_VERSION + " (mailto:ournotes@users.noreply.github.com)";
}

struct Url {
    std::string scheme;  // http or https
    std::string origin;  // scheme://host[:port]
    std::string path;    // /path?query
    std::string host() const { return origin.substr(scheme.size() + 3); }
};

Url parseUrl(const std::string& url)
{
    auto sep = url.find("://");
    if (sep == std::string::npos)
        throw MetadataError("Invalid URL: " + url);
    Url u;
    u.scheme = toLower(url.substr(0, sep));
    if (u.scheme != "http" && u.scheme != "https")
        throw MetadataError("Unsupported URL: " + url);
    auto pathStart = url.find_first_of("/?#", sep + 3);
    auto host = url.substr(sep + 3, pathStart == std::string::npos ? std::string::npos : pathStart - sep - 3);
    if (host.empty())
        throw MetadataError("Invalid URL: " + url);
    u.origin = u.scheme + "://" + host;
    u.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    auto hash = u.path.find('#');
    if (hash != std::string::npos)
        u.path.resize(hash);
    if (u.path.empty() || u.path[0] != '/')
        u.path = "/" + u.path;
    return u;
}

std::string resolveUrl(const Url& base, const std::string& location)
{
    if (location.find("://") != std::string::npos)
        return location;
    if (startsWith(location, "//"))
        return base.scheme + ":" + location;
    if (startsWith(location, "/"))
        return base.origin + location;
    auto path = base.path.substr(0, base.path.find('?'));
    return base.origin + path.substr(0, path.rfind('/') + 1) + location;
}

[[noreturn]] void throwNetworkError(httplib::Error err, const std::string& host)
{
    using E = httplib::Error;
    switch (err) {
    case E::SSLConnection:
    case E::SSLServerVerification:
    case E::SSLServerHostnameVerification:
        throw MetadataError("Could not establish a secure connection to " + host + " (" + httplib::to_string(err) +
                            ").");
    case E::SSLLoadingCerts:
        throw MetadataError("HTTPS is unavailable: the system's certificate store could not be loaded.");
    case E::ConnectionTimeout:
    case E::Timeout:
        throw MetadataError(host + " did not respond in time. Please try again later.");
    case E::Connection:
        throw MetadataError("Could not connect to " + host + ". Check your internet connection.");
    default:
        throw MetadataError("Network error while contacting " + host + ": " + httplib::to_string(err) + ".");
    }
}

struct Response {
    int status = 0;
    std::string body;
    std::string contentType;
};

using Sink = std::function<bool(const char*, size_t)>;

// GET following up to kMaxRedirects redirects (also across hosts). With a sink, the body
// of a 200 response is streamed to it; everything else is buffered in Response::body.
// A dropped connection or a 429/503 answer is retried once after a short pause.
Response httpGet(const std::string& url, const std::string& accept, const Sink& sink = nullptr)
{
    std::string current = url;
    bool retried = false;
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        auto u = parseUrl(current);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
        if (u.scheme == "https")
            throw MetadataError("HTTPS is unavailable: this build of OurNotes was compiled without OpenSSL, so "
                                "online lookups are disabled.");
#endif
        httplib::Client cli(u.origin);
        if (!cli.is_valid())
            throw MetadataError("Invalid URL: " + current);
        cli.set_connection_timeout(kTimeoutSeconds);
        cli.set_read_timeout(kTimeoutSeconds);
        cli.set_write_timeout(kTimeoutSeconds);
        cli.set_follow_location(false);
        httplib::Headers headers = {{"User-Agent", userAgent()}, {"Accept", accept}};

        Response r;
        std::string location;
        bool streamed = false;
        auto res = cli.Get(
            u.path, headers,
            [&](const httplib::Response& resp) {
                r.status = resp.status;
                r.contentType = resp.get_header_value("Content-Type");
                location = resp.get_header_value("Location");
                return true;
            },
            [&](const char* data, size_t len) {
                if (sink && r.status == 200) {
                    streamed = true;
                    return sink(data, len);
                }
                if (r.body.size() + len > kMaxBufferedBody)
                    return false;
                r.body.append(data, len);
                return true;
            });
        bool transient = res ? (r.status == 429 || r.status == 503)
                             : (res.error() == httplib::Error::Read || res.error() == httplib::Error::ConnectionClosed);
        if (transient && !retried && !streamed) {
            retried = true;
            --hop;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (!res)
            throwNetworkError(res.error(), u.host());
        if (r.status >= 300 && r.status < 400 && r.status != 304 && !location.empty()) {
            current = resolveUrl(u, location);
            continue;
        }
        return r;
    }
    throw MetadataError("Too many redirects while fetching " + url + ".");
}

// nullopt for 400/404 (identifier unknown to the service).
std::optional<Json> fetchJson(const std::string& url, const std::string& service,
                              const std::string& accept = "application/json")
{
    auto r = httpGet(url, accept);
    if (r.status == 404 || r.status == 400 || r.status == 410)
        return std::nullopt;
    if (r.status == 429 || r.status == 503)
        throw MetadataError(service + " is busy right now. Please try again in a moment.");
    if (r.status != 200)
        throw MetadataError(service + " returned an error (HTTP " + std::to_string(r.status) + ").");
    try {
        return Json::parse(r.body);
    } catch (const std::exception&) {
        throw MetadataError("Unexpected response from " + service + ".");
    }
}

std::string encodePath(const std::string& s) { return replaceAll(urlEncode(s), "%2F", "/"); }

// =================================================================================
// Text helpers
// =================================================================================

// Strips inline markup (<i>, <sub>, MathML ...) and decodes entities.
std::string stripTags(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            auto end = s.find('>', i);
            if (end == std::string_view::npos)
                break;
            i = end + 1;
        } else {
            out += s[i++];
        }
    }
    return collapseWhitespace(htmlUnescape(out));
}

// Crossref JATS abstracts: one line per paragraph, "Abstract" headings dropped.
std::string jatsToText(std::string_view s)
{
    std::vector<std::string> paras;
    std::string cur;
    auto flush = [&] {
        auto p = collapseWhitespace(htmlUnescape(cur));
        if (!p.empty())
            paras.push_back(p);
        cur.clear();
    };
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\n' && i + 1 < s.size() && s[i + 1] == '\n') {  // blank line in plain text
            flush();
            i += 2;
            continue;
        }
        if (s[i] != '<') {
            cur += s[i++];
            continue;
        }
        auto end = s.find('>', i);
        if (end == std::string_view::npos)
            break;
        auto tag = toLower(s.substr(i + 1, end - i - 1));
        bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
            tag.erase(0, 1);
        tag = tag.substr(0, tag.find_first_of(" \t\n/"));
        auto colon = tag.find(':');
        if (colon != std::string::npos)
            tag = tag.substr(colon + 1);
        i = end + 1;
        if (tag == "title" && !closing) {
            auto close = s.find("</", i);
            if (close != std::string_view::npos && iequals(trim(stripTags(s.substr(i, close - i))), "abstract")) {
                auto closeEnd = s.find('>', close);
                i = closeEnd == std::string_view::npos ? s.size() : closeEnd + 1;
                continue;
            }
        }
        if (tag == "p" || tag == "sec" || tag == "title" || tag == "br" || tag == "list-item" || tag == "div")
            flush();
    }
    flush();
    return join(paras, "\n");
}

// "SMITH" -> "Smith"; mixed-case names are left alone.
std::string fixCase(const std::string& name)
{
    bool hasLower = std::any_of(name.begin(), name.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    int upper = (int)std::count_if(name.begin(), name.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    if (hasLower || upper < 2)
        return name;
    std::string out = name;
    bool start = true;
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z' && !start)
            c = (char)(c + 32);
        start = c == ' ' || c == '-' || c == '\'' || c == '.';
    }
    return out;
}

std::string firstString(const Json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_array() && !it->empty() && (*it)[0].is_string())
        return (*it)[0].get<std::string>();
    return {};
}

std::string numberString(const Json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number_integer())
        return std::to_string(it->get<long long>());
    return {};
}

std::string joinNonEmptyWords(const std::vector<std::string>& parts)
{
    std::string out;
    for (auto& p : parts)
        if (!p.empty())
            out += (out.empty() ? "" : " ") + p;
    return out;
}

MetadataError notFound(const std::string& what)
{
    return MetadataError("No metadata found for " + what + ".");
}

// =================================================================================
// Crossref
// =================================================================================

std::string crossrefType(const std::string& t)
{
    static const std::map<std::string, std::string> m = {
        {"journal-article", "journalArticle"}, {"proceedings-article", "conferencePaper"},
        {"book-chapter", "bookSection"},       {"book-section", "bookSection"},
        {"book-part", "bookSection"},          {"book", "book"},
        {"monograph", "book"},                 {"edited-book", "book"},
        {"reference-book", "book"},            {"posted-content", "preprint"},
        {"report", "report"},                  {"dissertation", "thesis"},
        {"dataset", "dataset"},                {"standard", "standard"}};
    auto it = m.find(t);
    return it == m.end() ? "document" : it->second;
}

std::string crossrefDate(const Json& m)
{
    for (const char* key : {"issued", "published-print", "published-online", "published", "created"}) {
        auto it = m.find(key);
        if (it == m.end() || !it->contains("date-parts"))
            continue;
        const auto& dp = (*it)["date-parts"];
        if (!dp.is_array() || dp.empty() || !dp[0].is_array())
            continue;
        const auto& p = dp[0];
        auto part = [&](size_t i) { return i < p.size() && p[i].is_number_integer() ? p[i].get<int>() : 0; };
        DateParts d{part(0), part(1), part(2)};
        if (d.valid())
            return isoDate(d);
    }
    return {};
}

void crossrefCreators(Json& item, const Json& m, const char* key, const std::string& creatorType)
{
    auto it = m.find(key);
    if (it == m.end() || !it->is_array())
        return;
    for (auto& a : *it) {
        auto family = jstr(a, "family");
        if (!family.empty())
            addCreator(item, creatorType, personName(fixCase(jstr(a, "given")), fixCase(family)));
        else if (!jstr(a, "name").empty())
            addCreator(item, creatorType, literalName(jstr(a, "name")));
    }
}

Json crossrefToItem(const Json& m)
{
    auto type = crossrefType(jstr(m, "type"));
    Json item = newItem(type);

    auto title = stripTags(firstString(m, "title"));
    auto subtitle = stripTags(firstString(m, "subtitle"));
    if (!subtitle.empty() && normalize(title).find(normalize(subtitle)) == std::string::npos)
        title += ": " + subtitle;
    setField(item, "title", title);
    crossrefCreators(item, m, "author", "author");
    crossrefCreators(item, m, "editor", "editor");
    crossrefCreators(item, m, "translator", "translator");
    setField(item, "date", crossrefDate(m));

    std::vector<std::string> containers;
    for (auto& c : m.value("container-title", Json::array()))
        if (c.is_string() && !trim(c.get<std::string>()).empty())
            containers.push_back(stripTags(c.get<std::string>()));
    if (!containers.empty()) {
        if (type == "book") {
            setField(item, "series", containers[0]);
        } else if (containers.size() > 1 && (type == "bookSection" || type == "conferencePaper")) {
            setField(item, "series", containers.front());  // e.g. Lecture Notes in Computer Science
            setField(item, "publicationTitle", containers.back());
        } else {
            setField(item, "publicationTitle", containers[0]);
        }
    }
    setField(item, "journalAbbreviation", stripTags(firstString(m, "short-container-title")));
    setField(item, "shortTitle", stripTags(firstString(m, "short-title")));
    setField(item, "volume", numberString(m, "volume"));
    setField(item, "issue", numberString(m, "issue"));
    auto pages = numberString(m, "page");
    setField(item, "pages", pages.empty() ? numberString(m, "article-number") : pages);
    setField(item, "edition", numberString(m, "edition-number"));

    std::string institution;
    if (m.contains("institution") && m["institution"].is_array() && !m["institution"].empty())
        institution = jstr(m["institution"][0], "name");
    if ((type == "thesis" || type == "preprint") && !institution.empty())
        setField(item, "publisher", institution);
    else
        setField(item, "publisher", jstr(m, "publisher"));
    auto place = jstr(m, "publisher-location");
    if (type == "conferencePaper" && m.contains("event") && m["event"].is_object()) {
        setField(item, "conferenceName", jstr(m["event"], "name"));
        if (place.empty())
            place = jstr(m["event"], "location");
    }
    setField(item, "place", place);

    std::vector<std::string> issn, isbn;
    for (auto& v : m.value("ISSN", Json::array()))
        if (v.is_string())
            issn.push_back(v);
    for (auto& v : m.value("ISBN", Json::array()))
        if (v.is_string())
            isbn.push_back(v);
    setField(item, "ISSN", join(issn, ", "));
    setField(item, "ISBN", join(isbn, " "));
    setField(item, "DOI", jstr(m, "DOI"));
    std::string url;
    if (m.contains("resource") && m["resource"].contains("primary"))
        url = jstr(m["resource"]["primary"], "URL");
    setField(item, "url", url.empty() ? jstr(m, "URL") : url);
    setField(item, "abstractNote", jatsToText(jstr(m, "abstract")));
    setField(item, "language", jstr(m, "language"));
    setField(item, "libraryCatalog", "DOI.org (Crossref)");
    setField(item, "accessDate", nowIso());
    return finishItem(item);
}

// DOIs registered elsewhere (DataCite, mEDRA ...) via doi.org content negotiation.
std::optional<Json> lookupDoiCsl(const std::string& doi)
{
    auto csl = fetchJson("https://doi.org/" + encodePath(doi), "doi.org", "application/vnd.citationstyles.csl+json");
    if (!csl || !csl->is_object())
        return std::nullopt;
    auto item = fromCsl(*csl);
    if (jstr(item, "DOI").empty())
        setField(item, "DOI", doi);
    auto abstract = jstr(item, "abstractNote");
    if (abstract.find('<') != std::string::npos)
        item["abstractNote"] = jatsToText(abstract);
    setField(item, "libraryCatalog", "DOI.org");
    setField(item, "accessDate", nowIso());
    return finishItem(item);
}

Json lookupDoi(const std::string& doi)
{
    auto json = fetchJson("https://api.crossref.org/works/" + encodePath(doi), "Crossref");
    if (json && json->contains("message") && (*json)["message"].is_object())
        return crossrefToItem((*json)["message"]);
    if (auto item = lookupDoiCsl(doi))
        return *item;
    throw notFound("DOI " + doi);
}

// =================================================================================
// arXiv (Atom)
// =================================================================================

struct XmlElement {
    std::string attrs;
    std::string inner;
};

// All <tag ...>...</tag> (or self-closing) elements; nesting of the same tag is not supported.
std::vector<XmlElement> xmlElements(std::string_view xml, const std::string& tag)
{
    std::vector<XmlElement> out;
    const std::string open = "<" + tag;
    const std::string close = "</" + tag + ">";
    size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string_view::npos) {
        size_t after = pos + open.size();
        if (after >= xml.size())
            break;
        char c = xml[after];
        if (c != '>' && c != '/' && !std::isspace((unsigned char)c)) {
            pos = after;
            continue;
        }
        size_t gt = xml.find('>', after);
        if (gt == std::string_view::npos)
            break;
        XmlElement e;
        e.attrs = std::string(xml.substr(after, gt - after));
        if (xml[gt - 1] == '/') {
            e.attrs.pop_back();
            out.push_back(std::move(e));
            pos = gt + 1;
            continue;
        }
        size_t end = xml.find(close, gt + 1);
        if (end == std::string_view::npos)
            break;
        e.inner = std::string(xml.substr(gt + 1, end - gt - 1));
        out.push_back(std::move(e));
        pos = end + close.size();
    }
    return out;
}

std::string xmlAttr(std::string_view attrs, const std::string& name)
{
    size_t pos = 0;
    while ((pos = attrs.find(name, pos)) != std::string_view::npos) {
        size_t i = pos + name.size();
        bool boundary = pos == 0 || std::isspace((unsigned char)attrs[pos - 1]);
        while (i < attrs.size() && std::isspace((unsigned char)attrs[i]))
            ++i;
        if (boundary && i < attrs.size() && attrs[i] == '=') {
            ++i;
            while (i < attrs.size() && std::isspace((unsigned char)attrs[i]))
                ++i;
            if (i < attrs.size() && (attrs[i] == '"' || attrs[i] == '\'')) {
                auto end = attrs.find(attrs[i], i + 1);
                if (end != std::string_view::npos)
                    return htmlUnescape(attrs.substr(i + 1, end - i - 1));
            }
        }
        pos += name.size();
    }
    return {};
}

// Text content: tags dropped, entities decoded, CDATA kept verbatim, whitespace collapsed.
std::string xmlText(std::string_view inner)
{
    std::string out;
    size_t i = 0;
    while (i < inner.size()) {
        auto cdata = inner.find("<![CDATA[", i);
        auto text = inner.substr(i, cdata == std::string_view::npos ? std::string_view::npos : cdata - i);
        out += stripTags(text);
        if (cdata == std::string_view::npos)
            break;
        auto end = inner.find("]]>", cdata + 9);
        out += ' ';
        out += inner.substr(cdata + 9, end == std::string_view::npos ? std::string_view::npos : end - cdata - 9);
        out += ' ';
        i = end == std::string_view::npos ? inner.size() : end + 3;
    }
    return collapseWhitespace(out);
}

std::string xmlChildText(std::string_view xml, const std::string& tag)
{
    auto els = xmlElements(xml, tag);
    return els.empty() ? std::string() : xmlText(els[0].inner);
}

Json arxivToItem(const std::string& xml, const std::string& requested)
{
    auto entries = xmlElements(xml, "entry");
    if (entries.empty())
        throw notFound("arXiv ID " + requested);
    const auto& e = entries[0].inner;
    auto idUrl = xmlChildText(e, "id");
    auto title = xmlChildText(e, "title");
    if (idUrl.find("/api/errors") != std::string::npos || title == "Error" || title.empty())
        throw notFound("arXiv ID " + requested);
    auto abs = idUrl.find("/abs/");
    auto id = stripArxivVersion(abs == std::string::npos ? requested : idUrl.substr(abs + 5));

    Json item = newItem("preprint");
    setField(item, "title", title);
    for (auto& a : xmlElements(e, "author"))
        addCreator(item, "author", splitName(xmlChildText(a.inner, "name")));
    setField(item, "abstractNote", xmlChildText(e, "summary"));
    setField(item, "date", xmlChildText(e, "published").substr(0, 10));
    setField(item, "url", "https://arxiv.org/abs/" + id);
    setField(item, "DOI", "10.48550/arXiv." + id);
    setField(item, "repository", "arXiv");
    setField(item, "archiveID", "arXiv:" + id);
    setField(item, "libraryCatalog", "arXiv.org");
    setField(item, "accessDate", nowIso());

    auto categories = xmlElements(e, "arxiv:primary_category");
    auto primary = categories.empty() ? std::string() : xmlAttr(categories[0].attrs, "term");
    appendExtra(item, "arXiv:" + id + (primary.empty() ? "" : " [" + primary + "]"));
    auto journalRef = xmlChildText(e, "arxiv:journal_ref");
    if (!journalRef.empty())
        appendExtra(item, "Journal reference: " + journalRef);
    auto publishedDoi = xmlChildText(e, "arxiv:doi");
    if (!publishedDoi.empty())
        appendExtra(item, "Published version DOI: " + publishedDoi);
    auto comment = xmlChildText(e, "arxiv:comment");
    if (!comment.empty())
        item["_notes"] = Json::array({"<p>Comment: " + htmlEscape(comment) + "</p>"});
    return finishItem(item);
}

Json lookupArxiv(const std::string& rawId)
{
    auto id = rawId;
    if (startsWith(toLower(id), "arxiv:"))
        id = id.substr(6);
    auto r = httpGet("https://export.arxiv.org/api/query?id_list=" + urlEncode(id), "application/atom+xml");
    if (r.status == 429 || r.status == 503)
        throw MetadataError("arXiv is busy right now. Please try again in a moment.");
    if (r.status == 400 || r.status == 404)
        throw notFound("arXiv ID " + id);
    if (r.status != 200)
        throw MetadataError("arXiv returned an error (HTTP " + std::to_string(r.status) + ").");
    return arxivToItem(r.body, id);
}

// =================================================================================
// Open Library (ISBN) and PubMed
// =================================================================================

Json lookupIsbn(const std::string& isbn)
{
    auto key = "ISBN:" + isbn;
    auto json = fetchJson("https://openlibrary.org/api/books?bibkeys=" + urlEncode(key) + "&format=json&jscmd=data",
                          "Open Library");
    if (!json || !json->contains(key))
        throw notFound("ISBN " + isbn);
    const auto& b = (*json)[key];
    Json item = newItem("book");
    auto title = jstr(b, "title");
    auto subtitle = jstr(b, "subtitle");
    setField(item, "title", subtitle.empty() ? title : title + ": " + subtitle);
    for (auto& a : b.value("authors", Json::array()))
        addCreator(item, "author", splitName(jstr(a, "name")));
    if (b.contains("publishers") && !b["publishers"].empty())
        setField(item, "publisher", jstr(b["publishers"][0], "name"));
    if (b.contains("publish_places") && !b["publish_places"].empty())
        setField(item, "place", jstr(b["publish_places"][0], "name"));
    auto date = jstr(b, "publish_date");
    auto d = parseDate(date);
    setField(item, "date", d.valid() ? isoDate(d) : date);
    setField(item, "numPages", numberString(b, "number_of_pages"));
    setField(item, "ISBN", isbn);  // records often merge several editions' ISBNs
    setField(item, "url", jstr(b, "url"));
    setField(item, "libraryCatalog", "Open Library");
    return finishItem(item);
}

// "436-44" -> "436-444"
std::string expandPages(const std::string& pages)
{
    auto dash = pages.find('-');
    if (dash == std::string::npos)
        return pages;
    auto a = pages.substr(0, dash), b = pages.substr(dash + 1);
    auto digits = [](const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), isDigit); };
    if (digits(a) && digits(b) && b.size() < a.size())
        return a + "-" + a.substr(0, a.size() - b.size()) + b;
    return pages;
}

// PubMed efetch XML (full names and abstract, unlike esummary).
Json lookupPmid(const std::string& pmid)
{
    auto r = httpGet("https://eutils.ncbi.nlm.nih.gov/entrez/eutils/efetch.fcgi?db=pubmed&retmode=xml&id=" +
                         urlEncode(pmid),
                     "application/xml");
    if (r.status == 429 || r.status == 503)
        throw MetadataError("PubMed is busy right now. Please try again in a moment.");
    if (r.status != 200 && r.status != 400 && r.status != 404)
        throw MetadataError("PubMed returned an error (HTTP " + std::to_string(r.status) + ").");
    auto articles = xmlElements(r.body, "PubmedArticle");
    if (r.status != 200 || articles.empty())
        throw notFound("PMID " + pmid);
    const auto& a = articles[0].inner;

    Json item = newItem("journalArticle");
    auto title = xmlChildText(a, "ArticleTitle");
    if (endsWith(title, ".") && !endsWith(title, ".."))
        title.pop_back();
    setField(item, "title", title);

    auto authorLists = xmlElements(a, "AuthorList");
    for (auto& au : xmlElements(authorLists.empty() ? std::string() : authorLists[0].inner, "Author")) {
        auto last = xmlChildText(au.inner, "LastName");
        if (!last.empty())
            addCreator(item, "author", personName(xmlChildText(au.inner, "ForeName"), last));
        else if (!xmlChildText(au.inner, "CollectiveName").empty())
            addCreator(item, "author", literalName(xmlChildText(au.inner, "CollectiveName")));
    }

    std::vector<std::string> abstract;
    for (auto& part : xmlElements(a, "AbstractText")) {
        auto label = xmlAttr(part.attrs, "Label");
        auto text = xmlText(part.inner);
        if (!text.empty())
            abstract.push_back(label.empty() || label == "UNLABELLED" ? text : label + ": " + text);
    }
    setField(item, "abstractNote", join(abstract, "\n"));

    auto journals = xmlElements(a, "Journal");
    const std::string journal = journals.empty() ? std::string() : journals[0].inner;
    setField(item, "publicationTitle", xmlChildText(journal, "Title"));
    setField(item, "journalAbbreviation", xmlChildText(journal, "ISOAbbreviation"));
    setField(item, "ISSN", xmlChildText(journal, "ISSN"));
    setField(item, "volume", xmlChildText(journal, "Volume"));
    setField(item, "issue", xmlChildText(journal, "Issue"));
    auto pubDates = xmlElements(journal, "PubDate");
    if (!pubDates.empty()) {
        const auto& pd = pubDates[0].inner;
        auto raw = joinNonEmptyWords({xmlChildText(pd, "Year"), xmlChildText(pd, "Month"), xmlChildText(pd, "Day")});
        if (raw.empty())
            raw = xmlChildText(pd, "MedlineDate");
        auto d = parseDate(raw);
        setField(item, "date", d.valid() ? isoDate(d) : raw);
    }
    setField(item, "pages", expandPages(xmlChildText(a, "MedlinePgn")));
    auto idLists = xmlElements(a, "ArticleIdList");
    for (auto& id : xmlElements(idLists.empty() ? std::string() : idLists[0].inner, "ArticleId")) {
        auto type = xmlAttr(id.attrs, "IdType");
        if (type == "doi")
            setField(item, "DOI", xmlText(id.inner));
        else if (type == "pmc")
            setField(item, "PMCID", xmlText(id.inner));
    }
    setField(item, "PMID", pmid);
    setField(item, "language", xmlChildText(a, "Language"));
    setField(item, "libraryCatalog", "PubMed");
    return finishItem(item);
}

// =================================================================================
// Identifier parsing
// =================================================================================

// Drops trailing punctuation and unbalanced closing brackets ("(doi:10.1/x)." -> "10.1/x").
std::string trimDoi(std::string doi)
{
    long paren = 0, bracket = 0, brace = 0;  // closing minus opening
    for (char c : doi) {
        paren += c == ')' ? 1 : c == '(' ? -1 : 0;
        bracket += c == ']' ? 1 : c == '[' ? -1 : 0;
        brace += c == '}' ? 1 : c == '{' ? -1 : 0;
    }
    while (!doi.empty()) {
        char c = doi.back();
        bool strip = std::string_view(".,;:'\">").find(c) != std::string_view::npos;
        if (c == ')' && paren > 0)
            strip = paren-- > 0;
        else if (c == ']' && bracket > 0)
            strip = bracket-- > 0;
        else if (c == '}' && brace > 0)
            strip = brace-- > 0;
        if (!strip)
            break;
        doi.pop_back();
    }
    return doi;
}

std::optional<std::string> normalizeIsbn(std::string_view s)
{
    std::string d;
    for (char c : s) {
        if (isDigit(c))
            d += c;
        else if (c == 'x' || c == 'X')
            d += 'X';
        else if (c != '-' && c != ' ')
            return std::nullopt;
    }
    if (d.size() == 10) {
        int sum = 0;
        for (size_t i = 0; i < 10; ++i) {
            if (d[i] == 'X' && i != 9)
                return std::nullopt;
            int v = d[i] == 'X' ? 10 : d[i] - '0';
            sum += (int)(10 - i) * v;
        }
        return sum % 11 == 0 ? std::optional<std::string>(d) : std::nullopt;
    }
    if (d.size() == 13 && d.find('X') == std::string::npos && (startsWith(d, "978") || startsWith(d, "979"))) {
        int sum = 0;
        for (size_t i = 0; i < 13; ++i)
            sum += (d[i] - '0') * (i % 2 ? 3 : 1);
        return sum % 10 == 0 ? std::optional<std::string>(d) : std::nullopt;
    }
    return std::nullopt;
}

void addUnique(std::vector<Identifier>& out, Identifier id)
{
    for (auto& o : out)
        if (o.kind == id.kind && toLower(o.value) == toLower(id.value))
            return;
    out.push_back(std::move(id));
}

std::optional<Identifier> classifyToken(std::string t)
{
    auto first = t.find_first_not_of("<([{\"'");
    t = first == std::string::npos ? std::string() : t.substr(first);
    while (!t.empty() && std::string_view(">\"'.,;").find(t.back()) != std::string_view::npos)
        t.pop_back();
    if (t.empty())
        return std::nullopt;
    auto lower = toLower(t);

    if (startsWith(lower, "pmid:")) {
        auto v = trim(t.substr(5));
        if (!v.empty() && v.size() <= 9 && std::all_of(v.begin(), v.end(), isDigit))
            return Identifier{"pmid", v};
        return std::nullopt;
    }
    if (startsWith(lower, "isbn:")) {
        auto isbn = normalizeIsbn(t.substr(5));
        return isbn ? std::optional<Identifier>(Identifier{"isbn", *isbn}) : std::nullopt;
    }
    if (lower.find("doi.org/") != std::string::npos || startsWith(lower, "doi:") || startsWith(lower, "10.")) {
        auto doi = trimDoi(cleanDoi(t));
        if (!doi.empty())
            return Identifier{"doi", doi};
    }
    std::string ax = t;
    auto p = lower.find("arxiv.org/abs/");
    if (p == std::string::npos)
        p = lower.find("arxiv.org/pdf/");
    if (p != std::string::npos) {
        ax = t.substr(p + 14);
        ax = ax.substr(0, ax.find_first_of("?#"));
        while (!ax.empty() && ax.back() == '/')
            ax.pop_back();
        if (endsWith(toLower(ax), ".pdf"))
            ax.resize(ax.size() - 4);
    } else if (startsWith(lower, "arxiv:")) {
        ax = t.substr(6);
    }
    if (isArxivId(ax))
        return Identifier{"arxiv", stripArxivVersion(ax)};
    if (auto isbn = normalizeIsbn(t))
        return Identifier{"isbn", *isbn};
    return std::nullopt;
}

} // namespace

// =================================================================================
// Public API
// =================================================================================

std::vector<Identifier> parseIdentifiers(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : text) {
        if (std::isspace((unsigned char)c) || c == ',') {
            if (!cur.empty())
                tokens.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        tokens.push_back(cur);

    std::vector<Identifier> out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        auto t = tokens[i];
        auto lower = toLower(t);
        if ((lower == "doi:" || lower == "pmid:" || lower == "arxiv:" || lower == "isbn:") && i + 1 < tokens.size())
            t += tokens[++i];
        if (auto id = classifyToken(t))
            addUnique(out, *id);
    }
    return out;
}

std::vector<Identifier> findIdentifiersInText(const std::string& text)
{
    std::vector<Identifier> out;
    const std::string lower = toLower(text);
    const size_t n = text.size();
    auto skipSpaces = [&](size_t j) {
        while (j < n && (text[j] == ' ' || text[j] == '\t'))
            ++j;
        return j;
    };
    for (size_t i = 0; i < n; ++i) {
        char c = lower[i];
        if (c == '1' && lower.compare(i, 3, "10.") == 0 && (i == 0 || !isAlnum(lower[i - 1]))) {
            size_t j = i + 3;
            while (j < n && isDigit(lower[j]))
                ++j;
            size_t digits = j - i - 3;
            if (digits < 4 || digits > 9 || j >= n || lower[j] != '/')
                continue;
            size_t k = j + 1;
            while (k < n && !std::isspace((unsigned char)text[k]) && text[k] != '"' && text[k] != '<')
                ++k;
            auto doi = trimDoi(text.substr(i, k - i));
            if (doi.size() > j - i + 1) {
                addUnique(out, {"doi", doi});
                i = k - 1;
            }
        } else if (c == 'a' && lower.compare(i, 5, "arxiv") == 0 && (i == 0 || !isAlnum(lower[i - 1]))) {
            size_t j = i + 5;
            if (lower.compare(j, 1, ":") == 0)
                j += 1;
            else if (lower.compare(j, 9, ".org/abs/") == 0 || lower.compare(j, 9, ".org/pdf/") == 0)
                j += 9;
            else
                continue;
            j = skipSpaces(j);
            size_t k = j;
            while (k < n && k - j < 48 && (isAlnum(text[k]) || text[k] == '.' || text[k] == '-' || text[k] == '/'))
                ++k;
            auto id = text.substr(j, k - j);
            while (!id.empty() && (id.back() == '.' || id.back() == '/' || id.back() == '-'))
                id.pop_back();
            if (endsWith(toLower(id), ".pdf"))
                id.resize(id.size() - 4);
            if (isArxivId(id)) {
                addUnique(out, {"arxiv", stripArxivVersion(id)});
                i = k - 1;
            }
        } else if (c == 'i' && lower.compare(i, 4, "isbn") == 0 && (i == 0 || !isAlnum(lower[i - 1]))) {
            size_t j = i + 4;
            if (lower.compare(j, 3, "-13") == 0 || lower.compare(j, 3, "-10") == 0)
                j += 3;
            j = skipSpaces(j);
            if (j < n && text[j] == ':')
                j = skipSpaces(j + 1);
            std::string candidate;
            size_t k = j;
            while (k < n && k - j < 20 && (isDigit(text[k]) || text[k] == '-' || text[k] == ' ' || text[k] == 'X' ||
                                            text[k] == 'x'))
                candidate += text[k++];
            std::string compact;
            for (char ch : candidate)
                if (ch != '-' && ch != ' ')
                    compact += ch;
            for (size_t len : {13u, 10u})
                if (compact.size() >= len)
                    if (auto isbn = normalizeIsbn(compact.substr(0, len))) {
                        addUnique(out, {"isbn", *isbn});
                        break;
                    }
        } else if (c == 'p' && lower.compare(i, 4, "pmid") == 0 && (i == 0 || !isAlnum(lower[i - 1]))) {
            size_t j = skipSpaces(i + 4);
            if (j < n && text[j] == ':')
                j = skipSpaces(j + 1);
            size_t k = j;
            while (k < n && isDigit(text[k]))
                ++k;
            if (k > j && k - j <= 9 && (k == n || !isAlnum(text[k])))
                addUnique(out, {"pmid", text.substr(j, k - j)});
        }
    }
    return out;
}

Json lookup(const Identifier& id)
{
    if (id.kind == "doi") {
        auto doi = cleanDoi(id.value);
        if (doi.empty())
            throw MetadataError("\"" + id.value + "\" is not a valid DOI.");
        if (startsWith(toLower(doi), "10.48550/arxiv."))
            return lookupArxiv(doi.substr(15));
        return lookupDoi(doi);
    }
    if (id.kind == "arxiv")
        return lookupArxiv(id.value);
    if (id.kind == "isbn") {
        auto isbn = normalizeIsbn(id.value);
        if (!isbn)
            throw MetadataError("\"" + id.value + "\" is not a valid ISBN.");
        return lookupIsbn(*isbn);
    }
    if (id.kind == "pmid")
        return lookupPmid(trim(id.value));
    throw MetadataError("Unsupported identifier type \"" + id.kind + "\".");
}

double titleSimilarity(const std::string& a, const std::string& b)
{
    auto simplify = [](const std::string& s) {
        std::string out;
        for (char c : normalize(s)) {
            unsigned char u = (unsigned char)c;
            if (u >= 0x80 || std::isalnum(u))
                out += c;
            else if (!out.empty() && out.back() != ' ')
                out += ' ';
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out.substr(0, 400);
    };
    auto x = simplify(a), y = simplify(b);
    if (x.empty() || y.empty())
        return 0.0;
    std::vector<size_t> prev(y.size() + 1), row(y.size() + 1);
    for (size_t j = 0; j <= y.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= x.size(); ++i) {
        row[0] = i;
        for (size_t j = 1; j <= y.size(); ++j)
            row[j] = std::min({prev[j] + 1, row[j - 1] + 1, prev[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1)});
        std::swap(prev, row);
    }
    return 1.0 - (double)prev[y.size()] / (double)std::max(x.size(), y.size());
}

std::optional<Json> lookupByTitle(const std::string& title, const std::string& authorHint)
{
    auto q = collapseWhitespace(title);
    if (q.empty())
        return std::nullopt;
    auto query = collapseWhitespace(q + " " + authorHint);
    auto json = fetchJson("https://api.crossref.org/works?rows=3&query.bibliographic=" + urlEncode(query), "Crossref");
    if (!json || !json->contains("message"))
        return std::nullopt;
    const Json items = (*json)["message"].value("items", Json::array());
    const Json* best = nullptr;
    double bestScore = 0;
    for (auto& m : items) {
        auto t = stripTags(firstString(m, "title"));
        auto sub = stripTags(firstString(m, "subtitle"));
        double score = titleSimilarity(q, t);
        if (!sub.empty())
            score = std::max(score, titleSimilarity(q, t + ": " + sub));
        if (score > bestScore) {
            bestScore = score;
            best = &m;
        }
    }
    if (!best || bestScore < 0.9)
        return std::nullopt;
    return crossrefToItem(*best);
}

std::string arxivPdfUrl(const std::string& arxivId)
{
    auto id = trim(arxivId);
    if (startsWith(toLower(id), "arxiv:"))
        id = id.substr(6);
    return "https://arxiv.org/pdf/" + id;
}

void downloadFile(const std::string& url, const fs::path& dest, bool requirePdf)
{
    std::error_code ec;
    if (dest.has_parent_path())
        fs::create_directories(dest.parent_path(), ec);
    auto part = dest;
    part += ".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out)
        throw MetadataError("Cannot write " + pathString(part) + ".");

    std::string head, sinkError;
    auto sink = [&](const char* data, size_t len) {
        if (requirePdf && head.size() < 1024) {
            head.append(data, std::min(len, 1024 - head.size()));
            if (head.size() >= 1024 && head.find("%PDF-") == std::string::npos) {
                sinkError = "The downloaded file is not a PDF (the site may require a login).";
                return false;
            }
        }
        out.write(data, (std::streamsize)len);
        if (!out) {
            sinkError = "Could not write " + pathString(part) + ".";
            return false;
        }
        return true;
    };
    auto fail = [&](const std::string& message) {
        out.close();
        fs::remove(part, ec);
        throw MetadataError(message);
    };

    Response r;
    try {
        r = httpGet(url, requirePdf ? "application/pdf,*/*;q=0.8" : "*/*", sink);
    } catch (const MetadataError& e) {
        fail(sinkError.empty() ? e.what() : sinkError);
    }
    out.close();
    if (r.status != 200)
        fail(r.status == 404 ? "The file was not found on the server (HTTP 404)."
                             : "Download failed (HTTP " + std::to_string(r.status) + ").");
    if (requirePdf && head.find("%PDF-") == std::string::npos)
        fail("The downloaded file is not a PDF (the site may require a login).");
    fs::rename(part, dest, ec);
    if (ec) {
        fs::remove(dest, ec);
        fs::rename(part, dest, ec);
        if (ec)
            fail("Cannot write " + pathString(dest) + ": " + ec.message());
    }
}

} // namespace on::formats
