#include "core/util.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace on {

static const char kKeyAlphabet[] = "23456789ABCDEFGHIJKLMNPQRSTUVWXYZ";

Json diskSpace(const fs::path& path)
{
    std::error_code ec;
    auto space = fs::space(path, ec);
    if (ec)
        return nullptr;
    return Json{{"capacity", (int64_t)space.capacity}, {"free", (int64_t)space.free}, {"available", (int64_t)space.available}};
}

int64_t fileSizeOr0(const fs::path& path)
{
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    return ec ? 0 : (int64_t)size;
}

std::string newKey()
{
    static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                            (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()};
    std::uniform_int_distribution<int> dist(0, (int)sizeof(kKeyAlphabet) - 2);
    std::string key(8, ' ');
    for (auto& c : key)
        c = kKeyAlphabet[dist(rng)];
    return key;
}

bool isValidKey(std::string_view key)
{
    if (key.empty() || key.size() > 64)
        return false;
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

static std::tm utcTm(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm;
}

std::string epochToIso(int64_t seconds)
{
    std::tm tm = utcTm((std::time_t)seconds);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string nowIso()
{
    return epochToIso(std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
}

int64_t nowMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string sqlToIso(const std::string& s)
{
    if (s.empty())
        return nowIso();
    std::string out = s;
    if (out.find('T') == std::string::npos)
        std::replace(out.begin(), out.end(), ' ', 'T');
    if (!endsWith(out, "Z"))
        out += 'Z';
    return out;
}

static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t isoToEpoch(const std::string& iso)
{
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3 &&
        std::sscanf(iso.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3)
        return 0;
    return daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
}

std::string isoToLocalDisplay(const std::string& iso)
{
    int64_t t = isoToEpoch(iso);
    if (!t)
        return iso;
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string yearOf(const std::string& date)
{
    for (size_t i = 0; i + 4 <= date.size(); ++i) {
        bool digits = true;
        for (size_t j = 0; j < 4; ++j)
            digits = digits && std::isdigit((unsigned char)date[i + j]);
        bool leftOk = i == 0 || !std::isdigit((unsigned char)date[i - 1]);
        bool rightOk = i + 4 == date.size() || !std::isdigit((unsigned char)date[i + 4]);
        if (digits && leftOk && rightOk)
            return date.substr(i, 4);
    }
    return {};
}

// --- Unicode folding ----------------------------------------------------------

static const char* const kLatin1Fold[64] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "y"};

static const char* const kLatinExtAFold[128] = {
    "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d",
    "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",
    "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i",
    "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l",
    "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",
    "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s",
    "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u",
    "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s"};

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static uint32_t decodeUtf8(std::string_view s, size_t& i)
{
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80)
        return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = extra == 3 ? (c & 0x07) : extra == 2 ? (c & 0x0F) : (c & 0x1F);
    if (!extra)
        return 0xFFFD;
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

std::string normalize(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp >= 'A' && cp <= 'Z') {
            out += (char)(cp + 32);
        } else if (cp < 0x80) {
            out += (char)cp;
        } else if (cp >= 0xC0 && cp <= 0xFF && kLatin1Fold[cp - 0xC0]) {
            out += kLatin1Fold[cp - 0xC0];
        } else if (cp >= 0x100 && cp <= 0x17F) {
            out += kLatinExtAFold[cp - 0x100];
        } else if (cp >= 0x300 && cp <= 0x36F) {
            // combining diacritical marks: drop
        } else if (cp >= 0x391 && cp <= 0x3A9) {
            appendUtf8(out, cp + 0x20);
        } else if (cp >= 0x410 && cp <= 0x42F) {
            appendUtf8(out, cp + 0x20);
        } else if (cp >= 0x400 && cp <= 0x40F) {
            appendUtf8(out, cp + 0x50);
        } else if (cp == 0xA0 || cp == 0x2009 || cp == 0x202F) {
            out += ' ';
        } else if (cp == 0x2010 || cp == 0x2011 || cp == 0x2013 || cp == 0x2014) {
            out += '-';
        } else if (cp == 0x2018 || cp == 0x2019) {
            out += '\'';
        } else if (cp == 0x201C || cp == 0x201D) {
            out += '"';
        } else {
            appendUtf8(out, cp);
        }
    }
    return out;
}

// --- strings --------------------------------------------------------------------

std::string trim(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b]))
        ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    return std::string(s.substr(b, e - b));
}

