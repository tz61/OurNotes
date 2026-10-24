// ournotes-cli commands for sync targets, synchronisation and full-text indexing.

#include <iostream>

#include "cli/commands.h"
#include "core/fulltext.h"
#include "core/hash.h"
#include "core/schema.h"
#include "core/sync/engine.h"
#include "core/sync/history.h"
#include "formats/citation.h"
#include "formats/formats.h"
#include "formats/metadata.h"
#include "formats/pdf_metadata.h"
#include "pdf/document.h"
#include "sync/http_transport.h"
#include "sync/local_transport.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace on;

namespace {

std::string readPassword(const std::string& prompt)
{
    std::cerr << prompt << std::flush;
    std::string pw;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
    std::getline(std::cin, pw);
    SetConsoleMode(h, mode);
#else
    termios old{};
    bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios noecho = old;
        noecho.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    }
    std::getline(std::cin, pw);
    if (tty)
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
#endif
    std::cerr << "\n";
    return pw;
}

std::unique_ptr<sync::Transport> makeTransport(const SyncTarget& t)
{
    if (t.kind == "local")
        return std::make_unique<sync::LocalTransport>(u8path(t.path));
    return std::make_unique<sync::HttpTransport>(t.url, t.token, t.verifyTls);
}

// Top-level regular items of a library (optionally one collection), with child notes
// and attachment paths for exporters.
std::vector<Json> collectItems(Library& lib, int64_t id, const std::string& collection, const std::vector<std::string>& keys)
{
    std::set<std::string> allowed;
    if (!collection.empty())
        allowed = lib.collectionItemKeys(id, collection, true);
    std::vector<std::string> selected = keys;
    if (selected.empty())
        for (auto& r : lib.itemRows(id))
            if (r.parentKey.empty() && !r.trashed && (collection.empty() || allowed.count(r.key)))
                selected.push_back(r.key);
    std::vector<Json> out;
    for (auto& d : lib.items(id, selected)) {
        if (!schema::isRegularType(jstr(d, "itemType")))
            continue;
        Json item = d;
        Json notes = Json::array(), atts = Json::array();
        for (auto& c : lib.children(id, jstr(d, "key"))) {
            if (jstr(c, "itemType") == "note")
                notes.push_back(jstr(c, "note"));
            else if (jstr(c, "itemType") == "attachment")
                atts.push_back(Json{{"title", jstr(c, "title")},
                                    {"path", pathString(lib.attachmentFile(id, c))},
                                    {"contentType", jstr(c, "contentType")},
                                    {"url", jstr(c, "url")}});
        }
        if (!notes.empty())
            item["_notes"] = notes;
        if (!atts.empty())
            item["_attachments"] = atts;
        out.push_back(item);
    }
    return out;
}

Json addItem(Library& lib, int64_t id, Json data, const std::string& collection)
{
    Json notes = data.value("_notes", Json::array());
    for (const char* k : {"_notes", "_files", "_attachments"})
        data.erase(k);
    if (!collection.empty())
        data["collections"] = Json::array({collection});
    auto item = lib.createItem(id, data);
    for (auto& n : notes)
        if (n.is_string())
            lib.createNote(id, n.get<std::string>(), jstr(item, "key"));
    return item;
}

// A PDF of its own filed under an item made from the metadata found for it, as the app's
// Retrieve Metadata does: the item takes the PDF's collections, the PDF is its "PDF".
std::string fileUnderNewItem(Library& lib, int64_t id, const std::string& pdfKey, const Json& found)
{
    auto att = lib.item(id, pdfKey);
    auto collections = att ? att->value("collections", Json::array()) : Json::array();
    Library::BatchGuard batch(lib);
    auto item = addItem(lib, id, found, {});
    if (!collections.empty())
        lib.updateItem(id, jstr(item, "key"), [&](Json& d) { d["collections"] = collections; });
    lib.updateItem(id, pdfKey, [&](Json& d) {
        d["parentItem"] = jstr(item, "key");
        d["title"] = "PDF";
        d.erase("collections");
    });
    return jstr(item, "key");
}

