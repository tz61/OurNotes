#include "cli/mcp_server.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <streambuf>

#include "core/json.h"
#include "core/util.h"
#include "ournotes/version.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace on;

namespace {

constexpr const char* kOrigin = "ournotes-cli mcp";  // in the app's sync history
constexpr size_t kMaxAnswer = 200000;                 // characters of a tool's answer; more is cut
constexpr int kDefaultItems = 50, kMostItems = 500;
constexpr int kDefaultPages = 20;
// The protocol's versions this server speaks, the newest first
const char* const kProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

const char* const kInstructions =
    "OurNotes is the user's reference library, like Zotero: papers, books and web pages (items) with "
    "their PDFs, the user's notes, and highlights and comments in the PDFs (annotations). Every item, "
    "note, attachment and annotation has an 8-character key. Collections nest and are named by their "
    "path, such as \"FA26/CS8803\". To answer about the library: search_items (or list_collections, then "
    "search_items with a collection), get_item for an item's fields, notes and attachments, read_text for "
    "a PDF's or a note's text, get_annotations for what the user marked. When a reply speaks of an item "
    "or a page, get_link gives a Markdown link that opens it in OurNotes. Tools that change the library "
    "are offered only when the user allowed it; changes are synced to the user's other computers, and "
    "items, notes and attachments are not deleted for good (trash_items puts them in the trash, where the "
    "user can restore them); annotations, which have no trash in the app either, are deleted for good with "
    "delete_annotations.";

class ToolError : public on::Error {
public:
    using on::Error::Error;
};

// --- a tool's arguments --------------------------------------------------------------------------

std::string text(const Json& a, const char* name, bool required = false)
{
    if (!a.contains(name) || a[name].is_null() || (a[name].is_string() && a[name].get<std::string>().empty())) {
        if (required)
            throw ToolError(std::string("missing argument: ") + name);
        return {};
    }
    if (a[name].is_string())
        return a[name].get<std::string>();
    if (a[name].is_number_integer())  // (a library's ID given as a number)
        return std::to_string(a[name].get<int64_t>());
    throw ToolError(std::string(name) + " should be a string");
}

std::string number(const Json& a, const char* name, int64_t def = 0)
{
    if (!a.contains(name) || a[name].is_null())
        return def > 0 ? std::to_string(def) : std::string();
    if (a[name].is_number())
        return std::to_string((int64_t)a[name].get<double>());
    if (a[name].is_string())
        return a[name].get<std::string>();  // (checked by the command)
    throw ToolError(std::string(name) + " should be a number");
}

bool yes(const Json& a, const char* name) { return a.contains(name) && a[name].is_boolean() && a[name].get<bool>(); }

std::vector<std::string> strings(const Json& a, const char* name, bool required = false)
{
    std::vector<std::string> out;
    if (a.contains(name) && a[name].is_array()) {
        for (auto& v : a[name]) {
            if (!v.is_string())
                throw ToolError(std::string(name) + " should be a list of strings");
            if (!v.get<std::string>().empty())
                out.push_back(v.get<std::string>());
        }
    } else if (a.contains(name) && a[name].is_string() && !a[name].get<std::string>().empty()) {
        out.push_back(a[name].get<std::string>());  // (one given alone)
    } else if (a.contains(name) && !a[name].is_null()) {
        throw ToolError(std::string(name) + " should be a list of strings");
    }
    if (required && out.empty())
        throw ToolError(std::string("missing argument: ") + name);
    return out;
}

std::string commaJoined(const std::vector<std::string>& tags, const char* name)
{
    for (auto& t : tags)
        if (t.find(',') != std::string::npos)
            throw ToolError(std::string(name) + ": a tag with a comma in it cannot be given here");
    return join(tags, ",");
}

// A note's text as the command takes it: "-" alone would be read as "from standard input"
// (and there is nothing there), so it is written as Markdown's "\\-" (a dash).
std::string markdownText(const std::string& s) { return s == "-" ? "\\-" : s; }

// A command line: the command, its options, then (after "--", so that none is taken for an
// option) the positionals.
struct Line {
    std::vector<std::string> options, positionals;
    void option(const char* name, const std::string& value)
    {
        if (!value.empty()) {
            options.push_back(name);
            options.push_back(value);
        }
    }
    void flag(const char* name, bool on = true)
    {
        if (on)
            options.push_back(name);
    }
    std::vector<std::string> words(const std::string& command) const
    {
        std::vector<std::string> w{command};
        w.insert(w.end(), options.begin(), options.end());
        w.push_back("--");
        w.insert(w.end(), positionals.begin(), positionals.end());
        return w;
    }
};

// --markdown or --html, as `format` says (else the command tells HTML from Markdown itself).
void noteFormat(Line& l, const Json& a)
{
    auto format = text(a, "format");
    if (format == "markdown" || format == "html")
        l.flag(format == "html" ? "--html" : "--markdown");
    else if (!format.empty())
        throw ToolError("format should be markdown or html");
}

// --- the tools -----------------------------------------------------------------------------------

struct Tool {
    std::string name, title, description;
    Json schema;
    bool writes = false;
    bool destructive = false;  // replaces or removes what was there
    bool online = false;       // asks services on the internet
    std::function<std::vector<std::string>(const Json&)> line;
    std::function<std::string(const std::string& output, const Json& args)> answer;  // (else the output)
};

Json prop(const char* type, const std::string& description) { return Json{{"type", type}, {"description", description}}; }

Json stringList(const std::string& description)
{
    return Json{{"type", "array"}, {"items", Json{{"type", "string"}}}, {"description", description}};
}

Json choice(const std::vector<std::string>& values, const std::string& description)
{
    return Json{{"type", "string"}, {"enum", values}, {"description", description}};
}

Json schema(Json properties, const std::vector<std::string>& required = {})
{
    Json s{{"type", "object"}, {"properties", properties.is_null() ? Json::object() : properties},
           {"additionalProperties", false}};
    if (!required.empty())
        s["required"] = required;
    return s;
}

const char* const kNoteFormat =
    "markdown (default): headings, lists, tables, **bold**, *italic*, links, $formulas$; a colour as "
    "<span style=\"color: #ff6666\">...</span> (the app's: red #ff6666, orange #f19837, yellow #ffd400, "
    "green #5fb236, blue #2ea8e5, purple #a28ae5, magenta #e56eee, gray #aaaaaa; background-color too), "
    "<u>, <s>, <sub>, <sup>. html: the note's HTML, as get_item gives it";
const char* const kKey = "An item's key (8 letters and digits), as search_items or get_item gave it";
const char* const kLibrary = "A library's name or ID (list_libraries); all libraries, or the user's own, if not given";
const char* const kCollection = "A collection's path (names joined by '/', such as \"FA26/CS8803\") or key";

// Cut where a UTF-8 character begins.
std::string cutAt(const std::string& s, size_t bytes)
{
    while (bytes > 0 && (static_cast<unsigned char>(s[bytes]) & 0xC0) == 0x80)
        --bytes;
    return s.substr(0, bytes);
}

const std::vector<Tool>& tools()
{
    static const std::vector<Tool> all = [] {
        std::vector<Tool> t;
        // reading
        t.push_back({"list_libraries", "List libraries", "The user's libraries (their own, and group libraries): ID, name and type.",
                     schema(nullptr), false, false, false, [](const Json&) {
                         return std::vector<std::string>{"libraries", "--json"};
                     }, {}});
        t.push_back({"list_collections", "List collections",
                     "Every collection with its path (names joined by '/'), key, library and number of items.",
                     schema({{"library", prop("string", kLibrary)}}), false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.flag("--json");
                         l.option("--library", text(a, "library"));
                         return l.words("collections");
                     }, {}});
        t.push_back(
            {"search_items", "Search items",
             "Top-level items (papers, books, web pages...) that match words, are in a collection, have tags, "
             "are in no collection or in the trash; in every library unless one is given. Each with its key, "
             "type, title, creators, date, tags, collections, notes (key, title) and attachments (key, title, "
             "whether the file is on this computer, number of annotations). Without criteria: all items.",
             schema({{"query", prop("string", "Words to look for (see mode)")},
                     {"mode", choice({"quick", "all", "everything"},
                                     "quick: titles, creators and years; all (default): every field and the notes; "
                                     "everything: the PDFs' full text too")},
                     {"collection", prop("string", kCollection)},
                     {"library", prop("string", kLibrary)},
                     {"tags", stringList("Only items that have all these tags")},
                     {"unfiled", prop("boolean", "Only items in no collection")},
                     {"trash", prop("boolean", "The items in the trash instead")},
                     {"limit", prop("integer", "At most this many (default 50, at most 500)")}}),
             false, false, false,
             [](const Json& a) {
                 Line l;
                 l.flag("--json");
                 l.option("--search", text(a, "query"));
                 l.option("--mode", text(a, "mode"));
                 l.option("--collection", text(a, "collection"));
                 l.option("--library", text(a, "library"));
                 l.option("--tag", commaJoined(strings(a, "tags"), "tags"));
                 l.flag("--unfiled", yes(a, "unfiled"));
                 l.flag("--trash", yes(a, "trash"));
                 return l.words("items");
             },
             [](const std::string& output, const Json& a) {
                 auto items = Json::parse(output);
                 int limit = kDefaultItems;
                 if (a.contains("limit") && a["limit"].is_number())
                     limit = std::clamp((int)a["limit"].get<double>(), 1, kMostItems);
                 size_t total = items.size();
                 if (total > (size_t)limit)
                     items.erase(items.begin() + limit, items.end());
                 auto shown = items.dump(-1, ' ', false, Json::error_handler_t::replace);
                 if (total > items.size())
                     shown += "\n(" + std::to_string(items.size()) + " of " + std::to_string(total) +
                              " items: narrow the search, or ask for a higher limit)";
                 else if (total == 0)
                     shown += "\n(no items)";
                 return shown;
             }});
        t.push_back({"get_item", "Get an item",
                     "One item by its key: all its fields, its notes (HTML and plain text) and attachments (with "
                     "the files' paths). A note's, attachment's or annotation's key works too.",
                     schema({{"key", prop("string", kKey)}}, {"key"}), false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.flag("--json");
                         l.positionals = {text(a, "key", true)};
                         return l.words("show");
                     }, {}});
        t.push_back({"read_text", "Read a PDF or a note",
                     "The text of an item's PDF (or of a PDF attachment), \"[page N]\" before each page; or a "
                     "note's text. A PDF is read in parts: pages (20 by default) from from_page; then a last line "
                     "says which pages of how many were given.",
                     schema({{"key", prop("string", "The key of an item with a PDF, of a PDF attachment, or of a note")},
                             {"from_page", prop("integer", "The first page to read (default 1)")},
                             {"pages", prop("integer", "How many pages (default 20)")}},
                            {"key"}),
                     false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.option("--from", number(a, "from_page"));
                         l.option("--pages", number(a, "pages", kDefaultPages));
                         l.positionals = {text(a, "key", true)};
                         return l.words("text");
                     }, {}});
        t.push_back({"get_annotations", "Get annotations",
                     "What the user marked in an item's PDFs (or in one PDF): highlights, underlines, notes and "
                     "drawings, each with its key, type, colour, page, the text marked, comment and tags.",
                     schema({{"key", prop("string", kKey)}}, {"key"}), false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.flag("--json");
                         l.positionals = {text(a, "key", true)};
                         return l.words("annotations");
                     }, {}});
        t.push_back({"list_tags", "List tags", "The tags, how many items have each, and their colours.",
                     schema({{"library", prop("string", kLibrary)}}), false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.flag("--json");
                         l.option("--library", text(a, "library"));
                         return l.words("tags");
                     }, {}});
        t.push_back({"get_related", "Get related items", "The items the user marked as related to an item.",
                     schema({{"key", prop("string", kKey)}}, {"key"}), false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.flag("--json");
                         l.positionals = {text(a, "key", true)};
                         return l.words("related");
                     }, {}});
        t.push_back({"format_citations", "Format citations",
                     "A bibliography of items (by key, or a whole collection) in a citation style, or with "
                     "in_text their in-text citations.",
                     schema({{"keys", stringList("Items' keys")},
                             {"collection", prop("string", kCollection + std::string(" (instead of keys)"))},
                             {"style", prop("string", "apa (default), chicago-author-date, ieee, mla, harvard, nature, vancouver")},
                             {"in_text", prop("boolean", "In-text citations, such as (Smith, 2020), instead")}}),
                     false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.option("--collection", text(a, "collection"));
                         l.option("--style", text(a, "style"));
                         l.flag("--in-text", yes(a, "in_text"));
                         l.positionals = strings(a, "keys");
                         if (l.positionals.empty() && text(a, "collection").empty())
                             throw ToolError("give keys or a collection");
                         return l.words("cite");
                     }, {}});
        t.push_back({"export_items", "Export items", "Items (by key, or a whole collection) as BibTeX, RIS or CSL JSON.",
                     schema({{"keys", stringList("Items' keys")},
                             {"collection", prop("string", kCollection + std::string(" (instead of keys)"))},
                             {"format", choice({"bibtex", "ris", "csljson"}, "bibtex (default), ris or csljson")}}),
                     false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.option("--collection", text(a, "collection"));
                         l.option("--format", text(a, "format").empty() ? "bibtex" : text(a, "format"));
                         l.positionals = strings(a, "keys");
                         if (l.positionals.empty() && text(a, "collection").empty())
                             throw ToolError("give keys or a collection");
                         return l.words("export");
                     }, {}});
        t.push_back({"get_link", "Get a link into OurNotes",
                     "A Markdown link that opens an item, a note, an annotation or a page of an item's PDF in "
                     "OurNotes, such as [(Smith, 2020, p. 3)](ournotes://...); with a quote, the words before it. "
                     "Put it in a reply or a note so the user can go to the source with a click.",
                     schema({{"key", prop("string", "The key of an item, note, attachment or annotation")},
                             {"page", prop("integer", "A page of the item's PDF")},
                             {"quote", prop("string", "Words quoted from there")}},
                            {"key"}),
                     false, false, false,
                     [](const Json& a) {
                         Line l;
                         l.flag("--markdown");
                         l.option("--page", number(a, "page"));
                         l.option("--quote", text(a, "quote"));
                         l.positionals = {text(a, "key", true)};
                         return l.words("link");
                     }, {}});

        // changing (with --write)
        t.push_back({"add_note", "Add a note",
                     "A new note, in Markdown (or HTML): under an item (item_key), or a note of its own in a "
                     "collection or in a library. Links from get_link work in it. Coloured text: "
                     "<span style=\"color: #ff6666\">...</span> in the Markdown. Answers with the new note's key.",
                     schema({{"text", prop("string", "The note, in Markdown (or HTML, with format html)")},
                             {"format", choice({"markdown", "html"}, kNoteFormat)},
                             {"item_key", prop("string", "The item to put it under")},
                             {"collection", prop("string", kCollection + std::string(": a note of its own there"))},
                             {"library", prop("string", "A library's name or ID, for a note of its own (default: the user's)")}},
                            {"text"}),
                     true, false, false,
                     [](const Json& a) {
                         Line l;
                         auto body = markdownText(text(a, "text", true));
                         auto item = text(a, "item_key");
                         noteFormat(l, a);
                         l.option("--library", text(a, "library"));
                         if (!item.empty()) {
                             l.positionals = {item, body};
                         } else {
                             l.option("--collection", text(a, "collection"));
                             l.flag("--standalone", text(a, "collection").empty());
                             l.positionals = {body};
                         }
                         return l.words("add-note");
                     }, {}});
        t.push_back({"edit_note", "Edit a note",
                     "Replaces a note's whole text, or adds to its end (append). Replacing loses what the new "
                     "text does not have: to change a note the user formatted (colours, underlines, a "
                     "formula's colour), read its HTML with get_item first, change only what should change, "
                     "and send all of it back with format html; or keep its colours as <span style=\"color: "
                     "...\"> in Markdown.",
                     schema({{"note_key", prop("string", "The note's key")},
                             {"text", prop("string", "The text, in Markdown (or HTML, with format html)")},
                             {"format", choice({"markdown", "html"}, kNoteFormat)},
                             {"append", prop("boolean", "Add to the end instead of replacing")}},
                            {"note_key", "text"}),
                     true, true, false,
                     [](const Json& a) {
                         Line l;
                         noteFormat(l, a);
                         l.flag("--append", yes(a, "append"));
                         l.positionals = {text(a, "note_key", true), markdownText(text(a, "text", true))};
                         return l.words("edit-note");
                     }, {}});
        t.push_back({"set_fields", "Change an item's fields",
                     "Sets fields of an item: title, date, abstractNote, publicationTitle, volume, issue, pages, "
                     "DOI, url, extra and the others of its type (an empty value removes the field). A field the "
                     "item's type does not have is refused, with the list of those it has. Not for authors, the "
                     "item's type or a note's text.",
                     schema({{"key", prop("string", kKey)},
                             {"fields", Json{{"type", "object"},
                                             {"additionalProperties", Json{{"type", "string"}}},
                                             {"description", "Field names and their new values"}}}},
                            {"key", "fields"}),
                     true, true, false,
                     [](const Json& a) {
                         Line l;
                         l.positionals = {text(a, "key", true)};
                         if (!a.contains("fields") || !a["fields"].is_object() || a["fields"].empty())
                             throw ToolError("missing argument: fields (names and values)");
                         for (auto& [name, value] : a["fields"].items()) {
                             if (!value.is_string())
                                 throw ToolError("the value of " + name + " should be a string");
                             l.positionals.push_back(name + "=" + value.get<std::string>());
                         }
                         return l.words("set");
                     }, {}});
        t.push_back({"tag_items", "Tag items", "Adds tags to items and removes others.",
                     schema({{"keys", stringList("Items' keys")},
                             {"add", stringList("Tags to add")},
                             {"remove", stringList("Tags to remove")}},
                            {"keys"}),
                     true, false, false,
                     [](const Json& a) {
                         Line l;
                         l.option("--add", commaJoined(strings(a, "add"), "add"));
                         l.option("--remove", commaJoined(strings(a, "remove"), "remove"));
                         if (l.options.empty())
                             throw ToolError("give tags to add or to remove");
                         l.positionals = strings(a, "keys", true);
                         return l.words("tag");
                     }, {}});
        t.push_back({"file_items", "Put items into a collection",
                     "Puts items into a collection (they stay in the others they are in).",
                     schema({{"keys", stringList("Items' keys")}, {"collection", prop("string", kCollection)}},
                            {"keys", "collection"}),
                     true, false, false,
                     [](const Json& a) {
                         Line l;
                         l.option("--collection", text(a, "collection", true));
                         l.positionals = strings(a, "keys", true);
                         return l.words("file");
                     }, {}});
        t.push_back({"create_collection", "Create a collection",
                     "A new collection, at the top of a library or inside another (parent). Answers with its key.",
                     schema({{"name", prop("string", "Its name")},
                             {"parent", prop("string", kCollection + std::string(": the collection to put it in"))},
                             {"library", prop("string", "A library's name or ID (default: the user's)")}},
                            {"name"}),
                     true, false, false,
                     [](const Json& a) {
                         Line l;
                         l.option("--parent", text(a, "parent"));
                         l.option("--library", text(a, "library"));
                         l.positionals = {text(a, "name", true)};
                         return l.words("new-collection");
                     }, {}});
        t.push_back({"annotate_pdf", "Mark a passage in a PDF",
                     "Marks a passage of an item's PDF, found by its words (quote: in any case, on page or on the "
                     "first page that has them): a highlight (default), an underline, or a note beside it; with a "
                     "colour and a comment. Answers with the annotation's key.",
                     schema({{"key", prop("string", "The key of the item (or of its PDF)")},
                             {"quote", prop("string", "The words to mark, as in the PDF")},
                             {"type", choice({"highlight", "underline", "note"}, "highlight (default), underline, or note")},
                             {"page", prop("integer", "The page to look on")},
                             {"color", prop("string", "yellow (default), red, green, blue, purple, magenta, orange, gray, or #rrggbb")},
                             {"comment", prop("string", "A comment on it")}},
                            {"key", "quote"}),
                     true, false, false,
                     [](const Json& a) {
                         Line l;
                         l.option("--quote", text(a, "quote", true));
                         l.option("--type", text(a, "type"));
                         l.option("--page", number(a, "page"));
                         l.option("--color", text(a, "color"));
                         l.option("--comment", text(a, "comment"));
                         l.positionals = {text(a, "key", true)};
                         return l.words("annotate");
                     }, {}});
        t.push_back({"add_items", "Add items by identifier",
                     "Adds items by DOI, arXiv ID, ISBN or PMID (\"PMID:...\"); their details, and an arXiv "
                     "paper's PDF, are looked up online. Answers with what was added.",
                     schema({{"identifiers", stringList("DOIs, arXiv IDs, ISBNs or PMIDs")},
                             {"collection", prop("string", kCollection + std::string(": to put them in"))}},
                            {"identifiers"}),
                     true, false, true,
                     [](const Json& a) {
                         Line l;
                         l.option("--collection", text(a, "collection"));
                         l.positionals = strings(a, "identifiers", true);
                         return l.words("add");
                     }, {}});
        t.push_back({"trash_items", "Move items to the trash",
                     "Moves items, notes or attachments to the trash, where the user can look them over and restore "
                     "them. Nothing is deleted for good. Not for annotations: delete_annotations.",
                     schema({{"keys", stringList("The keys")}}, {"keys"}), true, true, false,
                     [](const Json& a) {
                         Line l;
                         l.positionals = strings(a, "keys", true);
                         return l.words("trash");
                     }, {}});
        t.push_back({"delete_annotations", "Delete annotations",
                     "Deletes annotations (highlights, underlines, notes, text boxes, drawings in PDFs; their keys "
                     "from get_annotations) for good, as deleting them in the app does: annotations have no "
                     "trash, and the deletion syncs to the user's other computers. Only annotations: if any key "
                     "is something else, nothing is deleted. Delete what the user asked to have deleted. "
                     "Answers with each deleted annotation's type, page, colour, text and comment, so that one "
                     "can be made again with annotate_pdf.",
                     schema({{"keys", stringList("The annotations' keys")}}, {"keys"}), true, true, false,
                     [](const Json& a) {
                         Line l;
                         l.flag("--annotations-only");
                         l.flag("--yes");
                         l.positionals = strings(a, "keys", true);
                         return l.words("delete");
                     }, {}});
        return t;
    }();
    return all;
}

