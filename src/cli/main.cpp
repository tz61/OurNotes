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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#include <shellapi.h>
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

KEY is an item's key (8 letters and digits); --library takes a library's ID or name;
--collection a collection's key or path, such as "FA26/CS8803"; --json prints JSON for
scripts and agents. What deletes something for good asks for --yes. After --, all is
text, even what begins with "-".

Reading:
  guide | where | info              how to work with a library; where things are; statistics
  libraries [--json]                list libraries
  collections [--library L] [--json]
  items [--collection C | --library L] [--search TEXT] [--mode quick|all|everything]
        [--tag T,T] [--unfiled | --trash] [--json]
                                    list top-level items (all libraries)
  show KEY [--json]                 an item as JSON (--json: with its notes and attachments)
  text KEY [--pages N]              a PDF's text (an item's PDF, or an attachment's), or a note's
  annotations KEY [--json]          the highlights and comments in an item's PDFs
  types [TYPE] [--json]             item types; a type's fields and kinds of authors
  tags [--library L] [--json]       tags, how many items have each, their colours
  related KEY [--json]              the items related to an item
  link KEY [--page N] [--quote TEXT] [--markdown | --html | --json]
                                    a link that opens an item, a note, a PDF at a page or an
                                    annotation in the app (ournotes://...); --markdown: as
                                    a note's reference, such as (Smith, 2020, p. 3)
  duplicates [--library L] [--json] items that look like the same one
  open KEY [--folder]               an item's file (or link) opened by the system's program

Items:
  new-item TYPE [FIELD=VALUE...] [--collection C | --library L]
  set KEY FIELD=VALUE...            change fields (an empty VALUE removes the field)
  set-type KEY TYPE [--force]       another item type (--force: lose the fields it has not)
  creators KEY ["[TYPE:]Last, First" | "[TYPE:]Name" ...] [--add | --clear]
                                    list, replace or add to the authors (editor:..., ...)
  tag KEY... [--add T,T] [--remove T,T]
  related KEY --add K,K | --remove K,K
  duplicate KEY                     a copy of an item, related to it
  merge KEY KEY... [--into KEY]     duplicates merged into one (the oldest, or --into)
  copy KEY... --to-library L [--collection C]
                                    copies (with notes, files and annotations) elsewhere
  trash KEY... | restore KEY...     move to the trash, or back
  delete KEY... --yes               delete for good (an item, note, attachment or annotation)
  empty-trash [LIBRARY] --yes       the trash of the default library, or of the one named

Notes and attachments:
  add-note KEY [TEXT|-]             a note under an item (TEXT, or standard input)
  add-note --collection C | --standalone [TEXT|-]
                                    a note of its own (Markdown, or --html, --text)
  edit-note NOTE [TEXT|-] [--append]
                                    replace (or add to) a note's text; ![what](picture.png)
                                    in Markdown puts a picture in (a file, web address, data:)
  add-image NOTE PICTURE... [--row] [--caption TEXT] [--width PX | --height PX]
                                    pictures at the end of a note, each on a line of its
                                    own or side by side (--row), a caption under them
  attach KEY FILE... [--link] [--title T]
                                    files under an item (--link: linked to, not copied)
  attach-url [KEY] URL [--title T] [--collection C]
                                    a link under an item, or of its own
  move KEY... --to ITEM | --standalone [--collection C]
                                    notes and attachments under another item, or on their own
  retrieve-metadata KEY...          PDFs of their own: their details looked up, each filed
                                    under a new item

Annotations (KEY: a PDF, or the item it is under; X,Y: points from the page's top left):
  annotate KEY --quote TEXT [--type highlight|underline] [--page N]
  annotate KEY --type note|text [--at X,Y | --quote TEXT] [--comment TEXT] [--page N]
  annotate KEY --type image --rect X0,Y0,X1,Y1 [--page N]
  annotate KEY --type ink --ink "X,Y X,Y ...[;X,Y X,Y ...]" [--page N]
       ... [--color C] [--comment TEXT] [--tags T,T]
  annotation KEY [--comment T] [--color C] [--quote T] [--tags T,T]
                 [--rect X0,Y0,X1,Y1 | --at X,Y] [--font-size N]
                                    show, change, move or resize one (delete: delete KEY --yes)
  note-from-annotations KEY [--to NOTE]
                                    a note quoting an item's, a PDF's, or one annotation
  export-pdf KEY [-o FILE]          a copy of the PDF with its annotations in it
  render KEY [--page N] [--rect X0,Y0,X1,Y1] [--dpi D] [--no-page] [-o FILE.png]
                                    a page with its annotations as a picture; KEY an
                                    annotation (a doodle): its place on the page, --margin M
                                    around it; --no-page: the annotations alone, on clear
  view-state KEY [--page N] [--zoom PERCENT|page-width|page-fit]
                                    where (and how large) the app opens the PDF

Libraries, collections and tags:
  new-library NAME | rename-library L NAME | delete-library L --yes
  new-collection NAME [--parent C] [--library L]
  rename-collection C NAME
  move-collection C --parent C2 | --top
  delete-collection C [--with-items] --yes
  file KEY... --collection C        put items into a collection (unfile: take them out)
  rename-tag OLD NEW [--library L]  (a tag in several libraries: say which with --library)
  tag-color TAG COLOR|none [--position N] [--library L]
  delete-tag TAG [--library L] --yes

Import and export:
  add FILE|DOI|ISBN|arXiv|PMID... [--collection C] [--no-pdf]
                                    add items (a PDF's details are looked up; arXiv's PDF)
  import FILE [--collection C]      import BibTeX, RIS or CSL JSON
  export [KEY...] [--format bibtex|ris|csljson] [--collection C] [-o FILE]
  cite [KEY...] [--style apa|ieee|...] [--in-text] [--html] [--collection C]
  import-zotero [ZOTERO_DIR] [--hardlink | --no-files] [--no-trash]
                                    import (or update from) a Zotero data directory
  index [--rebuild]                 build the full-text index of PDF attachments

Sync and settings:
  targets                           list sync targets (* = active)
  add-server URL --user NAME [--password PW] [--name N] [--insecure]
                                    sign in to an ournotesd server
  add-folder PATH [--name N]        sync through a folder (USB drive, network share)
  use-target ID | remove-target ID
  sync [--target ID] [--no-files]   synchronise all libraries
  sync-history [--limit N] [--json] what the last syncs brought in and sent out
  conflicts [--clear] [--json]      changes that met other changes made elsewhere
  storage [--remote] [--json]       what the library takes on disk (--remote: the target too)
  config [NAME [VALUE]] [--json]    the app's preferences (quit the app before changing them)

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
#ifdef _WIN32
    // argv is in the ANSI code page on Windows: names, paths and text in other scripts were
    // lost; rebuilt as UTF-8, as ournotesd does
    std::vector<std::string> utf8;
    int n = 0;
    if (wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i)
            utf8.push_back(pathString(fs::path(w[i])));
        LocalFree(w);
    }
    std::vector<char*> utf8Argv;
    for (auto& a : utf8)
        utf8Argv.push_back(a.data());
    if (!utf8Argv.empty()) {
        argc = (int)utf8Argv.size();
        argv = utf8Argv.data();
    }
#endif
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
        lib.setOrigin("ournotes-cli");  // the app's sync history tells its changes apart
        std::map<std::string, std::function<int(Library&, Args&)>> commands = {
            {"info", cmdInfo},
            {"import-zotero", cmdImportZotero},
        };
        registerAllCommands(commands, config);
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
