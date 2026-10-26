// ournotes-cli commands for a PDF's annotations, as made in the app's reader
// (docs/agents.md): a passage highlighted or underlined, a note, a text box, an area or a
// drawing put on a page; an annotation's comment, colour, text and tags changed; a note
// made from them; a copy of the PDF with them in it; where the PDF is opened (page, zoom).

#include <cctype>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <iostream>

#include "cli/commands.h"
#include "core/links.h"
#include "core/schema.h"
#include "pdf/document.h"

using namespace on;

namespace {

constexpr double kNoteSize = 22;  // points, as in the app
double r3(double v) { return std::round(v * 1000) / 1000; }

// The PDF given, or the item's.
std::pair<int64_t, Json> pdfAttachment(Library& lib, const Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (schema::isRegularType(jstr(item, "itemType"))) {
        auto best = lib.bestAttachment(id, jstr(item, "key"));
        if (!best || jstr(*best, "contentType") != "application/pdf")
            throw Error(jstr(item, "key") + " has no PDF");
        item = *best;
    }
    if (jstr(item, "itemType") != "attachment" || jstr(item, "contentType") != "application/pdf")
        throw Error(jstr(item, "key") + " is no PDF (give a PDF attachment, or the item it is under)");
    return {id, item};
}

std::unique_ptr<pdf::Document> openPdf(Library& lib, int64_t id, const Json& att)
{
    requireFile(lib, id, att);
    return pdf::Document::open(lib.attachmentFile(id, att));
}

// "x,y" or "x0,y0,x1,y1": numbers, in points from the page's top left
std::vector<float> numbers(const std::string& spec, size_t count, const std::string& what)
{
    std::vector<float> out;
    try {
        for (auto& part : split(spec, ',')) {
            out.push_back(std::stof(trim(part)));
            if (!std::isfinite(out.back()))  // (nan, inf)
                throw Error("");
        }
    } catch (const std::exception&) {
        out.clear();
    }
    if (out.size() != count)
        throw Error("give " + what);
    return out;
}

int pageOption(const Args& args, pdf::Document& doc)
{
    auto v = args.option("--page");
    if (v.empty())
        return -1;
    int page = 0;
    try {
        page = std::stoi(v);
    } catch (const std::exception&) {
    }
    if (page < 1 || page > doc.pageCount())
        throw Error("no page " + v + ": the PDF has " + std::to_string(doc.pageCount()));
    return page - 1;
}

struct Passage {
    int page = 0, from = 0, to = 0;  // chars [from, to] of the page's text
    int nextFrom = -1, nextTo = -1;  // and [nextFrom, nextTo] of the next page's: across the break
};

// Text as compared: in lower case, every run of spaces (and line breaks) one space.
std::u32string folded(const std::string& utf8)
{
    std::u32string out;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = utf8[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
        for (int k = 1; k < n && i + k < utf8.size(); ++k)
            cp = (cp << 6) | (utf8[i + k] & 0x3f);
        i += n;
        bool space = cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == 0xa0;
        if (space && (out.empty() || out.back() == U' '))
            continue;
        out += space ? U' ' : cp < 0x80 ? char32_t(std::tolower(int(cp))) : char32_t(std::towlower(wint_t(cp)));
    }
    while (!out.empty() && out.back() == U' ')
        out.pop_back();
    return out;
}

// A page's text as read, folded likewise (a word broken at a line's end whole), each letter's
// character on the page (-1: the space between two lines).
struct Reading {
    std::u32string text;
    std::vector<int> at;
};

Reading reading(const pdf::PageText& page)
{
    Reading r;
    for (int i = 0; i < (int)page.chars.size(); ++i) {
        char32_t c = page.chars[i].c;
        if (i > 0 && page.chars[i].line != page.chars[i - 1].line) {
            if (!r.text.empty() && r.text.back() == U'-' && c < 0x80 && std::islower(int(c))) {
                r.text.pop_back();  // (hyphen-ated)
                r.at.pop_back();
            } else if (!r.text.empty() && r.text.back() != U' ') {
                r.text += U' ';
                r.at.push_back(-1);
            }
        }
        bool space = c == U' ' || c == U'\t' || c == 0xa0;
        if (space && (r.text.empty() || r.text.back() == U' '))
            continue;
        r.text += space ? U' ' : c < 0x80 ? char32_t(std::tolower(int(c))) : char32_t(std::towlower(wint_t(c)));
        r.at.push_back(i);
    }
    return r;
}

// The characters a stretch of a reading covers.
std::pair<int, int> charsOf(const Reading& r, size_t from, size_t length)
{
    int first = -1, last = -1;
    for (size_t k = from; k < from + length; ++k)
        if (r.at[k] >= 0) {
            first = first < 0 ? r.at[k] : first;
            last = r.at[k];
        }
    return {first, last};
}

// A quote across a page break: its first words at the end of `page` (the last place there),
// the rest at the start of the next (the first), what lies between them (a page number, a
// running head) left out.
std::optional<Passage> passageAcross(pdf::Document& doc, const std::u32string& quote, int page)
{
    if (page + 1 >= doc.pageCount())
        return std::nullopt;
    auto before = reading(doc.text(page)), after = reading(doc.text(page + 1));
    for (size_t k = quote.size(); k-- > 1;) {  // (the longest beginning first)
        if (quote[k] != U' ' || k + 1 >= quote.size())
            continue;
        auto head = quote.substr(0, k), tail = quote.substr(k + 1);
        auto i = before.text.rfind(head);
        auto j = after.text.find(tail);
        if (i == std::u32string::npos || j == std::u32string::npos)
            continue;
        auto [from, to] = charsOf(before, i, head.size());
        auto [nextFrom, nextTo] = charsOf(after, j, tail.size());
        if (from >= 0 && nextFrom >= 0)
            return Passage{page, from, to, nextFrom, nextTo};
    }
    return std::nullopt;
}

// The first passage that reads `quote` (in any case), on `page` or else anywhere: the
// characters inside the places the search found (on lines of any direction); else one that
// begins on `page` (or any) and goes on at the top of the next (see passageAcross).
std::optional<Passage> findPassage(pdf::Document& doc, const std::string& quote, int page)
{
    int first = page >= 0 ? page : 0, last = page >= 0 ? page : doc.pageCount() - 1;
    for (int p = first; p <= last; ++p) {
        auto hits = doc.search(p, quote);
        if (hits.empty() || hits[0].empty())
            continue;
        auto& text = doc.text(p);
        int from = -1, to = -1;
        for (int i = 0; i < (int)text.chars.size(); ++i) {
            auto& box = text.chars[i].box;
            pdf::Point centre{(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2};
            if (std::any_of(hits[0].begin(), hits[0].end(), [&](const pdf::Rect& r) { return r.contains(centre); })) {
                from = from < 0 ? i : from;
                to = i;
            }
        }
        if (from >= 0)
            return Passage{p, from, to};
    }
    // across a page break (from `page` on into the next)
    auto wanted = folded(quote);
    for (int p = first; p <= std::min(last, doc.pageCount() - 2); ++p)
        if (auto found = passageAcross(doc, wanted, p))
            return found;
    return std::nullopt;
}

Json positionFromRects(pdf::Document& doc, int page, const std::vector<pdf::Rect>& rects)
{
    Json arr = Json::array();
    for (auto& r : rects) {
        auto pr = doc.pageToPdf(page, r);
        arr.push_back({r3(pr.x0), r3(pr.y0), r3(pr.x1), r3(pr.y1)});
    }
    return Json{{"pageIndex", page}, {"rects", arr}};
}

void requireOnPage(pdf::Document& doc, int page, const pdf::Rect& r)
{
    auto size = doc.pageSize(page);
    if (r.x0 < 0 || r.y0 < 0 || r.x1 > size.width || r.y1 > size.height)
        throw Error("that is off the page: it is " + std::to_string((int)size.width) + " by " +
                    std::to_string((int)size.height) + " points");
}

Json tagList(const std::string& spec)
{
    Json tags = Json::array();
    for (auto& t : commaList(spec))
        tags.push_back(Json{{"tag", t}});
    return tags;
}

int cmdAnnotate(Library& lib, Args& args, Config& config)
{
    requirePositionals(args, 1);
    auto [id, att] = pdfAttachment(lib, args);
    requireEditable(lib, id);
    auto doc = openPdf(lib, id, att);
    auto quote = args.option("--quote");
    auto type = args.option("--type", quote.empty() ? "" : "highlight");
    static const std::vector<std::string> types = {"highlight", "underline", "note", "text", "image", "ink"};
    if (std::find(types.begin(), types.end(), type) == types.end())
        throw Error("say which: --type highlight, underline (with --quote), note, text, image or ink");
    int page = pageOption(args, *doc);
    auto color = args.option("--color");
    color = color.empty() ? config.get("/reader/color", "#ffd400").get<std::string>() : annotationColorValue(color);
    Json data{{"key", newKey()},
              {"itemType", "annotation"},
              {"parentItem", jstr(att, "key")},
              {"annotationType", type},
              {"annotationText", ""},
              {"annotationComment", args.option("--comment")},
              {"annotationColor", color},
              {"tags", tagList(args.option("--tags"))}};
    int p = std::max(page, 0), offset = 0;
    double top = 0;
    auto passage = [&] {
        auto found = findPassage(*doc, quote, page);
        if (!found)
            throw Error("no \"" + quote + "\" " + (page >= 0 ? "on page " + std::to_string(page + 1) : "in the PDF"));
        return *found;
    };
    if (type == "highlight" || type == "underline") {
        if (quote.empty())
            throw Error("give the passage to " + type + ": --quote TEXT");
        auto found = passage();
        p = found.page;
        auto& text = doc->text(p);
        auto rects = text.rectsOf(found.from, found.to);
        top = rects.front().y0;
        for (auto& r : rects)
            top = std::min<double>(top, r.y0);
        offset = found.from;
        data["annotationText"] = text.textOf(found.from, found.to);
        data["annotationPosition"] = positionFromRects(*doc, p, rects);
        if (found.nextFrom >= 0) {  // across two pages: one annotation, the rest on the next (as Zotero has it)
            auto& next = doc->text(p + 1);
            data["annotationPosition"]["nextPageRects"] =
                positionFromRects(*doc, p + 1, next.rectsOf(found.nextFrom, found.nextTo))["rects"];
            data["annotationText"] = jstr(data, "annotationText") + " " + next.textOf(found.nextFrom, found.nextTo);
        }
    } else if (type == "note" || type == "text") {
        // where: --at X,Y; or by the passage --quote reads (a note left of it, a text box under it)
        pdf::Point at{20, 20};
        if (auto spec = args.option("--at"); !spec.empty()) {
            auto xy = numbers(spec, 2, "--at X,Y (points from the page's top left)");
            at = {xy[0], xy[1]};
        } else if (!quote.empty()) {
            auto found = passage();
            p = found.page;
            auto first = doc->text(p).rectsOf(found.from, found.to).front();
            at = type == "note" ? pdf::Point{std::max<float>((float)kNoteSize / 2, first.x0 - (float)kNoteSize / 2 - 2),
                                             (first.y0 + first.y1) / 2}
                                : pdf::Point{first.x0, first.y1 + 2};
        }
        pdf::Rect r;
        if (type == "note") {
            float half = (float)kNoteSize / 2;
            r = {at.x - half, at.y - half, at.x + half, at.y + half};
        } else {
            if (args.option("--comment").empty())
                throw Error("give the text of the box: --comment TEXT");
            double fontSize = numberOption(args, "--font-size", 14, 4, 200);
            requireOnPage(*doc, p, {at.x, at.y, at.x, at.y});
            r = {at.x, at.y, at.x + 220, (float)(at.y + fontSize * 1.6)};
            auto size = doc->pageSize(p);
            if (r.x1 > size.width)  // as much of the width as there is
                r.x1 = size.width;
            if (r.width() < 20)
                throw Error("there is no room for a text box there: it starts " + std::to_string((int)size.width - (int)at.x) +
                            " points before the page's right edge");
            requireOnPage(*doc, p, r);
            auto pos = positionFromRects(*doc, p, {r});
            pos["fontSize"] = fontSize;
            pos["rotation"] = 0;
            data["annotationPosition"] = pos;
        }
        requireOnPage(*doc, p, r);
        if (type == "note")
            data["annotationPosition"] = positionFromRects(*doc, p, {r});
        offset = std::max(0, doc->text(p).hitTest(at, true));
        top = r.y0;
    } else if (type == "image") {
        auto v = numbers(args.option("--rect"), 4, "the area: --rect X0,Y0,X1,Y1 (points from the page's top left)");
        pdf::Rect r = pdf::Rect{v[0], v[1], v[2], v[3]}.normalized();
        if (r.width() <= 3 || r.height() <= 3)
            throw Error("the area is too small");
        requireOnPage(*doc, p, r);
        data["annotationPosition"] = positionFromRects(*doc, p, {r});
        top = r.y0;
    } else {  // ink: strokes "x,y x,y ...", apart by ;
        Json paths = Json::array();
        top = 1e9;
        for (auto& stroke : split(args.option("--ink"), ';')) {
            Json path = Json::array();
            int points = 0;
            for (auto& xy : split(trim(stroke), ' ')) {
                auto v = numbers(xy, 2, "the strokes: --ink \"X,Y X,Y ...;X,Y ...\" (points from the page's top left)");
                requireOnPage(*doc, p, {v[0], v[1], v[0], v[1]});
                auto pp = doc->pageToPdf(p, pdf::Point{v[0], v[1]});
                path.push_back(r3(pp.x));
                path.push_back(r3(pp.y));
                top = std::min<double>(top, v[1]);
                ++points;
            }
            if (points < 2)
                throw Error("a stroke has two points or more: --ink \"X,Y X,Y ...\"");
            paths.push_back(path);
        }
        if (paths.empty())
            throw Error("give the strokes: --ink \"X,Y X,Y ...;X,Y ...\" (points from the page's top left)");
        data["annotationPosition"] = Json{{"pageIndex", p}, {"width", 2}, {"paths", paths}};
    }
    data["annotationPageLabel"] = doc->pageLabel(p);
    data["annotationSortIndex"] = pdf::sortIndex(p, offset, (float)top);
    std::cout << jstr(lib.createItem(id, data), "key") << "\n";
    return 0;
}

Json annotationJson(const Json& a)
{
    Json tags = Json::array();
    for (auto& t : a.value("tags", Json::array()))
        tags.push_back(jstr(t, "tag"));
    return Json{{"key", jstr(a, "key")},
                {"attachment", jstr(a, "parentItem")},
                {"type", jstr(a, "annotationType")},
                {"page", jstr(a, "annotationPageLabel")},
                {"text", jstr(a, "annotationText")},
                {"comment", jstr(a, "annotationComment")},
                {"color", jstr(a, "annotationColor")},
                {"tags", tags},
                {"position", a.value("annotationPosition", Json::object())},
                {"external", jbool(a, "annotationIsExternal")}};
}

int cmdAnnotation(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto [id, a] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (jstr(a, "itemType") != "annotation")
        throw Error(jstr(a, "key") + " is no annotation (see: ournotes-cli annotations ITEM)");
    auto given = [&](const char* name) { return args.options.count(name) > 0; };
    bool moved = given("--rect") || given("--at") || given("--font-size");
    if (!given("--comment") && !given("--color") && !given("--quote") && !given("--tags") && !moved) {
        std::cout << annotationJson(a).dump(2) << "\n";
        return 0;
    }
    if (jbool(a, "annotationIsExternal"))
        throw Error(jstr(a, "key") + " is an annotation in the PDF file itself: it cannot be changed here");
    requireEditable(lib, id);
    Json data = a;
    if (given("--comment"))
        data["annotationComment"] = args.option("--comment");
    if (given("--color"))
        data["annotationColor"] = annotationColorValue(args.option("--color"));
    if (given("--quote")) {
        auto type = jstr(a, "annotationType");
        if (type != "highlight" && type != "underline")
            throw Error("a " + type + " has no text of the PDF's: give its --comment");
        data["annotationText"] = args.option("--quote");
    }
    if (given("--tags"))
        data["tags"] = tagList(args.option("--tags"));
    if (moved) {  // as dragged or resized in the app, on its page
        auto type = jstr(a, "annotationType");
        auto& pos = data["annotationPosition"];
        int page = pos.value("pageIndex", 0);
        auto att = lib.item(id, jstr(a, "parentItem"));
        if (!att)
            throw Error("the PDF of " + jstr(a, "key") + " is not in the library");
        auto doc = openPdf(lib, id, *att);
        if (page < 0 || page >= doc->pageCount())
            throw Error(jstr(a, "key") + " is on no page of its PDF");
        if (given("--font-size")) {
            if (type != "text")
                throw Error("a " + type + " has no text size: a text box has");
            pos["fontSize"] = numberOption(args, "--font-size", 14, 4, 200);
        }
        if (given("--rect") || given("--at")) {
            pdf::Rect r;
            if (given("--at")) {
                if (type != "note")
                    throw Error("--at places a note; a " + type + " takes --rect X0,Y0,X1,Y1");
                auto xy = numbers(args.option("--at"), 2, "--at X,Y (points from the page's top left)");
                float half = (float)kNoteSize / 2;
                r = {xy[0] - half, xy[1] - half, xy[0] + half, xy[1] + half};
            } else {
                if (type != "image" && type != "text")
                    throw Error("a " + type + " is not resized: an area or a text box is");
                auto v = numbers(args.option("--rect"), 4, "--rect X0,Y0,X1,Y1 (points from the page's top left)");
                r = pdf::Rect{v[0], v[1], v[2], v[3]}.normalized();
                if (r.width() <= 3 || r.height() <= 3)
                    throw Error("that is too small");
            }
            requireOnPage(*doc, page, r);
            pos["rects"] = positionFromRects(*doc, page, {r})["rects"];
            data["annotationSortIndex"] = pdf::sortIndex(page, 0, r.y0);  // as the app sorts one it moved
        }
    }
    lib.saveItem(id, data);
    std::cout << jstr(a, "key") << "\n";
    return 0;
}

std::string today()
{
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

// "(Author, Year, p. N)" for an annotation of the item `parent`'s PDF, as the reader cites
// As the reader's Add to Note: the annotation's text in quotes and where it is from (a link to
// it), then its comment.
std::string annotationHtml(Library& lib, int64_t id, const Json& a)
{
    auto ref = referenceTo(lib, id, jstr(a, "key"));
    auto text = jstr(a, "annotationText"), comment = jstr(a, "annotationComment");
    std::string html;
    if (!text.empty())
        html += "<p>" + referenceHtml(ref, text) + "</p>";
    if (!comment.empty())
        html += "<p>" + htmlEscape(comment) + (text.empty() ? " " + referenceHtml(ref) : std::string()) + "</p>";
    return html;
}

// As the app's "Add Item Note from Annotations": each highlight's text in quotes, then
// (Author, Year, p. N) linked to it and its comment; each other annotation's comment.
std::string annotationsHtml(Library& lib, int64_t id, const std::vector<Json>& pdfs)
{
    std::string html = "<h1>Annotations<br>(" + today() + ")</h1>";
    for (auto& pdf : pdfs)
        for (auto& a : lib.annotations(id, jstr(pdf, "key"))) {
            auto type = jstr(a, "annotationType"), text = jstr(a, "annotationText"), comment = jstr(a, "annotationComment");
            auto ref = referenceTo(lib, id, jstr(a, "key"));
            if (type == "highlight" || type == "underline") {
                html += "<p>" + referenceHtml(ref, text);
                if (!comment.empty())
                    html += " " + htmlEscape(comment);
                html += "</p>";
            } else if (!comment.empty()) {
                html += "<p>" + htmlEscape(comment) + " " + referenceHtml(ref) + "</p>";
            }
        }
    return html;
}

int cmdNoteFromAnnotations(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    std::vector<Json> pdfs;
    std::string parent, single;
    if (jstr(item, "itemType") == "annotation") {  // that one, as the reader's Add to Note
        auto pdf = lib.item(id, jstr(item, "parentItem"));
        if (!pdf)
            throw Error("the PDF of " + jstr(item, "key") + " is not in the library");
        parent = jstr(*pdf, "parentItem");
        single = jstr(item, "key");
    } else if (jstr(item, "itemType") == "attachment") {
        pdfs.push_back(item);
        parent = jstr(item, "parentItem");
    } else if (schema::isRegularType(jstr(item, "itemType"))) {
        parent = jstr(item, "key");
        for (auto& c : lib.children(id, parent, "attachment"))
            if (!jbool(c, "deleted") && jstr(c, "contentType") == "application/pdf")
                pdfs.push_back(c);
    } else {
        throw Error(jstr(item, "key") + " is a " + jstr(item, "itemType") + ": give a PDF, the item it is under, or an annotation");
    }
    requireEditable(lib, id);
    auto html = single.empty() ? annotationsHtml(lib, id, pdfs) : annotationHtml(lib, id, item);
    if (auto to = args.option("--to"); !to.empty()) {  // added to that note
        auto [nid, note] = findItem(lib, to, id);
        if (jstr(note, "itemType") != "note")
            throw Error(to + " is not a note");
        lib.updateItem(id, to, [&](Json& d) {
            auto old = jstr(d, "note");
            if (startsWith(old, "<div data-schema-version")) {
                old = old.substr(old.find('>') + 1);
                if (auto end = old.rfind("</div>"); end != std::string::npos)
                    old.resize(end);
            }
            d["note"] = "<div data-schema-version=\"9\">" + old + html + "</div>";
        });
        std::cout << to << "\n";
        return 0;
    }
    std::cout << jstr(lib.createNote(id, "<div data-schema-version=\"9\">" + html + "</div>", parent), "key") << "\n";
    return 0;
}

int cmdExportPdf(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto [id, att] = pdfAttachment(lib, args);
    auto doc = openPdf(lib, id, att);
    auto source = lib.attachmentFile(id, att);
    auto out = args.option("-o");
    auto dest = out.empty() ? fs::current_path() / u8path(pathString(source.stem()) + " (annotated).pdf") : u8path(out);
    std::error_code ec;
    if (fs::exists(dest, ec) && fs::equivalent(dest, source, ec))
        throw Error("that is the PDF itself: give another file with -o");
    if (out.empty() && fs::exists(dest, ec) && !args.flag("--force"))  // (one named with -o is replaced)
        throw Error(pathString(dest) + " is there already: add --force to replace it, or give another with -o");
    doc->exportWithAnnotations(dest, lib.annotations(id, jstr(att, "key")));
    std::cout << pathString(dest) << "\n";
    return 0;
}

// Where an annotation is on its page, in page space: its rects, or its strokes as wide as
// they are drawn.
pdf::Rect boundsOf(pdf::Document& doc, int page, const Json& pos)
{
    pdf::Rect bounds;
    auto add = [&](const pdf::Rect& r) { bounds = bounds.isEmpty() ? r : bounds.united(r); };
    for (auto& r : pos.value("rects", Json::array()))
        if (r.is_array() && r.size() == 4 && r[0].is_number() && r[1].is_number() && r[2].is_number() && r[3].is_number())
            add(doc.pdfToPage(page, pdf::Rect{r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()})
                    .normalized());
    float half = pos.value("width", 2.0f) / 2 + 1;
    for (auto& path : pos.value("paths", Json::array()))
        for (size_t i = 0; i + 1 < path.size(); i += 2)
            if (path[i].is_number() && path[i + 1].is_number()) {
                auto p = doc.pdfToPage(page, pdf::Point{path[i].get<float>(), path[i + 1].get<float>()});
                add({p.x - half, p.y - half, p.x + half, p.y + half});
            }
    return bounds;
}

// A page with its annotations, or the place of one annotation on it (and the page under it,
// or not), as a PNG picture.
int cmdRender(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    std::optional<Json> annotation;
    Json att = item;
    if (jstr(item, "itemType") == "annotation") {
        annotation = item;
        auto pdf = lib.item(id, jstr(item, "parentItem"));
        if (!pdf)
            throw Error("the PDF of " + jstr(item, "key") + " is not in the library");
        att = *pdf;
    } else {
        att = pdfAttachment(lib, args).second;
    }
    auto doc = openPdf(lib, id, att);
    float scale = (float)numberOption(args, "--dpi", 150, 30, 600) / 72;
    bool withPage = !args.flag("--no-page");
    auto drawn = lib.annotations(id, jstr(att, "key"));
    int page = 0;
    pdf::Rect clip;  // empty: the whole page
    if (annotation) {
        if (!args.option("--page").empty() || !args.option("--rect").empty())
            throw Error("an annotation is drawn where it is: --page and --rect are for a page");
        auto pos = annotation->value("annotationPosition", Json::object());
        page = pos.value("pageIndex", -1);
        if (page < 0 || page >= doc->pageCount())
            throw Error(jstr(item, "key") + " is on no page of its PDF");
        auto bounds = boundsOf(*doc, page, pos);
        if (bounds.isEmpty())
            throw Error(jstr(item, "key") + " has no place on its page");
        auto margin = (float)numberOption(args, "--margin", 18, 0, 1000);
        auto size = doc->pageSize(page);
        clip = {std::max(0.f, bounds.x0 - margin), std::max(0.f, bounds.y0 - margin),
                std::min(size.width, bounds.x1 + margin), std::min(size.height, bounds.y1 + margin)};
        if (!withPage)
            drawn = {*annotation};  // that one alone
    } else {
        page = std::max(0, pageOption(args, *doc));
        if (auto spec = args.option("--rect"); !spec.empty()) {
            auto v = numbers(spec, 4, "--rect X0,Y0,X1,Y1 (points from the page's top left)");
            clip = pdf::Rect{v[0], v[1], v[2], v[3]}.normalized();
            if (clip.width() < 1 || clip.height() < 1)
                throw Error("that is too small");
            requireOnPage(*doc, page, clip);
        }
    }
    auto size = clip.isEmpty() ? doc->pageSize(page) : pdf::Size{clip.width(), clip.height()};
    if (size.width * scale > 12000 || size.height * scale > 12000)
        throw Error("that is too large a picture at that --dpi");
    auto out = args.option("-o");
    auto name = pathString(lib.attachmentFile(id, att).stem()) +
                (annotation ? " " + jstr(item, "key") : " p" + std::to_string(page + 1)) + ".png";
    auto dest = out.empty() ? fs::current_path() / u8path(name) : u8path(out);
    std::error_code ec;
    if (out.empty() && fs::exists(dest, ec) && !args.flag("--force"))
        throw Error(pathString(dest) + " is there already: add --force to replace it, or give another with -o");
    doc->renderPng(page, scale, clip, drawn, withPage, dest);
    std::cout << pathString(fs::absolute(dest, ec)) << "\n";
    return 0;
}

// Zotero's view state: "scale" is a percentage, or "page-width" or "page-fit".
std::string zoomText(const Json& scale)
{
    if (scale.is_number())
        return std::to_string((int)std::lround(scale.get<double>())) + "%";
    return scale.is_string() ? scale.get<std::string>() : std::string("as the app opens PDFs");
}

int cmdViewState(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto [id, att] = pdfAttachment(lib, args);
    auto key = jstr(att, "key");
    auto state = lib.readerState(key);
    auto lastPage = lib.setting(id, "lastPageIndex_" + key);
    auto pageSpec = args.option("--page"), zoom = args.option("--zoom");
    if (pageSpec.empty() && zoom.empty()) {
        // as the reader opens it: the page read last on any computer wins
        Json page = state.value("pageIndex", Json());
        if (lastPage.is_number_integer())
            page = lastPage;
        Json scale = state.value("scale", Json());
        if (args.flag("--json")) {
            std::cout << Json{{"key", key},
                              {"page", page.is_number_integer() ? Json(page.get<int>() + 1) : Json()},
                              {"zoom", scale}}
                             .dump(2)
                      << "\n";
            return 0;
        }
        std::cout << "page " << (page.is_number_integer() ? std::to_string(page.get<int>() + 1) : std::string("1"))
                  << "\nzoom " << zoomText(scale) << "\n";
        return 0;
    }
    Json next = state;
    if (!zoom.empty()) {
        if (zoom == "page-width" || zoom == "page-fit") {
            next["scale"] = zoom;
        } else {
            double percent = 0;
            try {
                percent = std::stod(zoom);
            } catch (const std::exception&) {
            }
            if (!(percent >= 10 && percent <= 800))  // (not a number either)
                throw Error("give the zoom as page-width, page-fit, or a percentage from 10 to 800");
            next["scale"] = percent;
        }
    }
    if (!pageSpec.empty()) {
        auto doc = openPdf(lib, id, att);
        int page = pageOption(args, *doc);
        next["pageIndex"] = page;
        next.erase("top");  // the page from its top
        next.erase("left");
        lib.setSetting(id, "lastPageIndex_" + key, page);  // synced, as the reader keeps it
    }
    lib.setReaderState(key, next);
    std::cout << key << "\n";
    return 0;
}

} // namespace

void registerAnnotationCommands(CommandMap& commands, Config& config)
{
    commands["annotate"] = [&config](Library& lib, Args& args) { return cmdAnnotate(lib, args, config); };
    commands["annotation"] = cmdAnnotation;
    commands["note-from-annotations"] = cmdNoteFromAnnotations;
    commands["export-pdf"] = cmdExportPdf;
    commands["render"] = cmdRender;
    commands["view-state"] = cmdViewState;
}