// --- running a command ---------------------------------------------------------------------------

struct Answer {
    bool ok = false;
    std::string text;
};

// The command run in this process as `ournotes-cli WORDS...` would run it, on the library
// opened afresh (the app may have changed it since), with nothing on its standard input.
Answer runCommand(CommandMap& commands, const McpOptions& options, std::vector<std::string> words)
{
    words.insert(words.begin(), "ournotes-cli");
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    Args args((int)argv.size(), argv.data());
    std::ostringstream out, err;
    std::istringstream nothing;
    auto inState = std::cin.rdstate();
    auto* oldOut = std::cout.rdbuf(out.rdbuf());
    auto* oldErr = std::cerr.rdbuf(err.rdbuf());
    auto* oldIn = std::cin.rdbuf(nothing.rdbuf());
    int code = 1;
    std::string error;
    try {
        Library lib(options.dataDir);
        lib.setOrigin(kOrigin);
        code = commands.at(args.command)(lib, args);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "failed";
    }
    std::cout.rdbuf(oldOut);
    std::cerr.rdbuf(oldErr);
    std::cin.rdbuf(oldIn);
    std::cin.clear(inState);
    Answer a;
    a.ok = code == 0 && error.empty();
    auto said = trim(err.str());
    if (a.ok) {
        a.text = out.str();
        if (!said.empty())
            a.text += "\n" + said;
    } else {
        a.text = trim(said + (said.empty() || error.empty() ? "" : "\n") + error);
        if (a.text.empty())
            a.text = "failed";
    }
    return a;
}

