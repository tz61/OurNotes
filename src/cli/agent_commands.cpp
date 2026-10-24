// ournotes-cli commands for reading and changing a library, from a shell or an AI agent
// (see docs/agents.md): libraries and collections by name, JSON output, a PDF's text,
// fields, tags, notes, collections and the trash.

#include <cmath>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "cli/agent_guide.h"
#include "cli/commands.h"
#include "core/hash.h"
#include "core/schema.h"
#include "formats/metadata.h"
#include "ournotes/version.h"
#include "pdf/document.h"

using namespace on;

namespace {

std::string lower(std::string s) { return toLower(s); }

// "FA26/CS8803": the names from the top collection down.
std::string collectionPath(const std::map<std::string, Json>& byKey, const Json& c)
{
    std::string path = jstr(c, "name");
    std::set<std::string> seen{jstr(c, "key")};
    for (auto p = c.value("parentCollection", Json()); p.is_string();) {
        auto it = byKey.find(p.get<std::string>());
        if (it == byKey.end() || !seen.insert(it->first).second)
            break;
        path = jstr(it->second, "name") + "/" + path;
        p = it->second.value("parentCollection", Json());
    }
    return path;
}

struct CollectionInfo {
    int64_t library;
    std::string libraryName;
    Json data;
    std::string path;
};

std::vector<CollectionInfo> allCollections(Library& lib, std::optional<int64_t> only = std::nullopt)
{
    std::vector<CollectionInfo> out;
    for (auto& l : lib.libraries()) {
        if (only && l.id != *only)
            continue;
        std::map<std::string, Json> byKey;
        auto colls = lib.collections(l.id);
        for (auto& c : colls)
            byKey[jstr(c, "key")] = c;
        for (auto& c : colls)
            out.push_back({l.id, l.name, c, collectionPath(byKey, c)});
    }
    return out;
}

std::string asNoteHtml(const Args& args, const std::string& content)
{
    if (args.flag("--html") || (!args.flag("--markdown") && !args.flag("--text") && startsWith(trim(content), "<")))
        return content;
    return args.flag("--text") ? textToNoteHtml(content) : markdownToNoteHtml(content);
}

Json attachmentJson(Library& lib, int64_t id, const Json& a)
{
    Json j{{"key", jstr(a, "key")},
           {"title", jstr(a, "title")},
           {"contentType", jstr(a, "contentType")},
           {"linkMode", jstr(a, "linkMode")}};
    auto path = lib.attachmentFile(id, a);
    if (!path.empty()) {
        j["path"] = pathString(path);
        j["exists"] = lib.attachmentFileExists(id, a);
    }
    if (!jstr(a, "url").empty())
        j["url"] = jstr(a, "url");
    j["annotations"] = lib.annotationCount(id, jstr(a, "key"));
    return j;
}

// An item with what is under it, for --json.
Json itemJson(Library& lib, int64_t id, const std::string& libraryName, const Json& d, bool full)
{
    Json notes = Json::array(), atts = Json::array();
    for (auto& c : lib.children(id, jstr(d, "key"))) {
        if (jbool(c, "deleted"))
            continue;
        if (jstr(c, "itemType") == "note")
            notes.push_back(full ? Json{{"key", jstr(c, "key")},
                                        {"title", noteTitle(jstr(c, "note"))},
                                        {"html", jstr(c, "note")},
                                        {"text", htmlToText(jstr(c, "note"))}}
                                 : Json{{"key", jstr(c, "key")}, {"title", noteTitle(jstr(c, "note"))}});
        else if (jstr(c, "itemType") == "attachment")
            atts.push_back(attachmentJson(lib, id, c));
    }
    Json j{{"library", libraryName}, {"libraryId", id}, {"notes", notes}, {"attachments", atts}};
    if (full) {
        j["item"] = d;
    } else {
        j["key"] = jstr(d, "key");
        j["itemType"] = jstr(d, "itemType");
        j["title"] = jstr(d, "itemType") == "note" ? noteTitle(jstr(d, "note")) : jstr(d, "title");
        j["date"] = jstr(d, "date");
        j["creators"] = d.value("creators", Json::array());
        j["tags"] = d.value("tags", Json::array());
        j["collections"] = d.value("collections", Json::array());
    }
    return j;
}

int cmdLibraries(Library& lib, Args& args)
{
    Json out = Json::array();
    for (auto& l : lib.libraries()) {
        if (args.flag("--json"))
            out.push_back(Json{{"id", l.id}, {"name", l.name}, {"type", l.type}, {"uid", l.uid}});
        else
            std::cout << l.id << "\t" << l.type << "\t" << l.name << "\t" << l.uid << "\n";
    }
    if (args.flag("--json"))
        std::cout << out.dump(2) << "\n";
    return 0;
}

int cmdCollections(Library& lib, Args& args)
{
    auto only = libraryOption(lib, args);
    auto colls = allCollections(lib, only);
    if (args.flag("--json")) {
        Json out = Json::array();
        for (auto& c : colls) {
            auto parent = c.data.value("parentCollection", Json());
            out.push_back(Json{{"library", c.libraryName},
                               {"libraryId", c.library},
                               {"key", jstr(c.data, "key")},
                               {"name", jstr(c.data, "name")},
                               {"path", c.path},
                               {"parent", parent.is_string() ? parent : Json()},
                               {"items", lib.collectionItemKeys(c.library, jstr(c.data, "key")).size()}});
        }
        std::cout << out.dump(2) << "\n";
        return 0;
    }
    std::function<void(int64_t, const std::string&, int)> print = [&](int64_t id, const std::string& parent, int depth) {
        for (auto& c : colls) {
            auto p = c.data.value("parentCollection", Json());
            if (c.library != id || (p.is_string() ? p.get<std::string>() : std::string()) != parent)
                continue;
            auto key = jstr(c.data, "key");
            std::cout << std::string(depth * 2, ' ') << jstr(c.data, "name") << "  [" << key << "] ("
                      << lib.collectionItemKeys(id, key).size() << ")\n";
            print(id, key, depth + 1);
        }
    };
    for (auto& l : lib.libraries())
        if (!only || l.id == *only) {
            std::cout << l.name << "  [library " << l.id << "]\n";
            print(l.id, "", 1);
        }
    return 0;
}

int cmdItems(Library& lib, Args& args)
{
    std::vector<int64_t> ids;
    std::set<std::string> inCollection;
    bool byCollection = !args.option("--collection").empty();
    if (byCollection) {
        auto c = findCollection(lib, args.option("--collection"), libraryOption(lib, args));
        ids.push_back(c.library);
        inCollection = lib.collectionItemKeys(c.library, c.key, true);
    } else if (auto only = libraryOption(lib, args)) {
        ids.push_back(*only);
    } else {
        for (auto& l : lib.libraries())
            ids.push_back(l.id);
    }
    auto search = args.option("--search");
    auto mode = args.option("--mode", "all");
    auto m = mode == "quick" ? SearchMode::TitleCreatorYear : mode == "everything" ? SearchMode::Everything : SearchMode::AllFields;
    bool json = args.flag("--json"), trash = args.flag("--trash"), unfiled = args.flag("--unfiled");
    auto tags = commaList(args.option("--tag"));
    Json out = Json::array();
    for (auto id : ids) {
        std::set<std::string> found, tagged, inNone;
        if (!search.empty())
            for (auto& k : lib.search(id, search, m)) {  // it, and the item it is under
                found.insert(k);
                found.insert(lib.topLevelKey(id, k));
            }
        if (!tags.empty())  // an item whose tree has every tag: it, a note or an attachment under it
            for (auto& k : lib.itemKeysWithTags(id, tags)) {
                tagged.insert(k);
                tagged.insert(lib.topLevelKey(id, k));
            }
        if (unfiled)
            inNone = lib.unfiledItemKeys(id);
        auto name = libraryName(lib, id);
        auto rows = lib.itemRows(id);
        std::set<std::string> trashed;
        for (auto& r : rows)
            if (r.trashed)
                trashed.insert(r.key);
        for (auto& r : rows) {
            // the trash: its top-level items, and the notes and attachments put there alone
            bool listed = trash ? r.trashed && (r.parentKey.empty() || !trashed.count(r.parentKey))
                                : r.parentKey.empty() && !r.trashed;
            if (!listed || (!search.empty() && !found.count(r.key)) || (byCollection && !inCollection.count(r.key)) ||
                (!tags.empty() && !tagged.count(r.key)) || (unfiled && !inNone.count(r.key)))
                continue;
            if (json) {
                if (auto d = lib.item(id, r.key))
                    out.push_back(itemJson(lib, id, name, *d, false));
            } else {
                std::cout << r.key << "  " << (r.year.empty() ? "    " : r.year) << "  " << r.creator
                          << (r.creator.empty() ? "" : " - ") << r.title << (ids.size() > 1 ? "  (" + name + ")" : "")
                          << "\n";
            }
        }
    }
    if (json)
        std::cout << out.dump(2) << "\n";
    return 0;
}

int cmdShow(Library& lib, Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (args.flag("--json")) {
        std::cout << itemJson(lib, id, libraryName(lib, id), item, true).dump(2) << "\n";
        return 0;
    }
    std::cout << item.dump(2) << "\n";
    for (auto& c : lib.children(id, jstr(item, "key")))
        std::cout << "child " << jstr(c, "itemType") << " " << jstr(c, "key") << ": "
                  << (jstr(c, "itemType") == "note" ? noteTitle(jstr(c, "note")) : jstr(c, "title")) << "\n";
    return 0;
}

// The PDF of an attachment, or an item's best one.
std::pair<int64_t, Json> pdfOf(Library& lib, const Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (jstr(item, "itemType") != "attachment") {
        auto best = lib.bestAttachment(id, jstr(item, "key"));
        if (!best)
            throw Error("item " + jstr(item, "key") + " has no PDF");
        item = *best;
    }
    return {id, item};
}

int cmdText(Library& lib, Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (jstr(item, "itemType") == "note") {
        // its pictures as where their files are, to be looked at
        static const std::regex picture(R"re(<img\b[^>]*?data-attachment-key="([A-Za-z0-9]+)"[^>]*>)re");
        auto html = jstr(item, "note");
        std::string shown;
        size_t last = 0;
        for (auto it = std::sregex_iterator(html.begin(), html.end(), picture); it != std::sregex_iterator(); ++it) {
            auto att = lib.item(id, (*it)[1].str());
            auto path = att ? lib.attachmentFile(id, *att) : fs::path();
            shown += html.substr(last, it->position() - last) + "<p>[picture: " +
                     htmlEscape(path.empty() ? (*it)[1].str() : pathString(path)) + "]</p>";
            last = it->position() + it->length();
        }
        std::cout << htmlToText(shown + html.substr(last)) << "\n";
        return 0;
    }
    auto [pid, pdf] = pdfOf(lib, args);
    requireFile(lib, pid, pdf);
    auto doc = pdf::Document::open(lib.attachmentFile(pid, pdf));
    int pages = doc->pageCount();
    if (auto n = args.option("--pages"); !n.empty())
        pages = std::min(pages, std::max(1, std::stoi(n)));
    for (int i = 0; i < pages; ++i)
        std::cout << "[page " << (i + 1) << "]\n" << doc->pageText(i) << "\n";
    return 0;
}

int cmdAnnotations(Library& lib, Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    std::vector<Json> pdfs;
    if (jstr(item, "itemType") == "attachment")
        pdfs.push_back(item);
    else
        for (auto& c : lib.children(id, jstr(item, "key"), "attachment"))
            pdfs.push_back(c);
    Json out = Json::array();
    for (auto& pdf : pdfs)
        for (auto& a : lib.annotations(id, jstr(pdf, "key"))) {
            if (args.flag("--json")) {
                Json tags = Json::array();
                for (auto& t : a.value("tags", Json::array()))
                    tags.push_back(jstr(t, "tag"));
                out.push_back(Json{{"key", jstr(a, "key")},
                                   {"attachment", jstr(pdf, "key")},
                                   {"type", jstr(a, "annotationType")},
                                   {"page", jstr(a, "annotationPageLabel")},
                                   {"text", jstr(a, "annotationText")},
                                   {"comment", jstr(a, "annotationComment")},
                                   {"color", jstr(a, "annotationColor")},
                                   {"tags", tags}});
                continue;
            }
            std::cout << "p." << jstr(a, "annotationPageLabel", "?") << "  " << jstr(a, "annotationType") << "  "
                      << jstr(a, "annotationColor");
            if (!jstr(a, "annotationText").empty())
                std::cout << "  \"" << jstr(a, "annotationText") << "\"";
            if (!jstr(a, "annotationComment").empty())
                std::cout << "  -- " << jstr(a, "annotationComment");
            std::cout << "\n";
        }
    if (args.flag("--json"))
        std::cout << out.dump(2) << "\n";
    return 0;
}

int cmdSet(Library& lib, Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    auto type = jstr(item, "itemType");
    if (type == "note" || type == "annotation")
        throw Error(type == "note" ? "a note's text is changed with edit-note" : "an annotation is changed with: annotation KEY");
    requireEditable(lib, id);
    auto changes = fieldValues(args, 1, type);
    if (changes.empty())
        throw Error("nothing to set: give FIELD=VALUE (e.g. title=\"A new title\")");
    lib.updateItem(id, jstr(item, "key"), [&](Json& d) {
        for (auto& [field, value] : changes) {
            if (value.empty())
                d.erase(field);
            else
                d[field] = value;
        }
    });
    for (auto& [field, value] : changes)
        std::cout << jstr(item, "key") << "  " << field << " = " << value << "\n";
    return 0;
}

int cmdTag(Library& lib, Args& args)
{
    auto add = commaList(args.option("--add")), remove = commaList(args.option("--remove"));
    if (add.empty() && remove.empty())
        throw Error("give --add TAG[,TAG...] and/or --remove TAG[,TAG...]");
    auto only = libraryOption(lib, args);
    std::vector<std::pair<int64_t, std::string>> items;  // all looked at before any is changed
    for (auto& key : keysFrom(args)) {
        auto [id, item] = findItem(lib, key, only);
        requireEditable(lib, id);
        items.push_back({id, key});
    }
    for (auto& [id, key] : items) {
        lib.updateItem(id, key, [&](Json& d) {
            Json tags = Json::array();
            std::set<std::string> have;
            for (auto& t : d.value("tags", Json::array()))
                if (std::find(remove.begin(), remove.end(), jstr(t, "tag")) == remove.end() && have.insert(jstr(t, "tag")).second)
                    tags.push_back(t);
            for (auto& t : add)
                if (have.insert(t).second)
                    tags.push_back(Json{{"tag", t}});
            d["tags"] = tags;
        });
        std::cout << key << "\n";
    }
    return 0;
}

// --- pictures in notes ----------------------------------------------------------------

std::string base64Decode(const std::string& in)
{
    static const std::string digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int bits = 0, value = 0;
    for (char c : in) {
        if (c == '=' || std::isspace((unsigned char)c))
            continue;
        auto d = c == '-' ? 62 : c == '_' ? 63 : digits.find(c);  // (URL-safe too)
        if (d == std::string::npos)
            throw Error("a data: picture that is not base64");
        value = (value << 6) | (int)d;
        if ((bits += 6) >= 8) {
            bits -= 8;
            out += (char)((value >> bits) & 0xff);
        }
    }
    return out;
}

// What a note can show, by the file's first bytes: ".png", ".jpg", ".gif", ".svg"; or "".
std::string pictureKind(const std::string& head)
{
    if (head.rfind("\x89PNG", 0) == 0)
        return ".png";
    if (head.rfind("\xff\xd8\xff", 0) == 0)
        return ".jpg";
    if (head.rfind("GIF8", 0) == 0)
        return ".gif";
    if (head.find("<svg") != std::string::npos)
        return ".svg";
    return {};
}

// A folder of its own for files fetched for a moment, gone with it.
struct ScratchFolder {
    fs::path path = fs::temp_directory_path() / ("ournotes-" + randomToken(6));
    ScratchFolder() { fs::create_directories(path); }
    ~ScratchFolder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A picture a note's HTML shows by its file (<img src>, as Markdown's ![what](file) makes):
// a file here, a data: URI, or a picture on the web (fetched into `scratch`).
struct NotePicture {
    size_t at = 0, length = 0;  // its <img> in the HTML
    fs::path file;
    std::string attrs;  // its width, height and alt, kept
};

std::vector<NotePicture> notePictures(const std::string& html, const fs::path& scratch)
{
    static const std::regex tag(R"(<img\b[^>]*>)", std::regex::icase);
    static const std::regex source(R"re(\bsrc\s*=\s*(?:"([^"]*)"|'([^']*)'))re", std::regex::icase);
    static const std::regex kept(R"re(\b(?:width|height|alt)\s*=\s*(?:"[^"]*"|'[^']*'))re", std::regex::icase);
    std::vector<NotePicture> out;
    int n = 0;
    for (auto it = std::sregex_iterator(html.begin(), html.end(), tag); it != std::sregex_iterator(); ++it) {
        auto t = it->str();
        std::smatch m;
        if (t.find("data-attachment-key") != std::string::npos || !std::regex_search(t, m, source))
            continue;  // stored already, or showing nothing
        auto src = htmlUnescape(m[1].matched ? m[1].str() : m[2].str());
        NotePicture p{(size_t)it->position(), (size_t)it->length(), {}, {}};
        for (auto k = std::sregex_iterator(t.begin(), t.end(), kept); k != std::sregex_iterator(); ++k)
            p.attrs += " " + k->str();
        auto name = scratch / ("picture-" + std::to_string(++n));
        if (startsWith(src, "data:")) {
            auto comma = src.find(',');
            if (comma == std::string::npos || src.substr(0, comma).find(";base64") == std::string::npos)
                throw Error("a data: picture is given in base64 (data:image/png;base64,...)");
            auto bytes = base64Decode(src.substr(comma + 1));
            auto kind = pictureKind(bytes.substr(0, 512));
            if (kind.empty())
                throw Error("a note shows PNG, JPEG, GIF and SVG pictures: the data: one is none");
            p.file = name.concat(kind);
            writeFileAtomic(p.file, bytes);
        } else if (startsWith(src, "http://") || startsWith(src, "https://")) {
            formats::downloadFile(src, name, false);
            auto kind = pictureKind(readFile(name).substr(0, 512));
            if (kind.empty())
                throw Error(src + " is no PNG, JPEG, GIF or SVG picture");
            p.file = fs::path(name).concat(kind);
            fs::rename(name, p.file);
        } else {
            auto path = u8path(startsWith(src, "file://") ? src.substr(7) : src);
            std::error_code ec;
            if (!fs::is_regular_file(path, ec))
                throw Error("no picture " + src);
            auto type = guessContentType(path);
            if (type != "image/png" && type != "image/jpeg" && type != "image/gif" && type != "image/svg+xml")
                throw Error("a note shows PNG, JPEG, GIF and SVG pictures, not " + src);
            p.file = fs::absolute(path, ec);
        }
        out.push_back(p);
    }
    return out;
}

// The pictures stored under the note, as the app keeps pasted ones; the HTML showing them
// by their keys.
std::string storeNotePictures(Library& lib, int64_t id, const std::string& noteKey, const std::string& html,
                              const std::vector<NotePicture>& pictures)
{
    std::string out;
    size_t last = 0;
    for (auto& p : pictures) {
        auto att = lib.importNoteImage(id, p.file, noteKey);
        out += html.substr(last, p.at - last) + "<img data-attachment-key=\"" + jstr(att, "key") + "\"" + p.attrs + ">";
        last = p.at + p.length;
    }
    return out + html.substr(last);
}

// A note's HTML without the <div data-schema-version> around it.
std::string noteBody(std::string h)
{
    if (h.find("<div data-schema-version") == 0) {
        h = h.substr(h.find('>') + 1);
        auto end = h.rfind("</div>");
        if (end != std::string::npos)
            h.resize(end);
    }
    return h;
}

int cmdAddNote(Library& lib, Args& args)
{
    auto spec = args.option("--collection");
    std::string parent;
    int64_t id = 0;
    size_t contentAt = 0;
    bool standalone = args.flag("--standalone") || !spec.empty();
    if (!standalone) {  // a child note of an item
        auto [lid, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
        if (!schema::isRegularType(jstr(item, "itemType")))
            throw Error("notes go under a regular item (a paper, a book...), not a " + jstr(item, "itemType"));
        id = lid;
        parent = jstr(item, "key");
        contentAt = 1;
    }
    std::optional<CollectionRef> into;
    if (!spec.empty()) {
        into = findCollection(lib, spec, libraryOption(lib, args));
        id = into->library;
    } else if (standalone) {  // in the library, in no collection
        id = args.libraryId(lib);
    }
    requireEditable(lib, id);
    auto html = asNoteHtml(args, contentFrom(args, contentAt));
    ScratchFolder scratch;
    auto pictures = notePictures(html, scratch.path);  // all there before the note is made
    if (trim(htmlToText(html)).empty() && html.find("<img") == std::string::npos)
        throw Error("the note is empty");
    Json data{{"itemType", "note"}, {"note", html}};
    if (!parent.empty())
        data["parentItem"] = parent;
    if (into)
        data["collections"] = Json::array({into->key});
    auto key = jstr(lib.createItem(id, data), "key");
    if (!pictures.empty()) {
        auto shown = storeNotePictures(lib, id, key, html, pictures);
        lib.updateItem(id, key, [&](Json& d) { d["note"] = shown; });
    }
    std::cout << key << "\n";
    return 0;
}

int cmdEditNote(Library& lib, Args& args)
{
    auto [id, note] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (jstr(note, "itemType") != "note")
        throw Error(jstr(note, "key") + " is not a note");
    requireEditable(lib, id);
    auto html = asNoteHtml(args, contentFrom(args, 1));
    ScratchFolder scratch;
    auto pictures = notePictures(html, scratch.path);
    html = storeNotePictures(lib, id, jstr(note, "key"), html, pictures);
    if (args.flag("--append"))
        html = "<div data-schema-version=\"9\">" + noteBody(jstr(note, "note")) + noteBody(html) + "</div>";
    lib.updateItem(id, jstr(note, "key"), [&](Json& d) { d["note"] = html; });
    std::cout << jstr(note, "key") << "\n";
    return 0;
}

int cmdAddImage(Library& lib, Args& args)
{
    auto [id, note] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (jstr(note, "itemType") != "note")
        throw Error(jstr(note, "key") + " is not a note: pictures go into notes (a file under an item: attach)");
    if (args.positionals.size() < 2)
        throw Error("give the pictures: files, web addresses or data: URIs");
    requireEditable(lib, id);
    int width = (int)numberOption(args, "--width", 0, 1, 10000);
    std::string html;
    for (size_t i = 1; i < args.positionals.size(); ++i)
        html += "<p><img src=\"" + htmlEscape(args.positionals[i]) + "\"" +
                (width ? " width=\"" + std::to_string(width) + "\"" : std::string()) + "></p>";
    ScratchFolder scratch;
    auto pictures = notePictures(html, scratch.path);
    html = storeNotePictures(lib, id, jstr(note, "key"), html, pictures);
    lib.updateItem(id, jstr(note, "key"), [&](Json& d) {
        d["note"] = "<div data-schema-version=\"9\">" + noteBody(jstr(d, "note")) + html + "</div>";
    });
    std::cout << jstr(note, "key") << "\n";
    return 0;
}

int cmdNewCollection(Library& lib, Args& args)
{
    auto name = trim(args.positional(0));
    if (name.empty())
        throw Error("a collection needs a name");
    int64_t id = 0;
    std::string parent;
    if (auto p = args.option("--parent"); !p.empty()) {
        auto c = findCollection(lib, p, libraryOption(lib, args));
        id = c.library;
        parent = c.key;
    } else {
        id = args.libraryId(lib);
    }
    requireEditable(lib, id);
    std::cout << jstr(lib.createCollection(id, name, parent), "key") << "\n";
    return 0;
}

int fileOrUnfile(Library& lib, Args& args, bool file)
{
    if (args.option("--collection").empty())
        throw Error("say which collection with --collection");
    auto c = findCollection(lib, args.option("--collection"), libraryOption(lib, args));
    requireEditable(lib, c.library);
    std::vector<std::string> keys;
    for (auto& key : keysFrom(args)) {
        auto [id, item] = findItem(lib, key, c.library);
        if (!jstr(item, "parentItem").empty())
            throw Error(key + " is under another item: it goes with it");
        keys.push_back(key);
    }
    if (file)
        lib.addToCollection(c.library, c.key, keys);
    else
        lib.removeFromCollection(c.library, c.key, keys);
    for (auto& k : keys)
        std::cout << k << "\n";
    return 0;
}

// This program's own file.
fs::path executablePath()
{
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    buf.resize(GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size()));
    return fs::path(buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    std::error_code ec;
    auto path = fs::canonical(buf.c_str(), ec);
    return ec ? fs::path(buf.c_str()) : path;
#else
    std::error_code ec;
    auto path = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : path;
#endif
}

// Where this installation keeps its programs, this guide, the library and the settings.
std::string whereText(Library& lib, Config& config)
{
    auto exe = executablePath();
    auto bin = exe.parent_path();
#if defined(__APPLE__)
    auto guide = bin.parent_path() / "Resources" / "AGENTS.md";  // OurNotes.app/Contents/
#else
    auto guide = bin.parent_path() / "AGENTS.md";
#endif
#if defined(_WIN32)
    auto app = bin / "ournotes.exe";
#else
    auto app = bin / "ournotes";
#endif
    std::error_code ec;
    auto show = [&](const fs::path& p) { return fs::exists(p, ec) ? pathString(p) : pathString(p) + "  (not there)"; };
    std::string out = "OurNotes " OURNOTES_VERSION "\n";
    out += "ournotes-cli:  " + pathString(exe) + "\n";
    out += "the app:       " + show(app) + "\n";
    out += "this guide:    " + show(guide) + "\n";
    out += "data folder:   " + pathString(lib.dataDir()) + "  (the library: ournotes.sqlite; files in storage/)\n";
    out += "settings:      " + pathString(config.path()) + "\n";
    if (auto t = config.activeTarget())
        out += "sync:          " + t->name + ", " + (t->kind == "local" ? "a folder, " + t->path : "a server, " + t->url) + "\n";
    else
        out += "sync:          none set up (the library stays on this computer)\n";
    std::string names;
    for (auto& l : lib.libraries())
        names += (names.empty() ? "" : ", ") + l.name;
    out += "libraries:     " + names + "\n";
    return out;
}

int trashOrRestore(Library& lib, Args& args, bool trash)
{
    auto only = libraryOption(lib, args);
    std::vector<std::pair<int64_t, std::string>> items;  // all looked at before any is changed
    for (auto& key : keysFrom(args)) {
        auto [id, item] = findItem(lib, key, only);
        if (jstr(item, "itemType") == "annotation")
            throw Error(key + " is an annotation: it goes with delete KEY --yes, as in the app");
        requireEditable(lib, id);
        items.push_back({id, key});
    }
    for (auto& [id, key] : items) {
        lib.trashItems(id, {key}, trash);
        std::cout << key << "\n";
    }
    return 0;
}

} // namespace

std::optional<int64_t> libraryOption(Library& lib, const Args& args)
{
    auto v = args.option("--library");
    if (v.empty())
        return std::nullopt;
    return findLibrary(lib, v);
}

std::pair<int64_t, Json> findItem(Library& lib, const std::string& key, std::optional<int64_t> only)
{
    if (key.empty())
        throw Error("an item key is needed");
    std::vector<std::pair<int64_t, Json>> found;
    for (auto& l : lib.libraries())
        if (!only || l.id == *only)
            if (auto d = lib.item(l.id, key))
                found.push_back({l.id, *d});
    if (found.empty())
        throw Error("no item " + key + " (see: ournotes-cli items)");
    if (found.size() > 1)
        throw Error("item " + key + " is in several libraries: say which with --library");
    return found[0];
}

std::vector<std::string> keysFrom(const Args& args, size_t from)
{
    std::vector<std::string> keys(args.positionals.begin() + std::min(from, args.positionals.size()),
                                  args.positionals.end());
    if (keys.empty())
        throw Error("no item keys given");
    return keys;
}

std::string contentFrom(const Args& args, size_t at)
{
    auto v = args.positional(at);
    if (!v.empty() && v != "-")
        return v;
    return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
}

std::string libraryName(Library& lib, int64_t id)
{
    auto l = lib.library(id);
    return l ? l->name : std::string();
}

std::vector<std::string> commaList(const std::string& s)
{
    std::vector<std::string> out;
    for (auto& t : split(s, ','))
        if (!trim(t).empty())
            out.push_back(trim(t));
    return out;
}

std::vector<std::pair<std::string, std::string>> fieldValues(const Args& args, size_t from, const std::string& itemType)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (size_t i = from; i < args.positionals.size(); ++i) {
        auto& a = args.positionals[i];
        auto eq = a.find('=');
        if (eq == std::string::npos || eq == 0)
            throw Error("expected FIELD=VALUE, got \"" + a + "\"");
        auto field = a.substr(0, eq);
        if (!schema::hasField(itemType, field)) {
            std::string valid;
            for (auto& f : schema::fieldsFor(itemType))
                valid += (valid.empty() ? "" : ", ") + f;
            throw Error("a " + itemType + " has no field \"" + field + "\"; it has: " + valid +
                        (field == "creators"   ? " (authors are changed with: creators KEY)"
                         : field == "itemType" ? " (the type is changed with: set-type KEY TYPE)"
                                               : ""));
        }
        out.push_back({field, a.substr(eq + 1)});
    }
    return out;
}

double numberOption(const Args& args, const std::string& name, double def, double min, double max)
{
    auto v = args.option(name);
    if (v.empty())
        return def;
    double n = 0;
    try {
        size_t used = 0;
        n = std::stod(v, &used);
        if (used != v.size())
            throw Error("");
    } catch (const std::exception&) {
        n = min - 1;
    }
    if (!(n >= min && n <= max)) {
        auto text = [](double x) { return x == std::floor(x) ? std::to_string((long long)x) : std::to_string(x); };
        throw Error(name + " is a number from " + text(min) + " to " + text(max));
    }
    return n;
}

void requirePositionals(const Args& args, size_t most)
{
    if (args.positionals.size() > most)
        throw Error("what is \"" + args.positionals[most] + "\" for? (see: ournotes-cli help)");
}

void requireFile(Library& lib, int64_t id, const Json& att)
{
    if (lib.attachmentFileExists(id, att))
        return;
    if (jstr(att, "linkMode") == "linked_file")
        throw Error("the file " + jstr(att, "key") + " links to is not at " + pathString(lib.attachmentFile(id, att)));
    throw Error("the file of " + jstr(att, "key") + " is not on this computer (not downloaded)");
}

void requireEditable(Library& lib, int64_t id)
{
    auto l = lib.library(id);
    if (l && !l->editable)
        throw Error("the library \"" + l->name + "\" can only be read here");
}

void requireYes(const Args& args, const std::string& what)
{
    if (!args.flag("--yes"))
        throw Error(what + ". It cannot be undone: add --yes to do it");
}

int64_t findLibrary(Library& lib, const std::string& spec)
{
    auto libs = lib.libraries();
    bool numeric = !spec.empty() && std::all_of(spec.begin(), spec.end(), [](char c) { return std::isdigit((unsigned char)c); });
    for (auto& l : libs)
        if ((numeric && std::to_string(l.id) == spec) || l.name == spec)
            return l.id;
    std::vector<int64_t> loose;
    for (auto& l : libs)
        if (lower(l.name) == lower(spec))
            loose.push_back(l.id);
    if (loose.size() == 1)
        return loose[0];
    std::string names;
    for (auto& l : libs)
        names += (names.empty() ? "" : ", ") + l.name;
    throw Error("no library \"" + spec + "\"; there are: " + names);
}

int64_t findItemLibrary(Library& lib, const std::string& key) { return findItem(lib, key, std::nullopt).first; }

CollectionRef findCollection(Library& lib, const std::string& spec, std::optional<int64_t> inLibrary)
{
    if (spec.empty())
        throw Error("no collection given");
    auto colls = allCollections(lib, inLibrary);
    auto pick = [&](const std::function<bool(const CollectionInfo&)>& match) {
        std::vector<CollectionInfo> found;
        for (auto& c : colls)
            if (match(c))
                found.push_back(c);
        return found;
    };
    // its key; its path; the library's name and its path; then the same in any case
    auto found = pick([&](auto& c) { return jstr(c.data, "key") == spec; });
    if (found.empty())
        found = pick([&](auto& c) { return c.path == spec || c.libraryName + "/" + c.path == spec; });
    if (found.empty())
        found = pick([&](auto& c) {
            return lower(c.path) == lower(spec) || lower(c.libraryName + "/" + c.path) == lower(spec);
        });
    if (found.size() == 1)
        return {found[0].library, jstr(found[0].data, "key")};
    if (found.empty())
        throw Error("no collection \"" + spec + "\" (see: ournotes-cli collections)");
    std::string which;
    for (auto& c : found)
        which += "\n  " + c.libraryName + "/" + c.path + "  [" + jstr(c.data, "key") + "]";
    throw Error("\"" + spec + "\" could be any of these; give its key, or the library's name first:" + which);
}

void registerAllCommands(CommandMap& commands, Config& config)
{
    registerAgentCommands(commands, config);
    registerExtraCommands(commands, config);
    registerEditCommands(commands, config);
    registerAnnotationCommands(commands, config);
}

void registerAgentCommands(CommandMap& commands, Config& config)
{
    commands["guide"] = [&config](Library& lib, Args&) {
        std::cout << kAgentGuide << "\n## This installation (`ournotes-cli where`)\n\n```text\n"
                  << whereText(lib, config) << "```\n";
        return 0;
    };
    commands["where"] = [&config](Library& lib, Args&) {
        std::cout << whereText(lib, config);
        return 0;
    };
    commands["libraries"] = cmdLibraries;
    commands["collections"] = cmdCollections;
    commands["items"] = cmdItems;
    commands["show"] = cmdShow;
    commands["text"] = cmdText;
    commands["annotations"] = cmdAnnotations;
    commands["set"] = cmdSet;
    commands["tag"] = cmdTag;
    commands["add-note"] = cmdAddNote;
    commands["edit-note"] = cmdEditNote;
    commands["add-image"] = cmdAddImage;
    commands["new-collection"] = cmdNewCollection;
    commands["file"] = [](Library& lib, Args& args) { return fileOrUnfile(lib, args, true); };
    commands["unfile"] = [](Library& lib, Args& args) { return fileOrUnfile(lib, args, false); };
    commands["trash"] = [](Library& lib, Args& args) { return trashOrRestore(lib, args, true); };
    commands["restore"] = [](Library& lib, Args& args) { return trashOrRestore(lib, args, false); };
}
