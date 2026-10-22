// ournotes-cli commands for sync targets, synchronisation and full-text indexing.

#include <iostream>

#include "cli/commands.h"
#include "core/fulltext.h"
#include "core/hash.h"
#include "core/schema.h"
#include "core/sync/engine.h"
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
                    auto item = addItem(lib, id, found->item, collection);
                    lib.updateItem(id, jstr(att, "key"), [&](Json& d) {
                        d["parentItem"] = jstr(item, "key");
                        d["title"] = "PDF";
                        d.erase("collections");
                    });
                    std::cout << jstr(item, "key") << "  " << jstr(item, "title") << "  [" << found->source << "]\n";
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
        t.name = url;
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
        t.name = pathString(u8path(t.path).filename());
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
        auto transport = makeTransport(*target);
        sync::SyncOptions opts;
        opts.syncFiles = !args.flag("--no-files");
        opts.log = [](const std::string& m) { std::cout << m << "\n"; };
        sync::SyncEngine engine(lib, *transport, target->id, opts);
        auto report = engine.syncAll();
        std::cout << "Sync with " << target->describe() << ": " << report.summary() << "\n";
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
}