// arXiv's PDF of an item found by its arXiv ID, attached to it (as the app's Add by
// Identifier does). Quietly nothing when it cannot be had.
void attachArxivPdf(Library& lib, int64_t id, const formats::Identifier& ident, const Json& item)
{
    if (ident.kind != "arxiv" && jstr(item, "repository") != "arXiv")
        return;
    auto archive = jstr(item, "archiveID");
    auto arxivId = startsWith(archive, "arXiv:") ? archive.substr(6) : ident.value;
    auto dir = fs::temp_directory_path() / ("ournotes-" + randomToken(6));
    std::error_code ec;
    try {
        fs::create_directories(dir);
        auto dest = dir / (safeFilename(arxivId) + ".pdf");
        formats::downloadFile(formats::arxivPdfUrl(arxivId), dest, true);
        lib.importAttachment(id, dest, jstr(item, "key"));
    } catch (const std::exception& e) {
        std::cerr << arxivId << ": no PDF (" << e.what() << ")\n";
    }
    fs::remove_all(dir, ec);
}

// What the app's Preferences set, by a name of their own here.
struct Preference {
    std::string name;
    std::string pointer;  // in the settings file
    std::string kind;     // bool, int, choice, color, color-or-none, colors, style, folder
    Json def;
    std::vector<std::string> choices;
    int min = 0, max = 0;
    std::string about;
};

const std::vector<Preference>& preferences()
{
    static const std::vector<Preference> list = {
        {"data-dir", "/dataDir", "folder", "", {}, 0, 0, "where the library is: used from the next start, and the data is not moved"},
        {"scale", "/ui/scale", "int", 150, {}, 50, 300, "the interface scale in percent, from the next start"},
        {"appearance", "/ui/appearance", "choice", "system", {"system", "light", "dark"}, 0, 0, "light or dark, or as the system is"},
        {"dark-pages", "/reader/darkPages", "bool", true, {}, 0, 0, "PDF pages in dark colours in dark mode"},
        {"dark-pages-in-light-mode", "/reader/darkPagesInLightMode", "bool", false, {}, 0, 0, "PDF pages in dark colours in light mode"},
        {"page-tone-light", "/reader/pageTone/light", "int", 0, {}, 0, 100, "light pages tinted: 0 white .. 100 gray"},
        {"page-tone-dark", "/reader/pageTone/dark", "int", 0, {}, 0, 100, "dark pages tinted: 0 black .. 100 gray"},
        {"background-picture", "/ui/bandBackground", "bool", true, {}, 0, 0, "a picture behind the item list"},
        {"formula-color", "/notes/formulaColor", "color-or-none", "", {}, 0, 0, "the colour of formulas in notes; none: the text's"},
        {"note-zoom", "/notes/zoom", "int", 100, {}, 50, 300, "the text size of notes, in percent"},
        {"open-pdf", "/ui/openPdfIn", "choice", "tab", {"tab", "external"}, 0, 0, "PDFs open in OurNotes, or in the system's viewer"},
        {"subcollection-items", "/ui/showSubcollectionItems", "bool", false, {}, 0, 0, "a collection lists its subcollections' items too"},
        {"fulltext", "/fulltext/enabled", "bool", true, {}, 0, 0, "PDFs' text indexed for full-text search"},
        {"citation-style", "/ui/citationStyle", "style", "apa", {}, 0, 0, "the style of Copy Citation and bibliographies"},
        {"annotation-color", "/reader/color", "color", "#ffd400", {}, 0, 0, "the colour new annotations get"},
        {"palette", "/reader/palette", "colors", Json::array({"#ffd400", "#ff6666", "#5fb236"}), {}, 0, 0, "the colours of Q, W and E on a page"},
        {"sync-auto", "/sync/auto", "bool", true, {}, 0, 0, "sync after changes, here or on another computer"},
        {"sync-interval", "/sync/intervalMinutes", "int", 15, {}, 0, 1440, "also sync every so many minutes; 0: only after changes"},
        {"sync-files", "/sync/files", "choice", "on_sync", {"on_sync", "on_demand"}, 0, 0, "attachment files downloaded during sync, or when opened"},
        {"connector", "/connector/enabled", "bool", true, {}, 0, 0, "the browser extension may save pages"},
    };
    return list;
}

