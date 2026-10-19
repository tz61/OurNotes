// ournotes-cli: command-line access to an OurNotes library.

#include <cstdio>
#include <functional>
#include <iostream>
#include <map>

#include "core/config.h"
#include "core/library.h"
#include "core/schema.h"
#include "core/zotero_importer.h"
#include "ournotes/version.h"

#include "cli/commands.h"

#ifdef _WIN32
#include <io.h>
#define FP_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define FP_ISATTY(fd) isatty(fd)
#endif

using namespace on;

namespace {

void usage()
{
    std::cout << R"(ournotes-cli )" OURNOTES_VERSION R"( - command-line interface to an OurNotes library

Usage: ournotes-cli [--data-dir DIR] <command> [options]

Library:
  info                              data directory and library statistics
  libraries                         list libraries
  collections [--library ID]        list collections as a tree
  items [--library ID] [--search TEXT] [--mode quick|all|everything] [--collection KEY]
  show KEY [--library ID]           print an item as JSON
  import-zotero [ZOTERO_DIR] [--hardlink | --no-files] [--no-trash]
                                    import (or update from) a Zotero data directory
)" OURNOTES_CLI_EXTRA_USAGE R"(
The data directory defaults to the one configured in the desktop app
(~/OurNotes unless changed); OURNOTES_DATA_DIR overrides it.
)";
}

void printCollections(Library& lib, int64_t id, const std::string& parent, int depth)
{
    for (auto& c : lib.collections(id)) {
        auto p = c.value("parentCollection", Json());
        std::string pk = p.is_string() ? p.get<std::string>() : "";
        if (pk != parent)
            continue;
        auto key = jstr(c, "key");
        std::cout << std::string(depth * 2, ' ') << jstr(c, "name") << "  [" << key << "] ("
                  << lib.collectionItemKeys(id, key).size() << ")\n";
        printCollections(lib, id, key, depth + 1);
    }
}

int cmdInfo(Library& lib, Args&)
{
    std::cout << "Data directory: " << pathString(lib.dataDir()) << "\n";
    for (auto& l : lib.libraries()) {
        auto rows = lib.itemRows(l.id);
        int regular = 0, notes = 0, attachments = 0, trashed = 0;
        for (auto& r : rows) {
            if (r.trashed)
                ++trashed;
            if (r.itemType == "note")
                ++notes;
            else if (r.itemType == "attachment")
                ++attachments;
            else
                ++regular;
        }
        int annotations = (int)lib.db()
                              .scalarInt("SELECT COUNT(*) FROM items WHERE library_id=? AND item_type='annotation'", l.id)
                              .value_or(0);
        std::cout << "\n[" << l.id << "] " << l.name << " (" << l.type << ", uid " << l.uid << ")\n"
                  << "  items: " << regular << ", notes: " << notes << ", attachments: " << attachments
                  << ", annotations: " << annotations << ", in trash: " << trashed << "\n"
                  << "  collections: " << lib.collections(l.id).size() << ", tags: " << lib.tags(l.id).size() << "\n";
        if (!l.syncTarget.empty())
            std::cout << "  synced with target " << l.syncTarget << " at version " << l.syncVersion << ", "
                      << lib.dirtyObjects(l.id).size() << " local changes pending\n";
    }
    return 0;
}

int cmdLibraries(Library& lib, Args&)
{
    for (auto& l : lib.libraries())
        std::cout << l.id << "\t" << l.type << "\t" << l.name << "\t" << l.uid << "\n";
    return 0;
}

int cmdCollections(Library& lib, Args& args)
{
    printCollections(lib, args.libraryId(lib), "", 0);
    return 0;
}

