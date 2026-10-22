#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <optional>

#include "core/config.h"
#include "core/library.h"

// A library by its ID or name (also in another case).
int64_t findLibrary(on::Library& lib, const std::string& spec);
// A collection by its key or path ("FA26/CS8803", or "Course/FA26/CS8803" with its
// library's name), in any library or the one given.
struct CollectionRef {
    int64_t library = 0;
    std::string key;
};
CollectionRef findCollection(on::Library& lib, const std::string& spec, std::optional<int64_t> inLibrary = std::nullopt);
// The library an item is in (the only one with that key).
int64_t findItemLibrary(on::Library& lib, const std::string& key);

// Minimal argument parser: `--name value` options, `--flag` switches, positionals.
struct Args {
    std::string command;
    std::vector<std::string> positionals;
    std::map<std::string, std::string> options;

    Args(int argc, char** argv)
    {
        static const std::vector<std::string> valued = {"--data-dir", "--library", "--search", "--mode",
                                                        "--collection", "--format", "-o", "--target",
                                                        "--user", "--password", "--style", "--pages",
                                                        "--add", "--remove", "--parent"};
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            bool takesValue = std::find(valued.begin(), valued.end(), a) != valued.end();
            if (takesValue && i + 1 < argc) {
                options[a] = argv[++i];
            } else if (a.size() > 1 && a[0] == '-') {
                options[a] = "1";
            } else if (command.empty()) {
                command = a;
            } else {
                positionals.push_back(a);
            }
        }
    }
    std::string option(const std::string& name, const std::string& def = {}) const
    {
        auto it = options.find(name);
        return it == options.end() ? def : it->second;
    }
    bool flag(const std::string& name) const { return options.count(name) > 0; }
    std::string positional(size_t i) const { return i < positionals.size() ? positionals[i] : std::string(); }
    int64_t libraryId(on::Library& lib) const
    {
        auto v = option("--library");
        return v.empty() ? lib.defaultLibrary() : findLibrary(lib, v);
    }
    // The library of the items given by key, when neither --library nor --collection says.
    int64_t itemsLibrary(on::Library& lib) const
    {
        if (option("--library").empty() && option("--collection").empty() && !positionals.empty())
            return findItemLibrary(lib, positionals[0]);
        return libraryAndCollection(lib).first;
    }
    // --collection's library and key; else --library's (or the default one) and no collection.
    std::pair<int64_t, std::string> libraryAndCollection(on::Library& lib) const
    {
        if (option("--collection").empty())
            return {libraryId(lib), {}};
        auto only = option("--library").empty() ? std::nullopt : std::optional<int64_t>(libraryId(lib));
        auto c = findCollection(lib, option("--collection"), only);
        return {c.library, c.key};
    }
};

using CommandMap = std::map<std::string, std::function<int(on::Library&, Args&)>>;

// Commands implemented in other translation units (sync, export, full-text index).
void registerExtraCommands(CommandMap& commands, on::Config& config);
// Reading and changing a library (agent_commands.cpp; docs/agents.md).
void registerAgentCommands(CommandMap& commands, on::Config& config);

#define OURNOTES_CLI_EXTRA_USAGE \
    "  index [--rebuild]                 build the full-text index of PDF attachments\n" \
    "  add FILE|DOI|ISBN|arXiv|PMID...   add items (PDFs get metadata looked up) [--collection KEY]\n" \
    "  import FILE [--collection KEY]    import BibTeX, RIS or CSL JSON\n" \
    "  export [KEY...] [--format bibtex|ris|csljson] [--collection KEY] [-o FILE]\n" \
    "  cite [KEY...] [--style apa|ieee|...] [--in-text] [--html] [--collection KEY]\n" \
    "\nSync:\n" \
    "  targets                           list sync targets (* = active)\n" \
    "  add-server URL --user NAME [--password PW] [--insecure]\n" \
    "                                    sign in to an ournotesd server\n" \
    "  add-folder PATH                   sync through a folder (USB drive, network share)\n" \
    "  use-target ID | remove-target ID\n" \
    "  sync [--target ID] [--no-files]   synchronise all libraries\n"