Json toolResult(const std::string& text, bool error)
{
    std::string shown = text;
    if (shown.size() > kMaxAnswer)
        shown = cutAt(shown, kMaxAnswer) + "\n[cut here: " + std::to_string(text.size() - kMaxAnswer) +
                " more characters]";
    Json r{{"content", Json::array({Json{{"type", "text"}, {"text", shown}}})}};
    if (error)
        r["isError"] = true;
    return r;
}

Json describe(const Tool& t)
{
    return Json{{"name", t.name},
                {"title", t.title},
                {"description", t.description},
                {"inputSchema", t.schema},
                {"annotations", Json{{"title", t.title},
                                     {"readOnlyHint", !t.writes},
                                     {"destructiveHint", t.destructive},
                                     {"idempotentHint", !t.writes},
                                     {"openWorldHint", t.online}}}};
}

// What goes to a FILE (the protocol's standard output, see registerMcpCommand).
class FileBuffer : public std::streambuf {
public:
    explicit FileBuffer(FILE* f) : f_(f) {}

protected:
    int_type overflow(int_type c) override
    {
        if (c != traits_type::eof() && std::fputc(c, f_) == EOF)
            return traits_type::eof();
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        return (std::streamsize)std::fwrite(s, 1, (size_t)n, f_);
    }
    int sync() override { return std::fflush(f_) == 0 ? 0 : -1; }

private:
    FILE* f_;
};

} // namespace