std::string toLower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        c = (char)std::tolower((unsigned char)c);
    return out;
}

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool endsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
           });
}

std::vector<std::string> split(std::string_view s, char sep, bool skipEmpty)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos)
            pos = s.size();
        auto part = s.substr(start, pos - start);
        if (!skipEmpty || !part.empty())
            out.emplace_back(part);
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out += sep;
        out += parts[i];
    }
    return out;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to)
{
    if (from.empty())
        return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::vector<std::string> searchTokens(std::string_view q, bool fold)
{
    std::vector<std::string> out;
    auto add = [&](std::string_view token) { out.push_back(fold ? normalize(token) : std::string(token)); };
    std::string cur;
    bool quoted = false;
    for (char c : q) {
        if (c == '"') {
            if (!cur.empty())
                add(cur);
            cur.clear();
            quoted = !quoted;
        } else if (!quoted && std::isspace((unsigned char)c)) {
            if (!cur.empty())
                add(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty())
        add(trim(cur));
    return out;
}

// --- HTML -----------------------------------------------------------------------

std::string htmlEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string htmlUnescape(std::string_view s)
{
    static const std::pair<const char*, const char*> named[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"}, {"hellip", "\xE2\x80\xA6"},
        {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"}, {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        auto ent = s.substr(i + 1, semi - i - 1);
        bool done = false;
        if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = 0;
            try {
                cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                         ? (uint32_t)std::stoul(std::string(ent.substr(2)), nullptr, 16)
                         : (uint32_t)std::stoul(std::string(ent.substr(1)));
            } catch (...) {
                cp = 0;
            }
            if (cp) {
                appendUtf8(out, cp);
                done = true;
            }
        } else {
            for (auto& [name, val] : named) {
                if (ent == name) {
                    out += val;
                    done = true;
                    break;
                }
            }
        }
        if (done)
            i = semi;
        else
            out += s[i];
    }
    return out;
}

std::string htmlToText(std::string_view html)
{
    static const char* blockTags[] = {"p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
                                      "blockquote", "pre", "ul", "ol", "table"};
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            size_t end = html.find('>', i);
            if (end == std::string_view::npos)
                break;
            auto tag = html.substr(i + 1, end - i - 1);
            if (!tag.empty() && tag[0] == '/')
                tag.remove_prefix(1);
            size_t nameEnd = 0;
            while (nameEnd < tag.size() && std::isalnum((unsigned char)tag[nameEnd]))
                ++nameEnd;
            std::string name = toLower(tag.substr(0, nameEnd));
            for (auto* b : blockTags) {
                if (name == b) {
                    out += '\n';
                    break;
                }
            }
            if (name == "style" || name == "script") {
                size_t close = html.find("</", end);
                end = close == std::string_view::npos ? html.size() - 1 : html.find('>', close);
                if (end == std::string_view::npos)
                    end = html.size() - 1;
            }
            i = end + 1;
        } else {
            out += html[i++];
        }
    }
    out = htmlUnescape(out);
    // collapse blank lines and trailing spaces
    std::string result;
    for (auto& line : split(out, '\n', true)) {
        auto t = trim(line);
        if (!t.empty()) {
            if (!result.empty())
                result += '\n';
            result += t;
        }
    }
    return result;
}

std::string noteTitle(std::string_view html)
{
    auto text = htmlToText(html);
    auto nl = text.find('\n');
    auto title = text.substr(0, nl);
    if (title.size() > 120) {
        size_t cut = 120;
        while (cut > 0 && ((unsigned char)title[cut] & 0xC0) == 0x80)
            --cut;
        title = title.substr(0, cut) + "\xE2\x80\xA6";
    }
    return title;
}

std::string textToNoteHtml(std::string_view text)
{
    std::string out = "<div data-schema-version=\"9\">";
    for (auto& para : split(text, '\n', true))
        out += "<p>" + htmlEscape(para) + "</p>";
    out += "</div>";
    return out;
}

std::string safeFilename(std::string_view name, std::string_view fallback)
{
    std::string out;
    for (char c : name) {
        unsigned char u = (unsigned char)c;
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    out = trim(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    while (!out.empty() && out.front() == '.')
        out.erase(out.begin());
    if (out.size() > 200) {
        auto ext = fs::path(out).extension().string();
        size_t cut = 200 - ext.size();
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80)
            --cut;
        out = out.substr(0, cut) + ext;
    }
    return out.empty() ? std::string(fallback) : out;
}

std::string humanSize(uint64_t n)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        ++u;
    }
    std::ostringstream os;
    os << std::fixed << std::setprecision(u ? 1 : 0) << v << ' ' << units[u];
    return os.str();
}