int cmdItems(Library& lib, Args& args)
{
    auto id = args.libraryId(lib);
    auto search = args.option("--search");
    auto mode = args.option("--mode", "all");
    auto collection = args.option("--collection");
    std::set<std::string> filter;
    bool filtered = false;
    if (!search.empty()) {
        auto m = mode == "quick" ? SearchMode::TitleCreatorYear
                 : mode == "everything" ? SearchMode::Everything
                                        : SearchMode::AllFields;
        for (auto& k : lib.search(id, search, m))
            filter.insert(lib.topLevelKey(id, k));
        filtered = true;
    }
    std::set<std::string> inCollection;
    if (!collection.empty())
        inCollection = lib.collectionItemKeys(id, collection, true);
    for (auto& r : lib.itemRows(id)) {
        if (!r.parentKey.empty() || r.trashed)
            continue;
        if (filtered && !filter.count(r.key))
            continue;
        if (!collection.empty() && !inCollection.count(r.key))
            continue;
        std::cout << r.key << "  " << (r.year.empty() ? "    " : r.year) << "  " << r.creator
                  << (r.creator.empty() ? "" : " - ") << r.title << "\n";
    }
    return 0;
}

int cmdShow(Library& lib, Args& args)
{
    auto key = args.positional(0);
    auto id = args.libraryId(lib);
    auto item = lib.item(id, key);
    if (!item) {
        std::cerr << "No item " << key << "\n";
        return 1;
    }
    std::cout << item->dump(2) << "\n";
    for (auto& c : lib.children(id, key))
        std::cout << "child " << jstr(c, "itemType") << " " << jstr(c, "key") << ": "
                  << (jstr(c, "itemType") == "note" ? noteTitle(jstr(c, "note")) : jstr(c, "title")) << "\n";
    return 0;
}

int cmdImportZotero(Library& lib, Args& args)
{
    ZoteroImportOptions opt;
    opt.zoteroDir = args.positional(0).empty() ? findZoteroDataDir() : u8path(args.positional(0));
    if (args.flag("--hardlink"))
        opt.fileMode = ZoteroFileMode::Hardlink;
    if (args.flag("--no-files"))
        opt.fileMode = ZoteroFileMode::None;
    opt.includeTrash = !args.flag("--no-trash");
    opt.log = [](const std::string& m) { std::cout << m << "\n"; };
    std::string lastStage;
    bool tty = FP_ISATTY(1);
    opt.progress = [&](const std::string& stage, int done, int total) {
        if (!tty) {  // one line per stage when writing to a file or pipe
            if (done >= total)
                std::cout << stage << ": " << done << "/" << total << "\n";
            return;
        }
        if (stage != lastStage) {
            if (!lastStage.empty())
                std::cout << "\n";
            lastStage = stage;
        }
        std::cout << "\r" << stage << ": " << done << "/" << total << std::flush;
    };
    std::cout << "Importing from " << pathString(opt.zoteroDir) << " into " << pathString(lib.dataDir()) << "\n";
    auto report = importZotero(lib, opt);
    std::cout << "\nDone: " << report.summary() << "\n";
    for (auto& w : report.warnings)
        std::cout << "warning: " << w << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Args args(argc, argv);
    if (args.command.empty() || args.flag("--help") || args.command == "help") {
        usage();
        return args.command.empty() ? 1 : 0;
    }
    if (args.command == "version") {
        std::cout << "ournotes-cli " << OURNOTES_VERSION << "\n";
        return 0;
    }
    try {
        auto dataDir = args.option("--data-dir");
        if (dataDir.empty())
            migrateFromFreePen();
        Config config;
        Library lib(dataDir.empty() ? config.dataDir() : u8path(dataDir));
        std::map<std::string, std::function<int(Library&, Args&)>> commands = {
            {"info", cmdInfo},
            {"libraries", cmdLibraries},
            {"collections", cmdCollections},
            {"items", cmdItems},
            {"show", cmdShow},
            {"import-zotero", cmdImportZotero},
        };
        registerExtraCommands(commands, config);
        auto it = commands.find(args.command);
        if (it == commands.end()) {
            std::cerr << "Unknown command: " << args.command << "\n";
            usage();
            return 1;
        }
        return it->second(lib, args);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