void runMcpServer(CommandMap& commands, const McpOptions& options, std::istream& in, std::ostream& out)
{
    auto send = [&](const Json& message) {
        out << message.dump(-1, ' ', false, Json::error_handler_t::replace) << "\n" << std::flush;
    };
    auto reply = [&](const Json& id, Json result) {
        send(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    };
    auto fail = [&](const Json& id, int code, const std::string& message) {
        send(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", code}, {"message", message}}}});
    };
    auto offered = [&](const Tool& t) { return options.write || !t.writes; };

    auto handle = [&](const Json& m) {
        if (!m.is_object()) {
            fail(nullptr, -32600, "Invalid request");
            return;
        }
        auto method = jstr(m, "method");
        if (method.empty() || !m.contains("id"))
            return;  // a notification (initialized, cancelled...), or an answer: nothing to say
        const Json& id = m["id"];
        Json params = m.contains("params") && m["params"].is_object() ? m["params"] : Json::object();
        try {
            if (method == "initialize") {
                std::string version = kProtocolVersions[0];
                for (auto* v : kProtocolVersions)
                    if (jstr(params, "protocolVersion") == v)
                        version = v;
                reply(id, Json{{"protocolVersion", version},
                               {"capabilities", Json{{"tools", Json{{"listChanged", false}}}}},
                               {"serverInfo", Json{{"name", "ournotes"}, {"title", "OurNotes"}, {"version", OURNOTES_VERSION}}},
                               {"instructions", kInstructions}});
            } else if (method == "ping") {
                reply(id, Json::object());
            } else if (method == "tools/list") {
                Json list = Json::array();
                for (auto& t : tools())
                    if (offered(t))
                        list.push_back(describe(t));
                reply(id, Json{{"tools", list}});
            } else if (method == "tools/call") {
                auto name = jstr(params, "name");
                const Tool* tool = nullptr;
                for (auto& t : tools())
                    if (t.name == name && offered(t))
                        tool = &t;
                if (!tool) {
                    bool writing = false;
                    for (auto& t : tools())
                        writing = writing || (t.name == name && t.writes);
                    fail(id, -32602, writing ? "The tool " + name + " changes the library: start the server with "
                                                                    "`ournotes-cli mcp --write` to offer it"
                                             : "Unknown tool: " + name);
                    return;
                }
                Json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"]
                                                                                            : Json::object();
                std::vector<std::string> words;
                try {
                    words = tool->line(args);
                } catch (const ToolError& e) {
                    reply(id, toolResult(e.what(), true));
                    return;
                }
                auto answer = runCommand(commands, options, words);
                if (answer.ok && tool->answer) {
                    try {
                        answer.text = tool->answer(answer.text, args);
                    } catch (const std::exception& e) {
                        answer = {false, std::string("unexpected output: ") + e.what()};
                    }
                }
                reply(id, toolResult(answer.text, !answer.ok));
            } else if (method == "resources/list") {
                reply(id, Json{{"resources", Json::array()}});
            } else if (method == "resources/templates/list") {
                reply(id, Json{{"resourceTemplates", Json::array()}});
            } else if (method == "prompts/list") {
                reply(id, Json{{"prompts", Json::array()}});
            } else {
                fail(id, -32601, "Method not found: " + method);
            }
        } catch (const std::exception& e) {
            fail(id, -32603, e.what());
        }
    };

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (trim(line).empty())
            continue;
        Json message;
        try {
            message = Json::parse(line);
        } catch (const std::exception&) {
            fail(nullptr, -32700, "Parse error");
            continue;
        }
        if (message.is_array()) {  // (a batch, from an older client)
            for (auto& m : message)
                handle(m);
        } else {
            handle(message);
        }
    }
}

void registerMcpCommand(CommandMap& commands)
{
    commands["mcp"] = [&commands](Library& lib, Args& args) {
        McpOptions options;
        options.dataDir = lib.dataDir();
        options.write = args.flag("--write");
        // Standard output is the protocol's alone: whatever else would write there (a
        // library's message) goes to standard error instead.
        std::cout.flush();
        std::fflush(stdout);
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        int fd = _dup(_fileno(stdout));
        if (fd >= 0)
            _dup2(_fileno(stderr), _fileno(stdout));
        FILE* protocol = fd >= 0 ? _fdopen(fd, "wb") : nullptr;
#else
        int fd = dup(STDOUT_FILENO);
        if (fd >= 0)
            dup2(STDERR_FILENO, STDOUT_FILENO);
        FILE* protocol = fd >= 0 ? fdopen(fd, "w") : nullptr;
#endif
        if (!protocol)
            throw Error("cannot write to standard output");
        std::cerr << "ournotes-cli mcp " << OURNOTES_VERSION << ": the library in " << pathString(options.dataDir)
                  << (options.write ? ", to read and change" : ", to read (--write to change it too)") << std::endl;
        FileBuffer buffer(protocol);
        std::ostream out(&buffer);
        runMcpServer(commands, options, std::cin, out);
        std::fclose(protocol);
        return 0;
    };
}
