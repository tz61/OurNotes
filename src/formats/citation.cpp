#include "formats/citation.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

#include "core/schema.h"
#include "core/util.h"
#include "formats/common.h"

namespace on::formats {

namespace {

// =================================================================================
// Reference model
// =================================================================================

struct Person {
    std::string family;
    std::string given;
    std::string suffix;  // "Jr."
    bool literal = false;
};

struct Ref {
    std::string type;
    std::vector<Person> authors;  // primary creators, or the editors when there are none
    std::vector<Person> editors;
    bool editorsAsAuthors = false;
    std::string title, container, containerShort, volume, issue, pages, publisher, place, edition;
    std::string doi, url, number, genre;
    DateParts issued, accessed;
};

Person makePerson(const Json& c)
{
    Person p;
    auto name = jstr(c, "name");
    if (!name.empty()) {
        p.family = name;
        p.literal = true;
        return p;
    }
    p.family = trim(jstr(c, "lastName"));
    p.given = trim(jstr(c, "firstName"));
    auto comma = p.given.find(", ");
    if (comma != std::string::npos) {
        p.suffix = trim(p.given.substr(comma + 2));
        p.given = trim(p.given.substr(0, comma));
    }
    if (p.family.empty()) {
        p.family = p.given;
        p.given.clear();
    }
    return p;
}

Ref makeRef(const Json& item)
{
    Ref r;
    r.type = jstr(item, "itemType");
    auto primary = schema::primaryCreatorType(r.type);
    for (auto& c : item.value("creators", Json::array())) {
        auto type = jstr(c, "creatorType");
        if (type == primary)
            r.authors.push_back(makePerson(c));
        else if (type == "editor")
            r.editors.push_back(makePerson(c));
    }
    if (r.authors.empty() && !r.editors.empty()) {
        std::swap(r.authors, r.editors);
        r.editorsAsAuthors = true;
    }
    auto f = [&](const char* name) { return trim(schema::getField(item, name)); };
    r.title = f("title");
    r.container = f("publicationTitle");
    r.containerShort = trim(jstr(item, "journalAbbreviation"));
    r.volume = f("volume");
    r.issue = trim(jstr(item, "issue"));
    r.pages = f("pages");
    r.publisher = f("publisher");
    r.place = f("place");
    r.edition = trim(jstr(item, "edition"));
    r.doi = trim(jstr(item, "DOI"));
    r.url = trim(jstr(item, "url"));
    r.number = f("number");
    r.genre = f("type");
    r.issued = parseDate(f("date"));
    r.accessed = parseDate(jstr(item, "accessDate"));
    return r;
}

bool isArticle(const Ref& r)
{
    return r.type == "journalArticle" || r.type == "magazineArticle" || r.type == "newspaperArticle";
}

bool isInContainer(const Ref& r)
{
    return r.type == "bookSection" || r.type == "conferencePaper" || r.type == "encyclopediaArticle" ||
           r.type == "dictionaryEntry";
}

bool isWebLike(const Ref& r) { return r.type == "webpage" || r.type == "blogPost" || r.type == "forumPost"; }

// =================================================================================
// Text helpers
// =================================================================================

class Markup {
public:
    explicit Markup(OutputFormat f) : html_(f == OutputFormat::Html) {}
    bool html() const { return html_; }
    std::string esc(std::string_view s) const { return html_ ? htmlEscape(s) : std::string(s); }
    std::string ital(std::string_view s) const { return wrap("i", s); }
    std::string bold(std::string_view s) const { return wrap("b", s); }

private:
    bool html_;
    std::string wrap(const char* tag, std::string_view s) const
    {
        if (s.empty() || !html_)
            return std::string(s);
        return std::string("<") + tag + ">" + htmlEscape(s) + "</" + tag + ">";
    }
};

// Last visible character, skipping trailing HTML tags.
char lastVisible(const std::string& s)
{
    size_t i = s.size();
    while (i > 0) {
        if (s[i - 1] == '>') {
            auto lt = s.rfind('<', i - 1);
            if (lt == std::string::npos)
                return '>';
            i = lt;
            continue;
        }
        return s[i - 1];
    }
    return 0;
}

bool endsSentence(const std::string& s)
{
    char c = lastVisible(s);
    return c == '.' || c == '?' || c == '!';
}

std::string period(const std::string& s)
{
    if (s.empty() || endsSentence(s))
        return s;
    return s + ".";
}

// “Title.” / “Title,” (no extra punctuation after ? or !)
std::string quoted(const Markup& m, const std::string& title, const char* punct, const char* open = "“",
                   const char* close = "”")
{
    std::string t = m.esc(title);
    if (!endsSentence(t))
        t += punct;
    return open + t + close;
}

std::string joinNonEmpty(const std::vector<std::string>& parts, const std::string& sep)
{
    std::vector<std::string> kept;
    for (auto& p : parts)
        if (!p.empty())
            kept.push_back(p);
    return join(kept, sep);
}

// "a", "a<two>b", "a<sep>b<last>c"
std::string joinList(const std::vector<std::string>& names, const std::string& sep, const std::string& lastSep,
                     const std::string& twoSep)
{
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names[0];
    if (names.size() == 2)
        return names[0] + twoSep + names[1];
    std::vector<std::string> head(names.begin(), names.end() - 1);
    return join(head, sep) + lastSep + names.back();
}

std::string upperAscii(std::string s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
        s[0] = (char)(s[0] - 32);
    return s;
}

// "John Ronald" -> "J. R." / "JR"; "Jean-Paul" -> "J.-P." / "JP"
std::string initials(const std::string& given, const std::string& dot, const std::string& space, bool hyphen = true)
{
    std::string out;
    for (auto& word : split(replaceAll(given, ".", ". "), ' ')) {
        bool attach = word[0] == '-';
        auto pieces = split(word, '-');
        for (size_t k = 0; k < pieces.size(); ++k) {
            auto& piece = pieces[k];
            size_t start = 0;  // skip ASCII punctuation such as "("
            while (start < piece.size() && (unsigned char)piece[start] < 0x80 &&
                   !std::isalpha((unsigned char)piece[start]))
                ++start;
            if (start >= piece.size())
                continue;
            if (k == 0 && !attach && piece[start] >= 'a' && piece[start] <= 'z' && piece.size() - start > 1 &&
                piece.back() != '.')
                continue;  // particles such as "van"
            if (!out.empty())
                out += (k > 0 || attach) ? (hyphen ? std::string("-") : std::string()) : space;
            out += upperAscii(firstChar(std::string_view(piece).substr(start))) + dot;
        }
    }
    return out;
}

enum class NameForm { FamilyInitials, FamilyInitialsCompact, InitialsFamily, FamilyGiven, GivenFamily, Vancouver };

std::string formatName(const Person& p, NameForm form)
{
    if (p.literal || p.given.empty())
        return p.suffix.empty() ? p.family : p.family + ", " + p.suffix;
    std::string s;
    switch (form) {
    case NameForm::FamilyInitials:
        s = p.family + ", " + initials(p.given, ".", " ");
        return p.suffix.empty() ? s : s + ", " + p.suffix;
    case NameForm::FamilyInitialsCompact:
        s = p.family + ", " + initials(p.given, ".", "");
        return p.suffix.empty() ? s : s + ", " + p.suffix;
    case NameForm::InitialsFamily:
        s = initials(p.given, ".", " ") + " " + p.family;
        return p.suffix.empty() ? s : s + " " + p.suffix;
    case NameForm::FamilyGiven:
        s = p.family + ", " + p.given;
        return p.suffix.empty() ? s : s + ", " + p.suffix;
    case NameForm::GivenFamily:
        s = p.given + " " + p.family;
        return p.suffix.empty() ? s : s + " " + p.suffix;
    case NameForm::Vancouver:
        s = p.family + " " + initials(p.given, "", "", false);
        return p.suffix.empty() ? s : s + " " + replaceAll(p.suffix, ".", "");
    }
    return s;
}

std::vector<std::string> formatNames(const std::vector<Person>& people, NameForm form, size_t limit = SIZE_MAX)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < people.size() && i < limit; ++i)
        out.push_back(formatName(people[i], form));
    return out;
}