std::string urlEncode(std::string_view s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string urlDecode(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// --- creators -------------------------------------------------------------------

std::string creatorName(const Json& c)
{
    if (c.contains("name"))
        return jstr(c, "name");
    auto last = jstr(c, "lastName");
    return last.empty() ? jstr(c, "firstName") : last;
}

std::string creatorSummary(const Json& creators, const std::string& primaryType)
{
    if (!creators.is_array() || creators.empty())
        return {};
    std::vector<std::string> names;
    for (const char* type : {primaryType.c_str(), "editor", "contributor"}) {
        for (auto& c : creators)
            if (jstr(c, "creatorType") == type)
                names.push_back(creatorName(c));
        if (!names.empty())
            break;
    }
    if (names.empty())
        for (auto& c : creators)
            names.push_back(creatorName(c));
    if (names.size() == 1)
        return names[0];
    if (names.size() == 2)
        return names[0] + " and " + names[1];
    return names[0] + " et al.";
}

// --- files ----------------------------------------------------------------------

fs::path u8path(const std::string& utf8)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
}

std::string pathString(const fs::path& p)
{
#if defined(__cpp_char8_t)
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Error("cannot read " + pathString(path));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFileAtomic(const fs::path& path, std::string_view data)
{
    if (path.has_parent_path())
        fs::create_directories(path.parent_path());
    auto tmp = path;
    tmp += ".tmp" + std::to_string(nowMillis() % 100000);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw Error("cannot write " + pathString(tmp));
        out.write(data.data(), (std::streamsize)data.size());
        if (!out)
            throw Error("write failed: " + pathString(tmp));
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path);
    }
}

std::string guessContentType(const fs::path& path)
{
    static const std::pair<const char*, const char*> types[] = {
        {".pdf", "application/pdf"}, {".epub", "application/epub+zip"}, {".html", "text/html"},
        {".htm", "text/html"}, {".txt", "text/plain"}, {".md", "text/markdown"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
        {".svg", "image/svg+xml"}, {".djvu", "image/vnd.djvu"}, {".doc", "application/msword"},
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".odt", "application/vnd.oasis.opendocument.text"}, {".ppt", "application/vnd.ms-powerpoint"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".xls", "application/vnd.ms-excel"}, {".csv", "text/csv"}, {".json", "application/json"},
        {".zip", "application/zip"}, {".mp4", "video/mp4"}, {".mp3", "audio/mpeg"}, {".bib", "application/x-bibtex"},
        {".ris", "application/x-research-info-systems"}, {".xml", "application/xml"}};
    auto ext = toLower(pathString(path.extension()));
    for (auto& [e, t] : types)
        if (ext == e)
            return t;
    return "application/octet-stream";
}

} // namespace on