std::string preferenceText(const Json& v)
{
    if (v.is_string())
        return v.get<std::string>().empty() ? "none" : v.get<std::string>();
    if (v.is_array()) {
        std::string out;
        for (auto& c : v)
            out += (out.empty() ? "" : ",") + (c.is_string() ? c.get<std::string>() : c.dump());
        return out;
    }
    return v.dump();
}

Json preferenceValue(const Preference& p, const std::string& text)
{
    auto choices = [&](const std::vector<std::string>& list) {
        std::string out;
        for (auto& c : list)
            out += (out.empty() ? "" : ", ") + c;
        return out;
    };
    if (p.kind == "bool") {
        auto t = toLower(text);
        if (t == "true" || t == "on" || t == "yes" || t == "1")
            return true;
        if (t == "false" || t == "off" || t == "no" || t == "0")
            return false;
        throw Error(p.name + " is on or off");
    }
    if (p.kind == "int") {
        int v = 0;
        try {
            size_t used = 0;
            v = std::stoi(text, &used);
            if (used != text.size())
                throw Error("");
        } catch (const std::exception&) {
            throw Error(p.name + " is a whole number from " + std::to_string(p.min) + " to " + std::to_string(p.max));
        }
        if (v < p.min || v > p.max)
            throw Error(p.name + " is from " + std::to_string(p.min) + " to " + std::to_string(p.max));
        return v;
    }
    if (p.kind == "choice") {
        if (std::find(p.choices.begin(), p.choices.end(), text) == p.choices.end())
            throw Error(p.name + " is one of: " + choices(p.choices));
        return text;
    }
    if (p.kind == "color")
        return annotationColorValue(text);
    if (p.kind == "color-or-none")
        return toLower(text) == "none" || text.empty() ? std::string() : annotationColorValue(text);
    if (p.kind == "colors") {
        auto list = split(text, ',');
        if (list.size() != 3)
            throw Error(p.name + " is three colours, apart by commas (yellow,red,green or #rrggbb)");
        Json out = Json::array();
        for (auto& c : list)
            out.push_back(annotationColorValue(trim(c)));
        return out;
    }
    if (p.kind == "style") {
        std::vector<std::string> ids;
        for (auto& st : formats::builtinStyles())
            ids.push_back(st.id);
        if (std::find(ids.begin(), ids.end(), text) == ids.end())
            throw Error(p.name + " is one of: " + choices(ids));
        return text;
    }
    // a folder
    std::error_code ec;
    auto dir = fs::absolute(u8path(text), ec);
    if (!fs::is_directory(dir, ec))
        throw Error("no folder " + text);
    return pathString(dir);
}

} // namespace

