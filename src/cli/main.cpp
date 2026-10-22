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

For AI agents and scripts: run `ournotes-cli guide` first (how to read and change a
library safely, with examples).

Library (--library takes an ID or a name; --collection a key or a path such as
"FA26/CS8803"; --json prints JSON for scripts and agents):
  guide                             how to work with a library from an agent or a script
  where                             where OurNotes, its library and its settings are here
  info                              data directory and library statistics
  libraries [--json]                list libraries
  collections [--library L] [--json]
                                    list collections as a tree (all libraries)
  items [--collection C | --library L] [--search TEXT] [--mode quick|all|everything] [--json]
                                    list top-level items (all libraries)
  show KEY [--json]                 an item as JSON (--json: with its notes and attachments)
  text KEY [--pages N]              a PDF's text (an item's PDF, or an attachment's), or a note's
  annotations KEY [--json]          the highlights and comments in an item's PDFs
  set KEY FIELD=VALUE...            change fields (an empty VALUE removes the field)
  tag KEY... [--add T,T] [--remove T,T]
  add-note KEY [TEXT|-]             a note under an item (TEXT, or standard input)
  add-note --collection C [TEXT|-]  a standalone note in a collection
                                    (Markdown, or --html, --text)
  edit-note NOTE [TEXT|-] [--append]
                                    replace (or add to) a note's text
  new-collection NAME [--parent C] [--library L]
  file KEY... --collection C        put items into a collection (unfile: take them out)
  trash KEY... | restore KEY...     move to the trash, or back
  import-zotero [ZOTERO_DIR] [--hardlink | --no-files] [--no-trash]
                                    import (or update from) a Zotero data directory
)" OURNOTES_CLI_EXTRA_USAGE R"(
The data directory defaults to the one configured in the desktop app
(~/OurNotes unless changed); OURNOTES_DATA_DIR overrides it.
)";
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
            {"import-zotero", cmdImportZotero},
        };
        registerAgentCommands(commands, config);
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
