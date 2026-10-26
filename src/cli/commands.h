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

// Minimal argument parser: `--name value` options, `--flag` switches, positionals. After
// `--`, all are positionals; one with a space in it always is (text: "- a Markdown list").
struct Args {
    std::string command;
    std::vector<std::string> positionals;
    std::map<std::string, std::string> options;
    std::map<std::string, size_t> optionAt;  // how many positionals came before each option

    Args(int argc, char** argv)
    {
        static const std::vector<std::string> valued = {
            "--data-dir", "--library", "--search",  "--mode",   "--collection", "--format",    "-o",
            "--target",   "--user",    "--password", "--style", "--pages",      "--add",       "--remove",
            "--parent",   "--to",      "--to-library", "--into", "--title",     "--tag",       "--position",
            "--type",     "--page",    "--quote",   "--at",     "--rect",       "--ink",       "--color",
            "--comment",  "--tags",    "--font-size", "--zoom", "--limit",      "--name",      "--width",
            "--dpi",      "--margin",  "--height",  "--caption"};
        bool text = false;  // after --
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--" && !text) {
                text = true;
                continue;
            }
            bool named = !text && a.size() > 1 && a[0] == '-' && a.find_first_of(" \t\r\n") == std::string::npos;
            bool takesValue = named && std::find(valued.begin(), valued.end(), a) != valued.end();
            if (named)
                optionAt[a] = positionals.size();
            if (takesValue && i + 1 < argc) {
                options[a] = argv[++i];
            } else if (named) {
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

// Shared by the commands (agent_commands.cpp).
std::optional<int64_t> libraryOption(on::Library& lib, const Args& args);  // --library, if given
// An item by key, in whichever library has it (or the one given).
std::pair<int64_t, on::Json> findItem(on::Library& lib, const std::string& key, std::optional<int64_t> only);
std::vector<std::string> keysFrom(const Args& args, size_t from = 0);  // the positionals from `from` on
std::vector<std::string> commaList(const std::string& s);
// The text a note is given: the argument, or standard input when it is "-" or missing.
std::string contentFrom(const Args& args, size_t at);
std::string libraryName(on::Library& lib, int64_t id);
// FIELD=VALUE positionals from `from` on, each field checked against the item type.
std::vector<std::pair<std::string, std::string>> fieldValues(const Args& args, size_t from, const std::string& itemType);
// A number given as `name` (or `def`), from `min` to `max`.
double numberOption(const Args& args, const std::string& name, double def, double min, double max);
// Throws when more than `most` positionals are given (what is left over would be ignored).
void requirePositionals(const Args& args, size_t most);
// Throws unless an attachment's file is here: not downloaded yet, or (a linked file) not
// where it was.
void requireFile(on::Library& lib, int64_t id, const on::Json& attachment);
// Throws unless the library can be changed (a group library the user may only read).
void requireEditable(on::Library& lib, int64_t id);
// Throws unless --yes is given: what happens cannot be undone.
void requireYes(const Args& args, const std::string& what);

// A colour by its name (yellow, red, green, blue, purple, magenta, orange, gray) or as #rrggbb.
std::string annotationColorValue(const std::string& spec);

using CommandMap = std::map<std::string, std::function<int(on::Library&, Args&)>>;

namespace on::sync {
class Transport;
}
// A connection to a sync target (extra_commands.cpp).
std::unique_ptr<on::sync::Transport> makeTransport(const on::SyncTarget& t);

// Every command of ournotes-cli.
void registerAllCommands(CommandMap& commands, on::Config& config);
// Commands implemented in other translation units (sync, export, full-text index).
void registerExtraCommands(CommandMap& commands, on::Config& config);
// Reading and changing a library (agent_commands.cpp; docs/agents.md).
void registerAgentCommands(CommandMap& commands, on::Config& config);
// Libraries, collections, items, attachments and tags made, changed and deleted, as in the
// app (edit_commands.cpp).
void registerEditCommands(CommandMap& commands, on::Config& config);
// Annotations made and changed, a note from them, a PDF with them, a PDF's reading
// position (annotation_commands.cpp).
void registerAnnotationCommands(CommandMap& commands, on::Config& config);