void registerExtraCommands(CommandMap& commands, Config& config)
{
    commands["export"] = [](Library& lib, Args& args) {
        auto* f = formats::findFormat(args.option("--format", "bibtex"));
        if (!f) {
            std::cerr << "Unknown format; use bibtex, ris or csljson\n";
            return 1;
        }
        auto collection = args.libraryAndCollection(lib).second;
        auto out = f->exporter(collectItems(lib, args.itemsLibrary(lib), collection, args.positionals));
        auto file = args.option("-o");
        if (file.empty())
            std::cout << out;
        else
            writeFileAtomic(u8path(file), out);
        return 0;
    };
    commands["import"] = [](Library& lib, Args& args) {
        auto path = u8path(args.positional(0));
        auto text = readFile(path);
        auto* f = formats::findFormat(args.option("--format", formats::detectFormat(text)));
        if (!f) {
            std::cerr << "Unrecognised file format\n";
            return 1;
        }
        auto [id, collection] = args.libraryAndCollection(lib);
        requireEditable(lib, id);
        int n = 0;
        Library::BatchGuard batch(lib);
        for (auto& d : f->importer(text)) {
            auto files = d.value("_files", Json::array());
            auto item = addItem(lib, id, d, collection);
            for (auto& p : files) {
                auto fp = u8path(p.get<std::string>());
                if (fp.is_relative())
                    fp = path.parent_path() / fp;
                std::error_code ec;
                if (fs::is_regular_file(fp, ec))
                    lib.importAttachment(id, fp, jstr(item, "key"));
            }
            ++n;
        }
        std::cout << "Imported " << n << " items\n";
        return 0;
    };
    commands["cite"] = [](Library& lib, Args& args) {
        auto collection = args.libraryAndCollection(lib).second;
        auto items = collectItems(lib, args.itemsLibrary(lib), collection, args.positionals);
        auto style = args.option("--style", "apa");
        auto fmt = args.flag("--html") ? formats::OutputFormat::Html : formats::OutputFormat::Text;
        std::cout << (args.flag("--in-text") ? formats::formatCitation(items, style, fmt)
                                             : formats::formatBibliography(items, style, fmt))
                  << "\n";
        return 0;
    };
    commands["add"] = [](Library& lib, Args& args) {
        auto [id, collection] = args.libraryAndCollection(lib);
        requireEditable(lib, id);
        int failures = 0;
        for (auto& arg : args.positionals) {
            auto path = u8path(arg);
            std::error_code ec;
            if (fs::is_regular_file(path, ec)) {
                auto att = lib.importAttachment(id, path);
                if (!collection.empty())
                    lib.addToCollection(id, collection, {jstr(att, "key")});
                std::optional<formats::PdfMetadataMatch> found;
                if (jstr(att, "contentType") == "application/pdf") {
                    try {
                        found = formats::retrieveMetadataForPdf(path);
                    } catch (const std::exception& e) {
                        std::cerr << "metadata lookup failed: " << e.what() << "\n";
                    }
                }
                if (found) {
                    auto item = fileUnderNewItem(lib, id, jstr(att, "key"), found->item);
                    std::cout << item << "  " << jstr(*lib.item(id, item), "title") << "  [" << found->source << "]\n";
                } else {
                    std::cout << jstr(att, "key") << "  " << arg << " (no metadata found)\n";
                }
                continue;
            }
            auto ids = formats::parseIdentifiers(arg);
            if (ids.empty()) {
                std::cerr << "not a file or identifier: " << arg << "\n";
                ++failures;
                continue;
            }
            for (auto& ident : ids) {
                try {
                    auto item = addItem(lib, id, formats::lookup(ident), collection);
                    if (!args.flag("--no-pdf"))
                        attachArxivPdf(lib, id, ident, item);
                    std::cout << jstr(item, "key") << "  " << jstr(item, "title") << "\n";
                } catch (const std::exception& e) {
                    std::cerr << ident.value << ": " << e.what() << "\n";
                    ++failures;
                }
            }
        }
        return failures ? 2 : 0;
    };
    commands["targets"] = [&config](Library&, Args&) {
        auto active = config.activeTarget();
        for (auto& t : config.targets())
            std::cout << (active && active->id == t.id ? "* " : "  ") << t.id << "  " << t.describe() << "\n";
        if (config.targets().empty())
            std::cout << "No sync targets. Add one with add-server or add-folder.\n";
        return 0;
    };
    commands["add-server"] = [&config](Library&, Args& args) {
        auto url = args.positional(0);
        auto user = args.option("--user");
        if (url.empty() || user.empty()) {
            std::cerr << "usage: ournotes-cli add-server URL --user NAME [--password PW] [--insecure]\n";
            return 1;
        }
        if (url.find("://") == std::string::npos)
            url = "https://" + url;
        auto pw = args.option("--password");
        if (pw.empty())
            pw = readPassword("Password for " + user + ": ");
        bool verify = !args.flag("--insecure");
        auto token = sync::HttpTransport::login(url, user, pw, "ournotes-cli", verify);
        SyncTarget t;
        t.id = randomToken(4);
        t.kind = "remote";
        t.name = args.option("--name");
        if (t.name.empty()) {  // the server's host name, as the app names it
            auto host = url.substr(url.find("://") + 3);
            t.name = host.substr(0, host.find_first_of(":/"));
        }
        t.url = url;
        t.username = user;
        t.token = token;
        t.verifyTls = verify;
        config.upsertTarget(t);
        if (!config.get("/sync/active").is_string())
            config.setActiveTarget(t.id);
        config.save();
        std::cout << "Added " << t.describe() << " as " << t.id << "\n";
        return 0;
    };
    commands["add-folder"] = [&config](Library&, Args& args) {
        auto path = args.positional(0);
        if (path.empty()) {
            std::cerr << "usage: ournotes-cli add-folder PATH\n";
            return 1;
        }
        SyncTarget t;
        t.id = randomToken(4);
        t.kind = "local";
        t.path = pathString(fs::absolute(u8path(path)));
        t.name = args.option("--name").empty() ? pathString(u8path(t.path).filename()) : args.option("--name");
        config.upsertTarget(t);
        if (!config.get("/sync/active").is_string())
            config.setActiveTarget(t.id);
        config.save();
        std::cout << "Added " << t.describe() << " as " << t.id << "\n";
        return 0;
    };
    commands["use-target"] = [&config](Library&, Args& args) {
        if (!config.target(args.positional(0))) {
            std::cerr << "No such target\n";
            return 1;
        }
        config.setActiveTarget(args.positional(0));
        config.save();
        return 0;
    };
    commands["remove-target"] = [&config](Library&, Args& args) {
        config.removeTarget(args.positional(0));
        config.save();
        return 0;
    };
    commands["sync"] = [&config](Library& lib, Args& args) {
        auto id = args.option("--target");
        auto target = id.empty() ? config.activeTarget() : config.target(id);
        if (!target) {
            std::cerr << "No sync target configured (see add-server / add-folder).\n";
            return 1;
        }
        // in the app's Sync History too, as made by ournotes-cli
        sync::SyncHistory history(lib.dataDir() / "sync-history.json", "ournotes-cli");
        sync::SyncReport report;
        try {
            auto transport = makeTransport(*target);
            sync::SyncOptions opts;
            opts.syncFiles = !args.flag("--no-files");
            opts.log = [](const std::string& m) { std::cout << m << "\n"; };
            sync::SyncEngine engine(lib, *transport, target->id, opts);
            report = engine.syncAll();
        } catch (const std::exception& e) {
            history.addFailure(target->name, e.what());
            throw;
        }
        auto summary = report.summary();
        history.add(target->name, report, report.errors.empty(), summary.substr(0, summary.find('\n')));
        std::cout << "Sync with " << target->describe() << ": " << summary << "\n";
        for (auto& e : report.errors)
            std::cerr << "error: " << e << "\n";
        return report.errors.empty() ? 0 : 2;
    };
    commands["index"] = [](Library& lib, Args& args) {
        auto& index = lib.fulltext();
        if (args.flag("--rebuild"))
            index.clear();
        int indexed = 0, skipped = 0;
        for (auto& info : lib.libraries())
            for (auto& d : lib.allItems(info.id, true)) {
                if (jstr(d, "itemType") != "attachment" || jstr(d, "contentType") != "application/pdf")
                    continue;
                auto path = lib.attachmentFile(info.id, d);
                std::error_code ec;
                if (path.empty() || !fs::is_regular_file(path, ec))
                    continue;
                auto key = jstr(d, "key");
                auto md5 = lib.fileMd5(info.id, key, path);
                if (index.isIndexed(info.id, key, md5)) {
                    ++skipped;
                    continue;
                }
                try {
                    auto doc = pdf::Document::open(path);
                    index.store(info.id, key, md5, doc->fullText(), doc->pageCount());
                    ++indexed;
                    std::cout << "indexed " << pathString(path.filename()) << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "skipped " << pathString(path.filename()) << ": " << e.what() << "\n";
                }
            }
        std::cout << indexed << " documents indexed, " << skipped << " already up to date\n";
        return 0;
    };
    commands["retrieve-metadata"] = [](Library& lib, Args& args) {
        std::vector<std::pair<int64_t, Json>> pdfs;
        for (auto& key : keysFrom(args)) {
            auto [id, att] = findItem(lib, key, libraryOption(lib, args));
            if (jstr(att, "itemType") != "attachment" || jstr(att, "contentType") != "application/pdf" ||
                !jstr(att, "parentItem").empty())
                throw Error(key + " is not a PDF of its own: details are looked up for a PDF with no item over it");
            requireFile(lib, id, att);
            requireEditable(lib, id);
            pdfs.push_back({id, att});
        }
        int failures = 0;
        for (auto& [id, att] : pdfs) {
            auto key = jstr(att, "key");
            try {
                auto found = formats::retrieveMetadataForPdf(lib.attachmentFile(id, att));
                if (!found) {
                    std::cerr << key << ": no details found\n";
                    ++failures;
                    continue;
                }
                auto item = fileUnderNewItem(lib, id, key, found->item);
                std::cout << item << "  " << jstr(*lib.item(id, item), "title") << "  [" << found->source << "]\n";
            } catch (const std::exception& e) {
                std::cerr << key << ": " << e.what() << "\n";
                ++failures;
            }
        }
        return failures ? 2 : 0;
    };
    commands["delete-library"] = [&config](Library& lib, Args& args) {
        requirePositionals(args, 1);
        auto id = findLibrary(lib, args.positional(0));
        auto info = *lib.library(id);
        if (lib.libraries().size() <= 1)
            throw Error("\"" + info.name + "\" is the only library, and OurNotes needs one");
        auto contents = lib.libraryContents(id);
        if (!contents.empty())
            throw Error("\"" + info.name + "\" still has " +
                        (contents.items || contents.collections
                             ? std::to_string(contents.items) + " item(s) and " + std::to_string(contents.collections) + " collection(s)"
                             : std::to_string(contents.other) + " entries without an item") +
                        ", counting its trash: only an empty library is deleted (delete its collections, trash its "
                        "items and empty the trash first)");
        auto target = config.activeTarget();
        bool remote = target && info.syncTarget == target->id;
        requireYes(args, remote ? "this deletes the library here and on the sync target \"" + target->name +
                                      "\" (and on your other computers when they next sync)"
                                : "this deletes the library");
        if (remote) {
            auto transport = makeTransport(*target);
            sync::SyncEngine(lib, *transport, target->id).deleteRemoteLibrary(info.uid);
            if (!lib.libraryContents(id).empty())
                throw Error("the library got new entries meanwhile, so it was kept");
        }
        lib.deleteLibrary(id);
        std::cout << id << "\n";
        return 0;
    };
    commands["storage"] = [&config](Library& lib, Args& args) {
        requirePositionals(args, 0);
        auto usage = lib.diskUsage();
        Json remote;
        if (args.flag("--remote")) {
            auto target = config.activeTarget();
            if (!target)
                throw Error("no sync target is set up (see add-server, add-folder)");
            auto transport = makeTransport(*target);
            if (auto s = transport->status())
                remote = *s;
            else
                remote = transport->info();
            remote["target"] = target->describe();
        }
        if (args.flag("--json")) {
            if (!remote.is_null())
                usage["remote"] = remote;
            std::cout << usage.dump(2) << "\n";
            return 0;
        }
        auto size = [](const Json& v) { return humanSize(v.is_number() ? v.get<uint64_t>() : 0); };
        std::cout << "Data folder:       " << jstr(usage, "dataDir") << "\n"
                  << "Library database:  " << size(usage["database"]) << "\n"
                  << "Full-text index:   " << size(usage["fulltextIndex"]) << "\n"
                  << "Files (storage/):  " << usage["storage"].value("files", 0) << " files, " << size(usage["storage"]["bytes"]) << "\n";
        if (auto disk = usage["disk"]; disk.is_object())
            std::cout << "Disk:              " << size(disk["available"]) << " free of " << size(disk["capacity"]) << "\n";
        for (auto& l : usage["libraries"]) {
            std::cout << "\n" << jstr(l, "name") << ": " << l.value("items", 0) << " items, " << l.value("notes", 0)
                      << " notes, " << l.value("attachments", 0) << " attachments, " << l.value("annotations", 0)
                      << " annotations, " << l.value("trashed", 0) << " in the trash\n  files: " << l.value("files", 0)
                      << ", " << size(l["fileBytes"]);
            if (l.value("missingFiles", 0) > 0)
                std::cout << "; not on this computer: " << l.value("missingFiles", 0);
            std::cout << "\n";
        }
        if (!remote.is_null())
            std::cout << "\nSync target:\n" << remote.dump(2) << "\n";
        return 0;
    };
    commands["sync-history"] = [](Library& lib, Args& args) {
        requirePositionals(args, 0);
        sync::SyncHistory history(lib.dataDir() / "sync-history.json");
        int limit = (int)numberOption(args, "--limit", 10, 1, 1000);
        Json out = Json::array();
        for (auto& run : history.runs()) {
            if ((int)out.size() == limit)
                break;
            out.push_back(run);
        }
        if (args.flag("--json")) {
            std::cout << out.dump(2) << "\n";
            return 0;
        }
        for (auto& run : out) {
            std::cout << isoToLocalDisplay(jstr(run, "time")) << "  " << jstr(run, "target")
                      << (run.value("ok", true) ? "" : "  FAILED") << (jstr(run, "by").empty() ? "" : "  (by " + jstr(run, "by") + ")")
                      << "\n";
            if (!jstr(run, "summary").empty())
                std::cout << "  " << jstr(run, "summary") << "\n";
            for (auto& e : run.value("errors", Json::array()))
                std::cout << "  error: " << (e.is_string() ? e.get<std::string>() : e.dump()) << "\n";
            for (auto& c : run.value("changes", Json::array()))
                std::cout << "  " << (c.value("incoming", false) ? "in " : "out") << "  " << jstr(c, "action") << " "
                          << jstr(c, "type") << " " << jstr(c, "key") << "  " << jstr(c, "title")
                          << (jstr(c, "by").empty() ? "" : "  (by " + jstr(c, "by") + ")") << "\n";
            if (run.value("more", 0) > 0)
                std::cout << "  and " << run.value("more", 0) << " more\n";
        }
        return 0;
    };
    commands["conflicts"] = [](Library& lib, Args& args) {
        requirePositionals(args, 0);
        auto list = lib.conflicts();
        if (args.flag("--clear")) {
            for (auto& c : list)
                lib.deleteConflict(c.id);
            std::cout << list.size() << " cleared\n";
            return 0;
        }
        auto describe = [](const Json& d) {
            if (d.is_null())
                return std::string("(deleted)");
            for (auto& t : {jstr(d, "title"), noteTitle(jstr(d, "note")), jstr(d, "name"), jstr(d, "annotationText")})
                if (!t.empty())
                    return t;
            return d.dump().substr(0, 200);
        };
        Json out = Json::array();
        for (auto& c : list) {
            if (args.flag("--json")) {
                out.push_back(Json{{"id", c.id}, {"libraryId", c.libraryId}, {"type", c.type}, {"key", c.key},
                                   {"kept", c.kept}, {"time", c.created}, {"local", c.local}, {"remote", c.remote}});
                continue;
            }
            std::cout << c.type << " " << c.key << "  " << isoToLocalDisplay(c.created) << "\n  this computer ("
                      << (c.kept == "local" ? "kept" : "replaced") << "): " << describe(c.local) << "\n  other device ("
                      << (c.kept == "remote" ? "kept" : "replaced") << "): " << describe(c.remote) << "\n";
        }
        if (args.flag("--json"))
            std::cout << out.dump(2) << "\n";
        return 0;
    };
    commands["config"] = [&config](Library&, Args& args) {
        requirePositionals(args, 2);
        auto& prefs = preferences();
        auto value = [&](const Preference& p) {  // as the app takes it
            auto v = config.get(p.pointer, p.def);
            if (p.pointer == "/dataDir")
                return Json(pathString(config.dataDir()));
            return v.is_null() ? p.def : v;
        };
        auto name = args.positional(0);
        if (name.empty()) {
            Json out = Json::array();
            for (auto& p : prefs) {
                auto v = value(p);
                if (args.flag("--json"))
                    out.push_back(Json{{"name", p.name}, {"value", v}, {"default", p.def}, {"setting", p.pointer}, {"about", p.about}});
                else
                    std::cout << p.name << " = " << preferenceText(v) << "    -- " << p.about << "\n";
            }
            if (args.flag("--json"))
                std::cout << out.dump(2) << "\n";
            return 0;
        }
        auto it = std::find_if(prefs.begin(), prefs.end(), [&](const Preference& p) { return p.name == name; });
        if (it == prefs.end()) {
            std::string names;
            for (auto& p : prefs)
                names += (names.empty() ? "" : ", ") + p.name;
            throw Error("no setting \"" + name + "\"; there are: " + names);
        }
        if (args.positionals.size() < 2) {
            std::cout << preferenceText(value(*it)) << "\n";
            return 0;
        }
        auto given = preferenceValue(*it, args.positional(1));
        if (it->pointer == "/dataDir")
            config.setDataDir(u8path(given.get<std::string>()));
        else
            config.set(it->pointer, given);
        config.save();
        std::cout << it->name << " = " << preferenceText(given) << "\n";
        return 0;
    };
}
