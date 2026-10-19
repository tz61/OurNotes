#include "formats/ris.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#include "core/schema.h"
#include "core/util.h"
#include "formats/common.h"

namespace fp::formats {

namespace {

using schema::getField;

const std::map<std::string, std::string>& exportTypes()
{
    static const std::map<std::string, std::string> m = {
        {"journalArticle", "JOUR"}, {"conferencePaper", "CPAPER"}, {"book", "BOOK"},
        {"bookSection", "CHAP"},    {"thesis", "THES"},            {"report", "RPRT"},
        {"webpage", "ELEC"},        {"computerProgram", "COMP"},   {"preprint", "UNPB"},
        {"manuscript", "MANSCPT"},  {"magazineArticle", "MGZN"},   {"newspaperArticle", "NEWS"},
        {"patent", "PAT"},          {"dataset", "DATA"},           {"blogPost", "BLOG"},
        {"encyclopediaArticle", "ENCYC"}, {"dictionaryEntry", "DICT"}, {"map", "MAP"},
        {"film", "VIDEO"},          {"videoRecording", "VIDEO"},   {"audioRecording", "SOUND"},
        {"case", "CASE"},           {"statute", "STAT"},           {"bill", "BILL"},
        {"hearing", "HEAR"},        {"letter", "PCOMM"},           {"email", "ICOMM"},
        {"artwork", "ART"},         {"standard", "STAND"},         {"presentation", "SLIDE"},
        {"document", "GEN"}};
    return m;
}

const std::map<std::string, std::string>& importTypes()
{
    static const std::map<std::string, std::string> m = {
        {"JOUR", "journalArticle"},  {"JFULL", "journalArticle"}, {"EJOUR", "journalArticle"},
        {"ABST", "journalArticle"},  {"INPR", "journalArticle"},  {"CONF", "conferencePaper"},
        {"CPAPER", "conferencePaper"}, {"BOOK", "book"},          {"EBOOK", "book"},
        {"EDBOOK", "book"},          {"SER", "book"},             {"CTLG", "book"},
        {"CHAP", "bookSection"},     {"ECHAP", "bookSection"},    {"THES", "thesis"},
        {"RPRT", "report"},          {"GOVDOC", "report"},        {"ELEC", "webpage"},
        {"WEB", "webpage"},          {"COMP", "computerProgram"}, {"GEN", "document"},
        {"MANSCPT", "manuscript"},   {"MGZN", "magazineArticle"}, {"NEWS", "newspaperArticle"},
        {"PAT", "patent"},           {"DATA", "dataset"},         {"DBASE", "dataset"},
        {"BLOG", "blogPost"},        {"ENCYC", "encyclopediaArticle"}, {"DICT", "dictionaryEntry"},
        {"MAP", "map"},              {"VIDEO", "videoRecording"}, {"MPCT", "film"},
        {"SOUND", "audioRecording"}, {"MUSIC", "audioRecording"}, {"CASE", "case"},
        {"STAT", "statute"},         {"BILL", "bill"},            {"HEAR", "hearing"},
        {"PCOMM", "letter"},         {"ICOMM", "email"},          {"ART", "artwork"},
        {"STAND", "standard"},       {"SLIDE", "presentation"},   {"UNPB", "manuscript"}};
    return m;
}

// --- export -----------------------------------------------------------------------

void tagLine(std::string& out, const char* tag, const std::string& value)
{
    auto v = trim(replaceAll(value, "\r", ""));
    if (v.empty())
        return;
    out += tag;
    out += "  - ";
    out += replaceAll(v, "\n", "\r\n");
    out += "\r\n";
}

std::string risName(const Json& c)
{
    auto name = jstr(c, "name");
    if (!name.empty())
        return name;
    auto first = jstr(c, "firstName");
    return first.empty() ? jstr(c, "lastName") : jstr(c, "lastName") + ", " + first;
}

std::string risDate(const DateParts& d)
{
    if (!d.valid())
        return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d/", d.year);
    std::string out = buf;
    out += d.month ? (d.month < 10 ? "0" : "") + std::to_string(d.month) : "";
    out += "/";
    out += d.day ? (d.day < 10 ? "0" : "") + std::to_string(d.day) : "";
    out += "/";
    return out;
}

std::string itemToRis(const Json& item)
{
    auto type = jstr(item, "itemType");
    auto it = exportTypes().find(type);
    auto f = [&](const char* name) { return getField(item, name); };
    std::string out;
    tagLine(out, "TY", it == exportTypes().end() ? "GEN" : it->second);
    tagLine(out, "TI", f("title"));

    static const std::pair<const char*, const char*> roles[] = {
        {"editor", "A2"}, {"seriesEditor", "A3"}, {"translator", "A4"}};
    auto primary = schema::primaryCreatorType(type);
    for (auto& c : item.value("creators", Json::array()))
        if (jstr(c, "creatorType") == primary)
            tagLine(out, "AU", risName(c));
    for (auto& [role, tag] : roles)
        for (auto& c : item.value("creators", Json::array()))
            if (jstr(c, "creatorType") == role)
                tagLine(out, tag, risName(c));

    tagLine(out, "T2", f("publicationTitle"));
    tagLine(out, "J2", jstr(item, "journalAbbreviation"));
    tagLine(out, "T3", f("series").empty() ? f("seriesTitle") : f("series"));
    tagLine(out, "AB", jstr(item, "abstractNote"));
    tagLine(out, "CY", f("place"));
    auto d = parseDate(f("date"));
    tagLine(out, "DA", risDate(d));
    tagLine(out, "PY", d.valid() ? std::to_string(d.year) : yearOf(f("date")));
    tagLine(out, "DO", jstr(item, "DOI"));
    tagLine(out, "ET", jstr(item, "edition"));
    tagLine(out, "IS", jstr(item, "issue"));
    tagLine(out, "M1", f("number"));
    tagLine(out, "M3", f("type"));
    tagLine(out, "NV", jstr(item, "numberOfVolumes"));
    tagLine(out, "PB", f("publisher"));
    tagLine(out, "SN", jstr(item, "ISBN"));
    tagLine(out, "SN", jstr(item, "ISSN"));

    auto pages = replaceAll(replaceAll(f("pages"), "–", "-"), "—", "-");
    auto dash = pages.find('-');
    if (dash != std::string::npos && pages.find(',') == std::string::npos) {
        tagLine(out, "SP", pages.substr(0, dash));
        tagLine(out, "EP", pages.substr(pages.find_first_not_of('-', dash)));
    } else {
        tagLine(out, "SP", pages);
    }

    tagLine(out, "ST", jstr(item, "shortTitle"));
    tagLine(out, "VL", f("volume"));
    tagLine(out, "UR", jstr(item, "url"));
    tagLine(out, "Y2", risDate(parseDate(jstr(item, "accessDate"))));
    tagLine(out, "DP", jstr(item, "libraryCatalog"));
    tagLine(out, "CN", jstr(item, "callNumber"));
    tagLine(out, "LA", jstr(item, "language"));
    for (auto& t : item.value("tags", Json::array()))
        tagLine(out, "KW", jstr(t, "tag"));
    for (auto& n : item.value("_notes", Json::array()))
        if (n.is_string())
            tagLine(out, "N1", replaceAll(n.get<std::string>(), "\n", ""));
    for (auto& a : item.value("_attachments", Json::array())) {
        auto path = jstr(a, "path");
        if (!path.empty())
            tagLine(out, startsWith(jstr(a, "contentType"), "image/") ? "L4" : "L1", path);
    }
    out += "ER  - \r\n";
    return out;
}

// --- import -----------------------------------------------------------------------

struct Record {
    std::string type;
    std::vector<std::pair<std::string, std::string>> fields;
};

bool parseTagLine(const std::string& line, std::string& tag, std::string& value)
{
    if (line.size() < 5)
        return false;
    auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    if (!upper(line[0]) || !(upper(line[1]) || (line[1] >= '0' && line[1] <= '9')))
        return false;
    size_t i = 2;
    while (i < line.size() && line[i] == ' ')
        ++i;
    if (i == 2 || i > 4 || i >= line.size() || line[i] != '-')
        return false;
    tag = line.substr(0, 2);
    value = trim(line.substr(i + 1));
    return true;
}

std::string pickType(const Record& r)
{
    auto it = importTypes().find(r.type);
    auto type = it == importTypes().end() ? std::string("document") : it->second;
    if (r.type != "UNPB")
        return type;
    // RIS has no preprint type; recognise the usual preprint servers
    for (auto& [tag, value] : r.fields) {
        auto v = toLower(value);
        if ((tag == "M3" && v.find("preprint") != std::string::npos) ||
            ((tag == "PB" || tag == "DP" || tag == "DB") &&
             (v.find("arxiv") != std::string::npos || v.find("biorxiv") != std::string::npos ||
              v.find("medrxiv") != std::string::npos || v.find("ssrn") != std::string::npos ||
              v.find("preprint") != std::string::npos)) ||
            (tag == "M1" && startsWith(v, "arxiv:")) || (tag == "DO" && startsWith(cleanDoi(v), "10.48550/")))
            return "preprint";
    }
    return type;
}

int precision(const DateParts& d) { return d.valid() + (d.month != 0) + (d.day != 0); }

// RIS names are "Last, First"; a name without a comma is an organisation (as in Zotero).
Json risCreator(const std::string& value)
{
    return value.find(',') == std::string::npos ? literalName(value) : splitName(value);
}

Json recordToItem(const Record& r)
{
    auto type = pickType(r);
    Json item = newItem(type);
    std::vector<std::string> notes, files, dates;
    std::string startPage, endPage, journalAbbr;
    bool bookLike = type == "book";
    for (auto& [tag, value] : r.fields) {
        if (value.empty())
            continue;
        if (tag == "TI" || tag == "T1" || (tag == "BT" && bookLike)) {
            if (jstr(item, "title").empty())
                setField(item, "title", value);
        } else if (tag == "AU" || tag == "A1") {
            addCreator(item, "author", risCreator(value));
        } else if (tag == "A2" || tag == "ED") {
            addCreator(item, "editor", risCreator(value));
        } else if (tag == "A3") {
            addCreator(item, "seriesEditor", risCreator(value));
        } else if (tag == "A4") {
            addCreator(item, "translator", risCreator(value));
        } else if (tag == "T2" || tag == "JF" || tag == "BT" || tag == "JO") {
            if (getField(item, "publicationTitle").empty())
                setField(item, "publicationTitle", value);
            else if (tag == "JO")
                journalAbbr = value;
        } else if (tag == "JA" || tag == "J2") {
            journalAbbr = value;
        } else if (tag == "T3") {
            if (!setField(item, "series", value))
                setField(item, "seriesTitle", value);
        } else if (tag == "PY" || tag == "Y1" || tag == "DA") {
            dates.push_back(value);
        } else if (tag == "VL") {
            setField(item, "volume", value);
        } else if (tag == "IS" || tag == "CP") {
            setField(item, "issue", value);
        } else if (tag == "SP") {
            startPage = value;
        } else if (tag == "EP") {
            endPage = value;
        } else if (tag == "DO") {
            auto doi = cleanDoi(value);
            setField(item, "DOI", doi.empty() ? value : doi);
        } else if (tag == "UR" || tag == "L2") {
            if (jstr(item, "url").empty())
                setField(item, "url", value);
        } else if (tag == "AB" || tag == "N2") {
            if (jstr(item, "abstractNote").empty())
                setField(item, "abstractNote", value);
        } else if (tag == "KW") {
            addTag(item, value);
        } else if (tag == "SN") {
            std::string digits;
            for (char c : value)
                if (std::isdigit((unsigned char)c) || c == 'X' || c == 'x')
                    digits += c;
            bool issn = digits.size() == 8;
            if (issn || !setField(item, "ISBN", value))
                setField(item, "ISSN", value);
        } else if (tag == "PB") {
            setField(item, "publisher", value);
        } else if (tag == "CY" || tag == "PP") {
            setField(item, "place", value);
        } else if (tag == "LA") {
            setField(item, "language", value);
        } else if (tag == "ET") {
            setField(item, "edition", value);
        } else if (tag == "NV") {
            setField(item, "numberOfVolumes", value);
        } else if (tag == "M1") {
            if (!setField(item, "number", value))
                setField(item, "issue", value);
        } else if (tag == "M3") {
            setField(item, "type", value);
        } else if (tag == "Y2") {
            auto d = parseDate(value);
            setField(item, "accessDate", d.valid() ? isoDate(d) : value);
        } else if (tag == "DP" || tag == "DB") {
            setField(item, "libraryCatalog", value);
        } else if (tag == "ST") {
            setField(item, "shortTitle", value);
        } else if (tag == "CN") {
            setField(item, "callNumber", value);
        } else if (tag == "N1") {
            notes.push_back(startsWith(trim(value), "<") ? value : textToNoteHtml(value));
        } else if (tag == "L1" || tag == "L4") {
            auto path = value;
            if (startsWith(path, "file://"))
                path = urlDecode(path.substr(7));
            if (!startsWith(path, "http://") && !startsWith(path, "https://") && !startsWith(path, "internal-pdf:"))
                files.push_back(path);
        }
    }

    // keep the most precise of PY/Y1/DA
    DateParts best;
    std::string bestRaw;
    for (auto& raw : dates) {
        auto d = parseDate(raw);
        if (bestRaw.empty() || precision(d) > precision(best)) {
            best = d;
            bestRaw = raw;
        }
    }
    if (!dates.empty())
        setField(item, "date", best.valid() ? isoDate(best) : bestRaw);

    if (!startPage.empty())
        setField(item, "pages", endPage.empty() || startPage.find('-') != std::string::npos
                                    ? startPage
                                    : startPage + "-" + endPage);
    setField(item, "journalAbbreviation", journalAbbr);
    if (!notes.empty())
        item["_notes"] = notes;
    if (!files.empty())
        item["_files"] = files;
    return finishItem(item);
}

} // namespace

std::string exportRis(const std::vector<Json>& items)
{
    std::string out;
    for (auto& item : items) {
        if (!isExportable(item))
            continue;
        if (!out.empty())
            out += "\r\n";
        out += itemToRis(item);
    }
    return out;
}

std::vector<Json> importRis(const std::string& text)
{
    std::vector<Json> out;
    std::vector<Record> records;
    bool open = false;
    auto lines = split(text, '\n', false);
    for (size_t n = 0; n < lines.size(); ++n) {
        auto line = lines[n];
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (n == 0 && startsWith(line, "\xEF\xBB\xBF"))
            line = line.substr(3);
        std::string tag, value;
        if (parseTagLine(line, tag, value)) {
            if (tag == "TY") {
                std::transform(value.begin(), value.end(), value.begin(),
                               [](char c) { return (char)std::toupper((unsigned char)c); });
                records.push_back(Record{value, {}});
                open = true;
            } else if (tag == "ER") {
                open = false;
            } else if (open) {
                records.back().fields.emplace_back(tag, value);
            }
        } else if (open && !records.back().fields.empty() && !trim(line).empty()) {
            auto& v = records.back().fields.back().second;
            v += (v.empty() ? "" : "\n") + trim(line);
        }
    }
    for (auto& r : records)
        out.push_back(recordToItem(r));
    return out;
}

} // namespace fp::formats