std::string ordinal(const std::string& edition)
{
    if (edition.empty() || !std::all_of(edition.begin(), edition.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return edition;
    // its last two digits decide (a long number, an ISBN typed here, does not fit in an int)
    int n = std::stoi(edition.substr(edition.size() - std::min<size_t>(edition.size(), 2)));
    const char* suffix = "th";
    if (n % 100 < 11 || n % 100 > 13) {
        if (n % 10 == 1)
            suffix = "st";
        else if (n % 10 == 2)
            suffix = "nd";
        else if (n % 10 == 3)
            suffix = "rd";
    }
    return edition + suffix;
}

std::string bareDoi(const std::string& doi)  // "10.x/y" even if stored as a URL
{
    auto clean = cleanDoi(doi);
    return clean.empty() ? doi : clean;
}

std::string doiLink(const std::string& doi) { return "https://doi.org/" + bareDoi(doi); }

std::string placePublisher(const Ref& r, const std::string& sep = ": ")
{
    if (!r.place.empty() && !r.publisher.empty())
        return r.place + sep + r.publisher;
    return r.place.empty() ? r.publisher : r.place;
}

// --- pages ------------------------------------------------------------------------

enum class PageFormat { Expanded, Chicago, MinimalTwo, Minimal };

bool allDigits(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool isPageRange(const std::string& pages)
{
    return pages.find_first_of("-,") != std::string::npos || pages.find("–") != std::string::npos;
}

std::string pageRange(const std::string& raw, PageFormat fmt)
{
    auto p = replaceAll(replaceAll(raw, "–", "-"), "—", "-");
    while (p.find("--") != std::string::npos)
        p = replaceAll(p, "--", "-");
    auto dash = p.find('-');
    if (dash == std::string::npos || p.find(',') != std::string::npos || p.find('-', dash + 1) != std::string::npos)
        return replaceAll(p, "-", "–");
    auto a = trim(p.substr(0, dash)), b = trim(p.substr(dash + 1));
    if (!allDigits(a) || !allDigits(b))
        return a + "–" + b;
    if (b.size() < a.size())
        b = a.substr(0, a.size() - b.size()) + b;  // "436-44" -> 436-444
    if (fmt == PageFormat::Expanded || a.size() != b.size())
        return a + "–" + b;
    size_t diff = 0;
    while (diff < a.size() && a[diff] == b[diff])
        ++diff;
    if (diff == a.size())
        return a + "–" + b;
    auto minimal = [&](size_t keep) {
        size_t start = std::min(diff, b.size() > keep ? b.size() - keep : 0);
        return a + "–" + b.substr(start);
    };
    switch (fmt) {
    case PageFormat::Minimal:
        return minimal(1);
    case PageFormat::MinimalTwo:
        return minimal(2);
    case PageFormat::Chicago: {
        // a number too long for a long is a large one: only its last digits matter
        bool large = a.size() > 9;
        long start = std::stol(large ? a.substr(a.size() - 9) : a);
        if ((start < 100 && !large) || start % 100 == 0)
            return a + "–" + b;
        if (start % 100 < 10)
            return minimal(1);
        if (a.size() == 4 && a.size() - diff >= 3)
            return a + "–" + b;
        return minimal(2);
    }
    default:
        return a + "–" + b;
    }
}

std::string pagesWithLabel(const std::string& pages, PageFormat fmt, const char* single = "p. ",
                           const char* multiple = "pp. ")
{
    return (isPageRange(pages) ? multiple : single) + pageRange(pages, fmt);
}

// --- dates ------------------------------------------------------------------------

const char* const kMonthIeee[] = {"Jan.", "Feb.", "Mar.", "Apr.", "May", "Jun.",
                                  "Jul.", "Aug.", "Sep.", "Oct.", "Nov.", "Dec."};
const char* const kMonthMla[] = {"Jan.", "Feb.", "Mar.", "Apr.", "May", "June",
                                 "July", "Aug.", "Sept.", "Oct.", "Nov.", "Dec."};
const char* const kMonthShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string year(const DateParts& d, const std::string& missing = "n.d.")
{
    return d.valid() ? std::to_string(d.year) : missing;
}

std::string twoDigits(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

// "May 28, 2015" / "May 2015" / "2015"
std::string monthDayYear(const DateParts& d)
{
    if (!d.valid())
        return {};
    if (!d.month)
        return std::to_string(d.year);
    std::string s = monthName(d.month);
    if (d.day)
        s += " " + std::to_string(d.day) + ",";
    return s + " " + std::to_string(d.year);
}

// "28 May 2015" with the given month names
std::string dayMonthYear(const DateParts& d, const char* const* months)
{
    if (!d.valid())
        return {};
    std::string s;
    if (d.month) {
        if (d.day)
            s = std::to_string(d.day) + " ";
        s += months[d.month - 1] + std::string(" ");
    }
    return s + std::to_string(d.year);
}

const char* const kMonthLong[] = {"January", "February", "March",     "April",   "May",      "June",
                                  "July",    "August",   "September", "October", "November", "December"};

// --- thesis labels ----------------------------------------------------------------

enum class ThesisKind { Doctoral, Masters, Other };

ThesisKind thesisKind(const std::string& genre)
{
    auto g = toLower(genre);
    if (g.find("phd") != std::string::npos || g.find("ph.d") != std::string::npos ||
        g.find("doctor") != std::string::npos || g.find("dissertation") != std::string::npos)
        return ThesisKind::Doctoral;
    if (g.find("master") != std::string::npos || startsWith(g, "ms") || startsWith(g, "m.s") ||
        startsWith(g, "m.a") || startsWith(g, "ma ") || g == "ma")
        return ThesisKind::Masters;
    return ThesisKind::Other;
}

std::string thesisLabel(const Ref& r, const char* doctoral, const char* masters, const std::string& other)
{
    switch (thesisKind(r.genre)) {
    case ThesisKind::Doctoral: return doctoral;
    case ThesisKind::Masters: return masters;
    default: return r.genre.empty() ? other : r.genre;
    }
}

bool usesFullDate(const std::string& type)
{
    static const char* const types[] = {"webpage",   "blogPost", "forumPost", "newspaperArticle", "magazineArticle",
                                        "presentation", "email", "letter",    "instantMessage",   "interview",
                                        "podcast",   "tvBroadcast", "radioBroadcast"};
    return std::any_of(std::begin(types), std::end(types), [&](const char* t) { return type == t; });
}

// =================================================================================
// APA 7th edition
// =================================================================================

std::string apaNames(const std::vector<Person>& people)
{
    auto names = formatNames(people, NameForm::FamilyInitials);
    if (names.size() > 20) {
        std::vector<std::string> first(names.begin(), names.begin() + 19);
        return join(first, ", ") + ", … " + names.back();
    }
    return joinList(names, ", ", ", & ", ", & ");
}

std::string apaDate(const Ref& r)
{
    if (!r.issued.valid())
        return "n.d.";
    auto y = std::to_string(r.issued.year);
    if (usesFullDate(r.type) && r.issued.month) {
        y += ", " + std::string(monthName(r.issued.month));
        if (r.issued.day)
            y += " " + std::to_string(r.issued.day);
    }
    return y;
}

std::string apaEntry(const Ref& r, const Markup& m)
{
    bool article = isArticle(r), inContainer = isInContainer(r);
    bool italicTitle = !(article || inContainer || r.type == "blogPost" || r.type == "forumPost");

    std::string title = italicTitle ? m.ital(r.title) : m.esc(r.title);
    std::vector<std::string> paren;
    if (!inContainer && !r.edition.empty())
        paren.push_back(ordinal(r.edition) + " ed.");
    if (r.type == "report" && !r.number.empty())
        paren.push_back((r.genre.empty() ? std::string("Report") : r.genre) + " No. " + r.number);
    if (r.type == "preprint" && !r.number.empty())
        paren.push_back(r.number);
    if (!paren.empty())
        title += " (" + m.esc(join(paren, ", ")) + ")";
    if (r.type == "thesis")
        title += " [" + m.esc(joinNonEmpty({thesisLabel(r, "Doctoral dissertation", "Master's thesis", "Thesis"),
                                            r.publisher},
                                           ", ")) +
                 "]";
    else if (r.type == "dataset")
        title += " [Data set]";
    else if (r.type == "computerProgram")
        title += " [Computer software]";

    std::string out;
    auto date = "(" + apaDate(r) + ").";
    if (!r.authors.empty()) {
        auto names = apaNames(r.authors);
        if (r.editorsAsAuthors)
            names += r.authors.size() > 1 ? " (Eds.)" : " (Ed.)";
        out = period(m.esc(names)) + " " + date + " " + period(title);
    } else {
        out = period(title) + " " + date;
    }

    std::string source;
    if (article) {
        source = m.ital(r.container);
        if (!r.volume.empty())
            source += (source.empty() ? "" : ", ") + m.ital(r.volume);
        if (!r.issue.empty())
            source += "(" + m.esc(r.issue) + ")";
        if (!r.pages.empty())
            source += (source.empty() ? "" : ", ") + m.esc(pageRange(r.pages, PageFormat::Expanded));
    } else if (inContainer && (!r.container.empty() || !r.editors.empty())) {
        source = "In ";
        if (!r.editors.empty()) {
            auto eds = joinList(formatNames(r.editors, NameForm::InitialsFamily), ", ", ", & ", " & ");
            source += m.esc(eds + (r.editors.size() > 1 ? " (Eds.)" : " (Ed.)")) + ", ";
        }
        source += m.ital(r.container);
        std::vector<std::string> inner;
        if (!r.edition.empty())
            inner.push_back(ordinal(r.edition) + " ed.");
        if (!r.volume.empty())
            inner.push_back("Vol. " + r.volume);
        if (!r.pages.empty())
            inner.push_back(pagesWithLabel(r.pages, PageFormat::Expanded));
        if (!inner.empty())
            source += " (" + m.esc(join(inner, ", ")) + ")";
        if (!r.publisher.empty())
            source = period(source) + " " + m.esc(r.publisher);
    } else if (isWebLike(r)) {
        source = r.type == "webpage" ? m.esc(r.container) : m.ital(r.container);
    } else if (r.type != "thesis") {
        source = m.esc(r.publisher);
    }
    if (!source.empty())
        out += " " + period(source);

    if (!r.doi.empty())
        out += " " + m.esc(doiLink(r.doi));
    else if (!r.url.empty() && !r.issued.valid() && r.accessed.valid())
        out += " Retrieved " + m.esc(monthDayYear(r.accessed)) + ", from " + m.esc(r.url);
    else if (!r.url.empty())
        out += " " + m.esc(r.url);
    return out;
}

// =================================================================================
// Chicago Manual of Style 17th edition (author-date)
// =================================================================================

std::string chicagoNames(const std::vector<Person>& people)
{
    std::vector<std::string> names;
    size_t n = people.size() > 10 ? 7 : people.size();
    for (size_t i = 0; i < n; ++i)
        names.push_back(formatName(people[i], i == 0 ? NameForm::FamilyGiven : NameForm::GivenFamily));
    if (people.size() > 10)
        return join(names, ", ") + ", et al.";
    return joinList(names, ", ", ", and ", ", and ");
}

std::string chicagoEntry(const Ref& r, const Markup& m)
{
    bool article = isArticle(r), inContainer = isInContainer(r);
    bool quotedTitle = article || inContainer || isWebLike(r) || r.type == "thesis" || r.type == "preprint";
    auto title = quotedTitle ? quoted(m, r.title, ".") : period(m.ital(r.title));
    auto yr = year(r.issued);

    std::string out;
    if (!r.authors.empty()) {
        auto names = chicagoNames(r.authors);
        if (r.editorsAsAuthors)
            names += r.authors.size() > 1 ? ", eds." : ", ed.";
        out = period(m.esc(names)) + " " + period(yr) + " " + title;
    } else {
        out = title + " " + period(yr);
    }

    std::string rest;
    if (article) {
        auto s = m.ital(r.container);
        if (!r.volume.empty())
            s += " " + m.esc(r.volume);
        if (!r.issue.empty())
            s += " (" + m.esc(r.issue) + ")";
        if (!r.pages.empty())
            s += (r.volume.empty() && r.issue.empty() ? ", " : ": ") + m.esc(pageRange(r.pages, PageFormat::Chicago));
        rest = period(s);
    } else if (inContainer) {
        auto s = "In " + m.ital(r.container);
        if (!r.editors.empty()) {
            auto eds = joinList(formatNames(r.editors, NameForm::GivenFamily), ", ", ", and ", " and ");
            s += ", edited by " + m.esc(eds);
        }
        if (!r.pages.empty())
            s += ", " + m.esc(pageRange(r.pages, PageFormat::Chicago));
        rest = period(s);
        auto pp = placePublisher(r);
        if (!pp.empty())
            rest += " " + period(m.esc(pp));
    } else if (r.type == "thesis") {
        auto label = thesisLabel(r, "PhD diss.", "Master's thesis", "Thesis");
        rest = period(m.esc(joinNonEmpty({label, r.publisher}, ", ")));
    } else if (isWebLike(r)) {
        rest = r.container.empty() ? "" : period(m.esc(r.container));
        if (r.issued.month)
            rest += " " + period(m.esc(monthDayYear(r.issued)));
        else if (!r.issued.valid() && r.accessed.valid() && !r.url.empty())
            rest += " Accessed " + period(m.esc(monthDayYear(r.accessed)));
    } else if (r.type == "preprint") {
        rest = period(m.esc(r.publisher.empty() ? r.number : r.publisher));
    } else {
        if (!r.edition.empty())
            rest = period(m.esc(ordinal(r.edition) + " ed"));
        if (r.type == "report" && !r.number.empty())
            rest += " " + period(m.esc(joinNonEmpty({r.genre, r.number}, " ")));
        auto pp = placePublisher(r);
        if (!pp.empty())
            rest += " " + period(m.esc(pp));
    }
    if (!trim(rest).empty())
        out += " " + trim(rest);
    if (!r.doi.empty())
        out += " " + m.esc(doiLink(r.doi)) + ".";
    else if (!r.url.empty())
        out += " " + m.esc(r.url) + ".";
    return out;
}

// =================================================================================
// IEEE
// =================================================================================

std::string ieeeNames(const Ref& r)
{
    if (r.authors.empty())
        return {};
    std::string s = r.authors.size() >= 7
                        ? formatName(r.authors[0], NameForm::InitialsFamily) + " et al."
                        : joinList(formatNames(r.authors, NameForm::InitialsFamily), ", ", ", and ", " and ");
    if (r.editorsAsAuthors)
        s += r.authors.size() > 1 ? ", Eds." : ", Ed.";
    return s;
}

std::string ieeeDate(const DateParts& d, bool withDay)
{
    if (!d.valid())
        return {};
    std::string s;
    if (d.month) {
        s = kMonthIeee[d.month - 1];
        if (withDay && d.day)
            s += " " + twoDigits(d.day) + ",";
        s += " ";
    }
    return s + std::to_string(d.year);
}

// Appends ", doi: X." / ". [Online]. Available: URL" to comma-separated parts.
std::string ieeeClose(const Ref& r, const Markup& m, std::vector<std::string> parts, bool doiInline)
{
    std::string out;
    if (!r.doi.empty()) {
        if (doiInline) {
            parts.push_back("doi: " + m.esc(bareDoi(r.doi)));
            return period(joinNonEmpty(parts, ", "));
        }
        return period(joinNonEmpty(parts, ", ")) + " doi: " + m.esc(bareDoi(r.doi)) + ".";
    }
    if (!r.url.empty()) {
        if (r.accessed.valid())
            parts.push_back("Accessed: " + ieeeDate(r.accessed, true));
        return period(joinNonEmpty(parts, ", ")) + " [Online]. Available: " + m.esc(r.url);
    }
    return period(joinNonEmpty(parts, ", "));
}

std::string ieeeEntry(const Ref& r, const Markup& m)
{
    auto names = m.esc(ieeeNames(r));
    auto head = names.empty() ? std::string() : names + ", ";
    auto q = quoted(m, r.title, ",") + " ";

    if (isArticle(r)) {
        std::vector<std::string> parts = {m.ital(r.container)};
        if (!r.volume.empty())
            parts.push_back("vol. " + m.esc(r.volume));
        if (!r.issue.empty())
            parts.push_back("no. " + m.esc(r.issue));
        if (!r.pages.empty())
            parts.push_back(m.esc(pagesWithLabel(r.pages, PageFormat::Expanded)));
        parts.push_back(ieeeDate(r.issued, r.type != "journalArticle"));
        return head + q + ieeeClose(r, m, parts, true);
    }
    if (isInContainer(r)) {
        std::vector<std::string> parts = {"in " + m.ital(r.container)};
        if (!r.editors.empty())
            parts.push_back(m.esc(joinList(formatNames(r.editors, NameForm::InitialsFamily), ", ", ", and ", " and ") +
                                  (r.editors.size() > 1 ? ", Eds." : ", Ed.")));
        if (!r.edition.empty())
            parts.push_back(m.esc(ordinal(r.edition) + " ed."));
        parts.push_back(m.esc(placePublisher(r)));
        parts.push_back(ieeeDate(r.issued, false));
        if (!r.pages.empty())
            parts.push_back(m.esc(pagesWithLabel(r.pages, PageFormat::Expanded)));
        return head + q + ieeeClose(r, m, parts, false);
    }
    if (r.type == "thesis") {
        std::vector<std::string> parts = {m.esc(thesisLabel(r, "Ph.D. dissertation", "M.S. thesis", "Thesis")),
                                          m.esc(r.publisher), m.esc(r.place), ieeeDate(r.issued, false)};
        return head + q + ieeeClose(r, m, parts, false);
    }
    if (r.type == "report") {
        auto label = r.genre.empty() ? std::string("Rep.") : r.genre;
        std::vector<std::string> parts = {m.esc(r.publisher), m.esc(r.place),
                                          r.number.empty() ? std::string() : m.esc(label + " " + r.number),
                                          ieeeDate(r.issued, false)};
        return head + q + ieeeClose(r, m, parts, false);
    }
    if (r.type == "preprint") {
        std::string source = m.esc(r.publisher);
        if (!r.number.empty())
            source = source.empty() ? m.esc(r.number) : source + ": " + m.esc(r.number);
        std::vector<std::string> parts = {ieeeDate(r.issued, true), source};
        return head + q + ieeeClose(r, m, parts, false);
    }
    if (isWebLike(r)) {
        auto out = head + quoted(m, r.title, ".");
        auto site = joinNonEmpty({m.esc(r.container), ieeeDate(r.issued, true)}, ", ");
        if (!site.empty())
            out += " " + period(site);
        if (r.accessed.valid())
            out += " Accessed: " + ieeeDate(r.accessed, true) + ".";
        if (!r.url.empty())
            out += " [Online]. Available: " + m.esc(r.url);
        return out;
    }
    // books and other stand-alone works
    auto out = head + m.ital(r.title);
    if (!r.edition.empty())
        out += ", " + m.esc(ordinal(r.edition) + " ed.");
    out = period(out);
    auto tail = joinNonEmpty({m.esc(placePublisher(r)), ieeeDate(r.issued, false)}, ", ");
    if (!tail.empty())
        out += " " + period(tail);
    if (!r.doi.empty())
        out += " doi: " + m.esc(bareDoi(r.doi)) + ".";
    else if (!r.url.empty())
        out += " [Online]. Available: " + m.esc(r.url);
    return out;
}

// =================================================================================
// MLA 9th edition
// =================================================================================

std::string mlaNames(const Ref& r)
{
    const auto& a = r.authors;
    if (a.empty())
        return {};
    std::string s = formatName(a[0], NameForm::FamilyGiven);
    if (a.size() == 2)
        s += ", and " + formatName(a[1], NameForm::GivenFamily);
    else if (a.size() > 2)
        s += ", et al.";
    if (r.editorsAsAuthors)
        s += a.size() > 1 ? ", editors" : ", editor";
    return s;
}

std::string mlaEntry(const Ref& r, const Markup& m)
{
    auto names = mlaNames(r);
    std::string out = names.empty() ? std::string() : period(m.esc(names)) + " ";
    bool quotedTitle = isArticle(r) || isInContainer(r) || isWebLike(r);
    out += quotedTitle ? quoted(m, r.title, ".") : period(m.ital(r.title));

    auto date = dayMonthYear(r.issued, kMonthMla);
    auto locator = !r.doi.empty() ? doiLink(r.doi) : r.url;
    std::vector<std::string> parts;  // the MLA "container" elements, comma separated
    if (isArticle(r)) {
        parts = {m.ital(r.container), r.volume.empty() ? "" : "vol. " + m.esc(r.volume),
                 r.issue.empty() ? "" : "no. " + m.esc(r.issue), m.esc(date),
                 r.pages.empty() ? "" : m.esc(pagesWithLabel(r.pages, PageFormat::MinimalTwo))};
    } else if (isInContainer(r)) {
        std::string eds;
        if (!r.editors.empty())
            eds = "edited by " +
                  m.esc(joinList(formatNames(r.editors, NameForm::GivenFamily), ", ", ", and ", " and "));
        parts = {m.ital(r.container), eds,
                 r.edition.empty() ? "" : m.esc(ordinal(r.edition) + " ed."), m.esc(r.publisher), m.esc(date),
                 r.pages.empty() ? "" : m.esc(pagesWithLabel(r.pages, PageFormat::MinimalTwo))};
    } else if (r.type == "thesis") {
        if (!date.empty())
            out += " " + period(m.esc(date));
        parts = {m.esc(r.publisher), m.esc(thesisLabel(r, "PhD dissertation", "MA thesis", "Thesis"))};
        date.clear();
    } else if (r.type == "preprint") {
        parts = {m.esc(r.number), m.esc(r.publisher), m.esc(date)};
    } else if (isWebLike(r)) {
        parts = {m.ital(r.container), m.esc(date)};
    } else {
        parts = {r.edition.empty() ? "" : m.esc(ordinal(r.edition) + " ed."), m.esc(r.publisher), m.esc(date)};
    }
    parts.push_back(m.esc(locator));
    auto container = joinNonEmpty(parts, ", ");
    if (!container.empty())
        out += " " + period(container);
    if (isWebLike(r) && !r.issued.valid() && r.accessed.valid())
        out += " Accessed " + m.esc(dayMonthYear(r.accessed, kMonthMla)) + ".";
    return out;
}

// =================================================================================
// Harvard (Cite Them Right 12th edition)
// =================================================================================

std::string harvardNames(const std::vector<Person>& people)
{
    if (people.size() >= 4)
        return formatName(people[0], NameForm::FamilyInitialsCompact) + " et al.";
    return joinList(formatNames(people, NameForm::FamilyInitialsCompact), ", ", " and ", " and ");
}

std::string harvardEntry(const Ref& r, const Markup& m)
{
    bool quotedTitle = isArticle(r) || isInContainer(r) || r.type == "preprint";
    auto title = quotedTitle ? "‘" + m.esc(r.title) + "’" : m.ital(r.title);
    auto yr = "(" + year(r.issued, "no date") + ")";

    std::string out;
    if (!r.authors.empty()) {
        auto names = harvardNames(r.authors);
        if (r.editorsAsAuthors)
            names += r.authors.size() > 1 ? " (eds.)" : " (ed.)";
        out = m.esc(names) + " " + yr + " " + title;
    } else {
        out = title + " " + yr;
    }

    if (isArticle(r)) {
        std::string s = m.ital(r.container);
        auto volIssue = m.esc(r.volume) + (r.issue.empty() ? "" : "(" + m.esc(r.issue) + ")");
        out += ", " + joinNonEmpty({s, volIssue,
                                    r.pages.empty() ? "" : m.esc(pagesWithLabel(r.pages, PageFormat::Expanded))},
                                   ", ");
        out = period(out);
    } else if (isInContainer(r)) {
        out += ", in ";
        if (!r.editors.empty())
            out += m.esc(joinList(formatNames(r.editors, NameForm::FamilyInitialsCompact), ", ", " and ", " and ") +
                         (r.editors.size() > 1 ? " (eds.)" : " (ed.)")) +
                   " ";
        out += period(m.ital(r.container));
        auto tail = joinNonEmpty(
            {m.esc(placePublisher(r)), r.pages.empty() ? "" : m.esc(pagesWithLabel(r.pages, PageFormat::Expanded))},
            ", ");
        if (!tail.empty())
            out += " " + period(tail);
    } else {
        out = period(out);
        std::vector<std::string> sentences;
        if (!r.edition.empty())
            sentences.push_back(ordinal(r.edition) + " edn");
        if (r.type == "thesis") {
            sentences.push_back(thesisLabel(r, "PhD thesis", "Master's thesis", "Thesis"));
            sentences.push_back(r.publisher);
        } else if (r.type == "report") {
            if (!r.number.empty())
                sentences.push_back(joinNonEmpty({r.genre.empty() ? "Report" : r.genre, r.number}, " "));
            sentences.push_back(placePublisher(r));
        } else if (r.type == "preprint") {
            sentences.push_back(r.publisher);
        } else if (!isWebLike(r)) {
            sentences.push_back(placePublisher(r));
        }
        for (auto& s : sentences)
            if (!s.empty())
                out += " " + period(m.esc(s));
    }

    if (!r.doi.empty())
        out += " Available at: " + m.esc(doiLink(r.doi)) + ".";
    else if (!r.url.empty())
        out += " Available at: " + m.esc(r.url) +
               (r.accessed.valid() ? " (Accessed: " + dayMonthYear(r.accessed, kMonthLong) + ")" : "") + ".";
    return out;
}

// =================================================================================
// Nature
// =================================================================================

std::string natureNames(const Ref& r)
{
    if (r.authors.empty())
        return {};
    std::string s = r.authors.size() >= 6
                        ? formatName(r.authors[0], NameForm::FamilyInitials) + " et al."
                        : joinList(formatNames(r.authors, NameForm::FamilyInitials), ", ", " & ", " & ");
    if (r.editorsAsAuthors)
        s += r.authors.size() > 1 ? " (eds.)" : " (ed.)";
    return s;
}

std::string natureEntry(const Ref& r, const Markup& m)
{
    auto names = natureNames(r);
    std::string out = names.empty() ? std::string() : period(m.esc(names)) + " ";
    auto yr = r.issued.valid() ? std::to_string(r.issued.year) : std::string();
    auto paren = [&](std::vector<std::string> parts) {
        auto s = joinNonEmpty(parts, ", ");
        return s.empty() ? std::string() : "(" + s + ")";
    };
    auto doi = bareDoi(r.doi);

    if (isArticle(r)) {
        out += period(m.esc(r.title)) + " " + m.ital(r.containerShort.empty() ? r.container : r.containerShort);
        if (!r.volume.empty())
            out += " " + m.bold(r.volume);
        if (!r.pages.empty())
            out += ", " + m.esc(pageRange(r.pages, PageFormat::Expanded));
        auto p = paren({yr});
        return out + (p.empty() ? "" : " " + p) + ".";
    }
    if (isInContainer(r)) {
        out += period(m.esc(r.title)) + " in " + m.ital(r.container);
        if (!r.editors.empty())
            out += " " + m.esc("(" + std::string(r.editors.size() > 1 ? "eds " : "ed. ") +
                               joinList(formatNames(r.editors, NameForm::FamilyInitials), ", ", " & ", " & ") + ")");
        if (!r.pages.empty())
            out += " " + m.esc(pageRange(r.pages, PageFormat::Expanded));
        auto p = paren({m.esc(r.publisher), yr});
        out += (p.empty() ? "" : " " + p) + ".";
        if (!doi.empty())
            out += " doi:" + m.esc(doi) + ".";
        return out;
    }
    if (r.type == "preprint") {
        auto link = !r.doi.empty() ? doiLink(r.doi) : r.url;
        out += period(m.esc(r.title));
        if (!link.empty())
            out += " Preprint at " + m.esc(link);
        auto p = paren({yr});
        return out + (p.empty() ? "" : " " + p) + ".";
    }
    if (isWebLike(r)) {
        out += period(m.esc(r.title));
        if (!r.container.empty())
            out += " " + m.ital(r.container);
        if (!r.url.empty())
            out += " " + m.esc(r.url);
        auto p = paren({yr});
        return out + (p.empty() ? "" : " " + p) + ".";
    }
    out += period(m.ital(r.title));
    std::vector<std::string> inner;
    if (!r.edition.empty())
        inner.push_back(m.esc(ordinal(r.edition) + " edn"));
    if (r.type == "thesis")
        inner.push_back(m.esc(thesisLabel(r, "PhD thesis", "Master's thesis", "Thesis")));
    if (r.type == "report" && !r.number.empty())
        inner.push_back(m.esc(joinNonEmpty({r.genre.empty() ? "Report" : r.genre, r.number}, " ")));
    inner.push_back(m.esc(r.publisher));
    inner.push_back(yr);
    auto p = paren(inner);
    if (!p.empty())
        out += " " + p + ".";
    if (!doi.empty())
        out += " doi:" + m.esc(doi) + ".";
    return out;
}

// =================================================================================
// Vancouver (NLM / ICMJE)
// =================================================================================

std::string vancouverNames(const Ref& r)
{
    if (r.authors.empty())
        return {};
    auto names = formatNames(r.authors, NameForm::Vancouver, 6);
    std::string s = join(names, ", ");
    if (r.authors.size() > 6)
        s += ", et al";
    if (r.editorsAsAuthors)
        s += r.authors.size() > 1 ? ", editors" : ", editor";
    return s;
}

std::string vancouverDate(const DateParts& d, bool full)
{
    if (!d.valid())
        return {};
    std::string s = std::to_string(d.year);
    if (full && d.month) {
        s += std::string(" ") + kMonthShort[d.month - 1];
        if (d.day)
            s += " " + std::to_string(d.day);
    }
    return s;
}

std::string vancouverEntry(const Ref& r, const Markup& m)
{
    bool online = r.doi.empty() && !r.url.empty();
    auto internet = online ? std::string(" [Internet]") : std::string();
    auto cited = online && r.accessed.valid() ? " [cited " + vancouverDate(r.accessed, true) + "]" : std::string();
    auto names = vancouverNames(r);
    std::string out = names.empty() ? std::string() : period(m.esc(names)) + " ";
    auto pubYear = [&](bool full) {
        auto pp = placePublisher(r);
        auto date = trim(vancouverDate(r.issued, full) + cited);
        if (pp.empty())
            return date.empty() ? std::string() : period(m.esc(date));
        return period(m.esc(pp + (date.empty() ? "" : "; " + date)));
    };

    if (isArticle(r)) {
        auto journal = r.containerShort.empty() ? r.container : replaceAll(r.containerShort, ".", "");
        out += period(m.esc(r.title)) + " " + period(m.esc(journal) + internet);
        std::string s = trim(vancouverDate(r.issued, true) + cited);
        if (!r.volume.empty() || !r.issue.empty())
            s += ";" + r.volume + (r.issue.empty() ? "" : "(" + r.issue + ")");
        if (!r.pages.empty())
            s += ":" + pageRange(r.pages, PageFormat::Minimal);
        if (!trim(s).empty())
            out += " " + period(m.esc(s));
    } else if (isInContainer(r)) {
        out += period(m.esc(r.title)) + " In: ";
        if (!r.editors.empty())
            out += m.esc(join(formatNames(r.editors, NameForm::Vancouver, 6), ", ") +
                         (r.editors.size() > 1 ? ", editors. " : ", editor. "));
        out += period(m.esc(r.container) + internet);
        if (!r.edition.empty())
            out += " " + m.esc(ordinal(r.edition) + " ed.");
        auto py = pubYear(false);
        if (!py.empty())
            out += " " + py;
        if (!r.pages.empty())
            out += " " + m.esc("p. " + pageRange(r.pages, PageFormat::Minimal) + ".");
    } else if (r.type == "thesis") {
        out += m.esc(r.title) + internet + " [" +
               m.esc(thesisLabel(r, "dissertation", "master's thesis", "thesis")) +
               "].";
        auto py = pubYear(false);
        if (!py.empty())
            out += " " + py;
    } else if (r.type == "preprint") {
        out += m.esc(r.title) + " [Preprint].";
        auto s = joinNonEmpty({r.publisher, trim(vancouverDate(r.issued, false) + cited)}, "; ");
        if (!s.empty())
            out += " " + period(m.esc(s));
    } else if (isWebLike(r)) {
        out += m.esc(r.title) + " [Internet].";
        auto s = joinNonEmpty(
            {r.container.empty() ? r.publisher : r.container, trim(vancouverDate(r.issued, true) + cited)}, "; ");
        if (!s.empty())
            out += " " + period(m.esc(s));
    } else {
        out += period(m.esc(r.title) + internet);
        if (!r.edition.empty())
            out += " " + m.esc(ordinal(r.edition) + " ed.");
        auto py = pubYear(false);
        if (!py.empty())
            out += " " + py;
        if (r.type == "report" && !r.number.empty())
            out += " " + m.esc("Report No.: " + r.number + ".");
    }
    if (!r.doi.empty())
        out += " doi:" + m.esc(bareDoi(r.doi));
    else if (!r.url.empty())
        out += " Available from: " + m.esc(r.url);
    return out;
}

// =================================================================================
// In-text citations
// =================================================================================

std::string family(const Person& p) { return p.family; }

std::string titleForCitation(const Ref& r, const Markup& m)
{
    if (isArticle(r) || isInContainer(r) || isWebLike(r))
        return "“" + m.esc(r.title) + "”";
    return m.ital(r.title);
}

// Author part of an author-date citation. `two` joins two names, `last` joins the last of
// three; `etAlFrom` is the number of authors from which "et al." is used.
std::string citeAuthors(const Ref& r, const Markup& m, const std::string& two, const std::string& sep,
                        const std::string& last, size_t etAlFrom)
{
    if (r.authors.empty())
        return titleForCitation(r, m);
    const auto& a = r.authors;
    if (a.size() >= etAlFrom)
        return m.esc(family(a[0]) + " et al.");
    std::vector<std::string> names;
    for (auto& p : a)
        names.push_back(family(p));
    return m.esc(joinList(names, sep, last, two));
}

std::string superscript(const std::string& s)
{
    static const char* const digits[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    std::string out;
    for (char c : s)
        out += c >= '0' && c <= '9' ? digits[c - '0'] : std::string(1, c);
    return out;
}

// Citation numbers 1..count; three or more consecutive numbers collapse to a range.
std::vector<std::string> numberRuns(size_t count, const std::string& open, const std::string& close)
{
    if (count >= 3)
        return {open + "1" + close + "–" + open + std::to_string(count) + close};
    std::vector<std::string> out;
    for (size_t k = 1; k <= count; ++k)
        out.push_back(open + std::to_string(k) + close);
    return out;
}

// =================================================================================
// Style table
// =================================================================================

using EntryFn = std::string (*)(const Ref&, const Markup&);

struct Style {
    StyleInfo info;
    EntryFn entry;
};

const std::vector<Style>& styles()
{
    static const std::vector<Style> s = {
        {{"apa", "APA 7th edition", false}, apaEntry},
        {{"chicago-author-date", "Chicago Manual of Style 17th edition (author-date)", false}, chicagoEntry},
        {{"ieee", "IEEE", true}, ieeeEntry},
        {{"mla", "MLA 9th edition", false}, mlaEntry},
        {{"harvard", "Harvard (Cite Them Right 12th edition)", false}, harvardEntry},
        {{"nature", "Nature", true}, natureEntry},
        {{"vancouver", "Vancouver", true}, vancouverEntry},
    };
    return s;
}

const Style& findStyle(const std::string& id)
{
    for (auto& s : styles())
        if (s.info.id == id)
            return s;
    throw Error("Unknown citation style: " + id);
}

std::vector<Ref> makeRefs(const std::vector<Json>& items)
{
    std::vector<Ref> refs;
    for (auto& item : items)
        if (isExportable(item))
            refs.push_back(makeRef(item));
    return refs;
}

std::string sortKey(const Ref& r)
{
    std::string who = r.authors.empty() ? r.title : r.authors[0].family + " " + r.authors[0].given;
    char yearBuf[8];
    std::snprintf(yearBuf, sizeof yearBuf, "%04d", r.issued.valid() ? r.issued.year : 0);
    return normalize(who) + '\x01' + yearBuf + '\x01' + normalize(r.title);
}

} // namespace

const std::vector<StyleInfo>& builtinStyles()
{
    static const std::vector<StyleInfo> infos = [] {
        std::vector<StyleInfo> v;
        for (auto& s : styles())
            v.push_back(s.info);
        return v;
    }();
    return infos;
}

std::string formatBibliography(const std::vector<Json>& items, const std::string& styleId, OutputFormat fmt)
{
    const auto& style = findStyle(styleId);
    Markup m(fmt);
    auto refs = makeRefs(items);
    if (!style.info.numeric)
        std::stable_sort(refs.begin(), refs.end(),
                         [](const Ref& a, const Ref& b) { return sortKey(a) < sortKey(b); });

    std::vector<std::string> entries;
    for (size_t i = 0; i < refs.size(); ++i) {
        auto entry = style.entry(refs[i], m);
        if (style.info.numeric)
            entry = (styleId == "ieee" ? "[" + std::to_string(i + 1) + "] " : std::to_string(i + 1) + ". ") + entry;
        entries.push_back(m.html() ? "  <div class=\"csl-entry\">" + entry + "</div>" : entry);
    }
    if (!m.html())
        return join(entries, "\n");
    return "<div class=\"csl-bib-body\">\n" + join(entries, "\n") + (entries.empty() ? "" : "\n") + "</div>";
}

std::string formatCitation(const std::vector<Json>& items, const std::string& styleId, OutputFormat fmt)
{
    const auto& style = findStyle(styleId);
    Markup m(fmt);
    auto refs = makeRefs(items);
    if (refs.empty())
        return {};

    if (style.info.numeric) {
        if (styleId == "ieee")
            return join(numberRuns(refs.size(), "[", "]"), ", ");
        if (styleId == "vancouver")
            return "(" + join(numberRuns(refs.size(), "", ""), ",") + ")";
        auto nums = join(numberRuns(refs.size(), "", ""), ",");
        return m.html() ? "<sup>" + nums + "</sup>" : superscript(nums);
    }

    std::vector<std::string> parts;
    for (auto& r : refs) {
        if (styleId == "apa")
            parts.push_back(citeAuthors(r, m, " & ", ", ", ", & ", 3) + ", " + year(r.issued));
        else if (styleId == "chicago-author-date")
            parts.push_back(citeAuthors(r, m, " and ", ", ", ", and ", 4) + " " + year(r.issued));
        else if (styleId == "mla")
            parts.push_back(citeAuthors(r, m, " and ", ", ", ", and ", 3));
        else  // harvard
            parts.push_back(citeAuthors(r, m, " and ", ", ", " and ", 4) + ", " + year(r.issued, "no date"));
    }
    return "(" + join(parts, "; ") + ")";
}

} // namespace on::formats
