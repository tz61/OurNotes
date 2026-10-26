// ournotes-cli commands for the rest of what the app does to a library (docs/agents.md):
// libraries and collections made, renamed, moved and deleted; items made from nothing,
// given another type, authors, copies and relations; duplicates merged; notes and
// attachments moved; items copied to another library; files and links attached and opened;
// tags listed, renamed, coloured and deleted; items deleted for good, the trash emptied.

#include <cstdlib>
#include <iostream>
#include <set>
#include <tuple>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include "cli/commands.h"
#include "core/schema.h"
#include "core/sync/engine.h"
#include "core/sync/protocol.h"

using namespace on;

namespace {

std::string titleOf(const Json& d)
{
    auto t = jstr(d, "itemType") == "note" ? noteTitle(jstr(d, "note")) : jstr(d, "title");
    return t.empty() ? "(untitled)" : t;
}

bool isRegularItem(const Json& d) { return schema::isRegularType(jstr(d, "itemType")); }

// The items given, all in one library (which is returned).
int64_t sameLibrary(Library& lib, const Args& args, const std::vector<std::string>& keys, std::vector<Json>* items = nullptr)
{
    int64_t id = 0;
    auto only = libraryOption(lib, args);
    for (auto& k : keys) {
        auto [lid, d] = findItem(lib, k, only);
        if (id && lid != id)
            throw Error("the items are in different libraries: give those of one");
        id = lid;
        if (items)
            items->push_back(d);
    }
    return id;
}

// The libraries --library names, or else all of them.
std::vector<int64_t> libraryIds(Library& lib, const Args& args)
{
    if (auto only = libraryOption(lib, args))
        return {*only};
    std::vector<int64_t> ids;
    for (auto& l : lib.libraries())
        ids.push_back(l.id);
    return ids;
}

// --- libraries ------------------------------------------------------------------------------

void requireNewName(Library& lib, const std::string& name, int64_t except = 0)
{
    if (name.empty())
        throw Error("a library needs a name");
    for (auto& l : lib.libraries())
        if (l.id != except && toLower(l.name) == toLower(name))
            throw Error("there is a library \"" + l.name + "\" already");
}

int cmdNewLibrary(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto name = trim(args.positional(0));
    requireNewName(lib, name);
    // as the app makes one: of type "group" (a "user" library is the user's own, made at
    // the first start; an empty one that never synced is removed by the next sync)
    std::cout << lib.createLibrary(name, "group") << "\n";
    return 0;
}

int cmdRenameLibrary(Library& lib, Args& args)
{
    requirePositionals(args, 2);
    auto id = findLibrary(lib, args.positional(0));
    auto name = trim(args.positional(1));
    requireNewName(lib, name, id);
    lib.renameLibrary(id, name);
    std::cout << id << "\n";
    return 0;
}

// --- collections ----------------------------------------------------------------------------

int cmdRenameCollection(Library& lib, Args& args)
{
    requirePositionals(args, 2);
    auto c = findCollection(lib, args.positional(0), libraryOption(lib, args));
    auto name = trim(args.positional(1));
    if (name.empty())
        throw Error("give the collection's new name");
    requireEditable(lib, c.library);
    auto data = lib.collection(c.library, c.key);
    (*data)["name"] = name;
    lib.saveCollection(c.library, *data);
    std::cout << c.key << "\n";
    return 0;
}

int cmdMoveCollection(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto c = findCollection(lib, args.positional(0), libraryOption(lib, args));
    Json parent = false;  // at the top of its library
    if (auto p = args.option("--parent"); !p.empty()) {
        auto to = findCollection(lib, p, c.library);  // in its own library
        if (to.key == c.key)
            throw Error("a collection cannot go into itself");
        for (auto& d : lib.descendantCollections(c.library, c.key))
            if (d == to.key)
                throw Error("a collection cannot go into one of its own subcollections");
        parent = to.key;
    } else if (!args.flag("--top")) {
        throw Error("say where: --parent COLLECTION, or --top");
    }
    requireEditable(lib, c.library);
    auto data = lib.collection(c.library, c.key);
    (*data)["parentCollection"] = parent;
    lib.saveCollection(c.library, *data);
    std::cout << c.key << "\n";
    return 0;
}

int cmdDeleteCollection(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto c = findCollection(lib, args.positional(0), libraryOption(lib, args));
    requireEditable(lib, c.library);
    bool withItems = args.flag("--with-items");
    auto subs = lib.descendantCollections(c.library, c.key).size();
    auto what = "this deletes the collection \"" + jstr(*lib.collection(c.library, c.key), "name") + "\"" +
                (subs ? " and its " + std::to_string(subs) + " subcollection(s)" : "");
    requireYes(args, what + (withItems ? ", and moves their items to the trash" : " (the items stay in the library)"));
    lib.deleteCollection(c.library, c.key, withItems);
    std::cout << c.key << "\n";
    return 0;
}

// --- items ----------------------------------------------------------------------------------

void requireRegularType(const std::string& type)
{
    if (!schema::isValidType(type) || !schema::isRegularType(type))
        throw Error("no item type \"" + type + "\" (see: ournotes-cli types)");
}

int cmdNewItem(Library& lib, Args& args)
{
    auto type = args.positional(0);
    requireRegularType(type);
    auto [id, collection] = args.libraryAndCollection(lib);
    requireEditable(lib, id);
    Json data{{"itemType", type}, {"title", ""}, {"creators", Json::array()}};
    for (auto& [field, value] : fieldValues(args, 1, type))
        if (!value.empty())
            data[field] = value;
    if (!collection.empty())
        data["collections"] = Json::array({collection});
    std::cout << jstr(lib.createItem(id, data), "key") << "\n";
    return 0;
}

int cmdTypes(Library&, Args& args)
{
    requirePositionals(args, 1);
    auto type = args.positional(0);
    bool json = args.flag("--json");
    if (type.empty()) {
        Json out = Json::array();
        for (auto& t : schema::itemTypes(true)) {
            if (json)
                out.push_back(Json{{"type", t}, {"label", schema::typeLabel(t)}});
            else
                std::cout << t << "  " << schema::typeLabel(t) << "\n";
        }
        if (json)
            std::cout << out.dump(2) << "\n";
        return 0;
    }
    requireRegularType(type);
    auto primary = schema::primaryCreatorType(type);
    if (json) {
        Json fields = Json::array(), creators = Json::array();
        for (auto& f : schema::fieldsFor(type))
            fields.push_back(Json{{"field", f}, {"label", schema::fieldLabel(f)}});
        for (auto& c : schema::creatorTypesFor(type))
            creators.push_back(Json{{"creatorType", c}, {"label", schema::creatorLabel(c)}, {"primary", c == primary}});
        std::cout << Json{{"type", type}, {"label", schema::typeLabel(type)}, {"fields", fields}, {"creatorTypes", creators}}
                         .dump(2)
                  << "\n";
        return 0;
    }
    std::cout << type << "  " << schema::typeLabel(type) << "\nfields:\n";
    for (auto& f : schema::fieldsFor(type))
        std::cout << "  " << f << "  " << schema::fieldLabel(f) << "\n";
    std::cout << "creators:\n";
    for (auto& c : schema::creatorTypesFor(type))
        std::cout << "  " << c << "  " << schema::creatorLabel(c) << (c == primary ? "  (first)" : "") << "\n";
    return 0;
}

int cmdSetType(Library& lib, Args& args)
{
    requirePositionals(args, 2);
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    auto type = args.positional(1);
    if (!isRegularItem(item))
        throw Error(jstr(item, "key") + " is a " + jstr(item, "itemType") + ": only a regular item changes its type");
    requireRegularType(type);
    requireEditable(lib, id);
    if (jstr(item, "itemType") != type) {
        Json dropped;
        auto converted = schema::convertType(item, type, &dropped);
        if (!dropped.empty() && !args.flag("--force")) {
            std::string names;
            for (auto& [k, v] : dropped.items())
                names += (names.empty() ? "" : ", ") + k;
            throw Error("a " + type + " has no " + names + ": add --force to change the type and lose them");
        }
        lib.saveItem(id, converted);
    }
    std::cout << jstr(item, "key") << "\n";
    return 0;
}

std::string creatorText(const Json& c)
{
    if (c.contains("name"))
        return jstr(c, "name");
    auto first = jstr(c, "firstName");
    return jstr(c, "lastName") + (first.empty() ? "" : ", " + first);
}

int cmdCreators(Library& lib, Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    auto key = jstr(item, "key"), type = jstr(item, "itemType");
    if (!isRegularItem(item))
        throw Error(key + " is a " + type + ": only a regular item has authors");
    std::vector<std::string> names(args.positionals.begin() + std::min<size_t>(1, args.positionals.size()),
                                   args.positionals.end());
    if (auto first = args.option("--add"); !first.empty() && first != "1")  // --add "Doe, Jane" ...
        names.insert(names.begin(), first);
    if (names.empty() && !args.flag("--clear")) {
        auto creators = item.value("creators", Json::array());
        if (args.flag("--json")) {
            std::cout << creators.dump(2) << "\n";
            return 0;
        }
        for (auto& c : creators)
            std::cout << jstr(c, "creatorType") << ": " << creatorText(c) << "\n";
        return 0;
    }
    requireEditable(lib, id);
    auto valid = schema::creatorTypesFor(type);
    std::set<std::string> known;  // creator types of any item type
    for (auto& t : schema::itemTypes(true))
        for (auto& c : schema::creatorTypesFor(t))
            known.insert(c);
    Json list = args.flag("--add") ? item.value("creators", Json::array()) : Json::array();
    for (auto& n : names) {
        auto creatorType = schema::primaryCreatorType(type);
        auto rest = n;
        if (auto colon = n.find(':'); colon != std::string::npos && known.count(trim(n.substr(0, colon)))) {
            creatorType = trim(n.substr(0, colon));
            if (std::find(valid.begin(), valid.end(), creatorType) == valid.end()) {
                std::string list;
                for (auto& v : valid)
                    list += (list.empty() ? "" : ", ") + v;
                throw Error("a " + type + " has no " + creatorType + "; it has: " + list);
            }
            rest = n.substr(colon + 1);
        }
        rest = trim(rest);
        if (rest.empty())
            throw Error("no name in \"" + n + "\"");
        if (auto comma = rest.find(','); comma != std::string::npos)
            list.push_back(Json{{"creatorType", creatorType},
                                {"firstName", trim(rest.substr(comma + 1))},
                                {"lastName", trim(rest.substr(0, comma))}});
        else
            list.push_back(Json{{"creatorType", creatorType}, {"name", rest}});
    }
    lib.updateItem(id, key, [&](Json& d) { d["creators"] = list; });
    std::cout << key << "\n";
    return 0;
}

int cmdDuplicate(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (!isRegularItem(item))
        throw Error(jstr(item, "key") + " is a " + jstr(item, "itemType") + ": only a regular item is duplicated");
    requireEditable(lib, id);
    Json copy = item;
    for (const char* k : {"key", "dateAdded", "dateModified", "relations", "deleted"})
        copy.erase(k);
    auto made = lib.createItem(id, copy);
    lib.addRelated(id, jstr(made, "key"), jstr(item, "key"));
    std::cout << jstr(made, "key") << "\n";
    return 0;
}

int cmdRelated(Library& lib, Args& args)
{
    requirePositionals(args, 1);  // (more than one to --add: K1,K2)
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    auto key = jstr(item, "key");
    auto add = commaList(args.option("--add")), remove = commaList(args.option("--remove"));
    if (!add.empty() || !remove.empty()) {
        requireEditable(lib, id);
        for (auto& k : add) {
            findItem(lib, k, id);  // in the same library
            if (k == key)
                throw Error("an item is not related to itself");
        }
        for (auto& k : add)
            lib.addRelated(id, key, k);
        for (auto& k : remove)
            lib.removeRelated(id, key, k);
        std::cout << key << "\n";
        return 0;
    }
    Json out = Json::array();
    for (auto& k : lib.relatedKeys(id, key)) {
        auto d = lib.item(id, k);
        if (!d)
            continue;
        if (args.flag("--json"))
            out.push_back(Json{{"key", k}, {"itemType", jstr(*d, "itemType")}, {"title", titleOf(*d)}});
        else
            std::cout << k << "  " << titleOf(*d) << "\n";
    }
    if (args.flag("--json"))
        std::cout << out.dump(2) << "\n";
    return 0;
}

int cmdDuplicates(Library& lib, Args& args)
{
    requirePositionals(args, 0);
    Json out = Json::array();
    bool first = true;
    for (auto id : libraryIds(lib, args)) {
        auto name = libraryName(lib, id);
        for (auto& group : lib.duplicateGroups(id)) {
            Json items = Json::array();
            if (!args.flag("--json") && !first)
                std::cout << "\n";
            first = false;
            for (auto& d : lib.items(id, group)) {
                if (args.flag("--json")) {
                    items.push_back(Json{{"key", jstr(d, "key")},
                                         {"title", titleOf(d)},
                                         {"date", jstr(d, "date")},
                                         {"dateAdded", jstr(d, "dateAdded")}});
                } else {
                    auto year = yearOf(jstr(d, "date"));
                    std::cout << jstr(d, "key") << "  " << (year.empty() ? "    " : year) << "  " << titleOf(d)
                              << "  (" << name << ")\n";
                }
            }
            if (args.flag("--json"))
                out.push_back(Json{{"library", name}, {"libraryId", id}, {"items", items}});
        }
    }
    if (args.flag("--json"))
        std::cout << out.dump(2) << "\n";
    return 0;
}

int cmdMerge(Library& lib, Args& args)
{
    auto keys = keysFrom(args);
    if (auto into = args.option("--into"); !into.empty() && std::find(keys.begin(), keys.end(), into) == keys.end())
        keys.insert(keys.begin(), into);
    if (std::set<std::string>(keys.begin(), keys.end()).size() != keys.size())
        throw Error("an item is given twice");
    if (keys.size() < 2)
        throw Error("give two items or more to merge");
    std::vector<Json> items;
    auto id = sameLibrary(lib, args, keys, &items);
    for (auto& d : items) {
        if (!isRegularItem(d))
            throw Error(jstr(d, "key") + " is a " + jstr(d, "itemType") + ": only regular items are merged");
        if (jbool(d, "deleted"))
            throw Error(jstr(d, "key") + " is in the trash");
    }
    requireEditable(lib, id);
    // into the one given, or else the oldest, as the app does
    auto master = args.option("--into");
    if (master.empty())
        master = jstr(*std::min_element(items.begin(), items.end(),
                                        [](const Json& a, const Json& b) { return jstr(a, "dateAdded") < jstr(b, "dateAdded"); }),
                      "key");
    std::vector<std::string> others;
    for (auto& k : keys)
        if (k != master)
            others.push_back(k);
    lib.mergeItems(id, master, others);
    std::cout << master << "\n";
    return 0;
}

int cmdMove(Library& lib, Args& args)
{
    auto keys = keysFrom(args);
    auto to = args.option("--to");
    if (to.empty() == !args.flag("--standalone"))
        throw Error("say where: --to ITEM (under that item), or --standalone [--collection C]");
    std::vector<Json> items;
    auto id = sameLibrary(lib, args, keys, &items);
    for (auto& d : items)
        if (jstr(d, "itemType") != "note" && jstr(d, "itemType") != "attachment")
            throw Error(jstr(d, "key") + " is a " + jstr(d, "itemType") + ": only notes and attachments move");
    std::string parent, collection;
    if (!to.empty()) {
        auto [pid, p] = findItem(lib, to, id);
        if (!isRegularItem(p))
            throw Error(to + " is a " + jstr(p, "itemType") + ": notes and attachments go under a regular item");
        parent = to;
    } else if (auto c = args.option("--collection"); !c.empty()) {
        collection = findCollection(lib, c, id).key;
    }
    requireEditable(lib, id);
    lib.moveToParent(id, keys, parent, collection);
    for (auto& k : keys)
        std::cout << k << "\n";
    return 0;
}

int cmdCopy(Library& lib, Args& args, Config& config)
{
    auto keys = keysFrom(args);
    std::vector<Json> items;
    auto from = sameLibrary(lib, args, keys, &items);
    for (auto& d : items) {
        if (jstr(d, "itemType") == "annotation")
            throw Error(jstr(d, "key") + " is an annotation: copy its PDF, which takes its annotations along");
        if (jbool(d, "deleted"))
            throw Error(jstr(d, "key") + " is in the trash");
    }
    int64_t to = 0;
    std::string collection;
    auto toLibrary = args.option("--to-library");
    if (auto c = args.option("--collection"); !c.empty()) {
        auto ref = findCollection(lib, c, toLibrary.empty() ? std::nullopt : std::optional<int64_t>(findLibrary(lib, toLibrary)));
        to = ref.library;
        collection = ref.key;
    } else if (!toLibrary.empty()) {
        to = findLibrary(lib, toLibrary);
    } else {
        throw Error("say where: --to-library LIBRARY, and/or --collection C");
    }
    if (to == from)  // as in the app: within a library, items are filed, not copied
        throw Error("that is the items' own library: put them into a collection with file, or make a copy with duplicate");
    requireEditable(lib, to);
    // files synced but not downloaded (on demand): downloaded first, as the app does
    auto missing = lib.filesNotHere(from, keys);
    auto target = config.activeTarget();
    if (!missing.empty() && target) {
        auto transport = makeTransport(*target);
        auto failed = sync::SyncEngine(lib, *transport, target->id).downloadAttachments(from, missing);
        if (!failed.empty()) {
            std::string why;
            for (auto& [key, error] : failed)
                why += "\n  " + key + ": " + error;
            throw Error("nothing was copied: the files of these could not be downloaded (copies would be "
                        "without them for good)" +
                        why);
        }
        missing = lib.filesNotHere(from, keys);
    }
    for (auto& key : missing)
        std::cerr << "warning: the file of " << key
                  << " is not on this computer, and could not be downloaded: its copy has none either\n";
    for (auto& k : lib.copyItems(from, keys, to, collection))
        std::cout << k << "\n";
    return 0;
}

int cmdDelete(Library& lib, Args& args)
{
    auto keys = keysFrom(args);
    std::map<int64_t, std::vector<std::string>> byLibrary;
    auto only = libraryOption(lib, args);
    for (auto& k : keys) {
        auto [id, d] = findItem(lib, k, only);
        if (jbool(d, "annotationIsExternal"))
            throw Error(k + " is an annotation in the PDF file itself: it cannot be deleted here");
        requireEditable(lib, id);
        byLibrary[id].push_back(k);
    }
    requireYes(args, "this deletes " + std::to_string(keys.size()) +
                         " item(s) for good, with the notes, attachments, files and annotations under them");
    for (auto& [id, ks] : byLibrary)
        lib.eraseItems(id, ks);
    for (auto& k : keys)
        std::cout << k << "\n";
    return 0;
}

int cmdEmptyTrash(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto id = args.positionals.empty() ? args.libraryId(lib) : findLibrary(lib, args.positional(0));
    requireEditable(lib, id);
    std::vector<std::string> trashed;  // as Library::emptyTrash takes them
    for (auto& d : lib.allItems(id, true))
        if (jbool(d, "deleted"))
            trashed.push_back(jstr(d, "key"));
    if (trashed.empty())
        return 0;
    requireYes(args, "this deletes the " + std::to_string(trashed.size()) + " item(s) in the trash of \"" +
                         libraryName(lib, id) + "\" for good, with what is under them");
    lib.emptyTrash(id);
    for (auto& k : trashed)
        std::cout << k << "\n";
    return 0;
}

// --- attachments ----------------------------------------------------------------------------

int cmdAttach(Library& lib, Args& args)
{
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (!isRegularItem(item))
        throw Error(jstr(item, "key") + " is a " + jstr(item, "itemType") +
                    ": files go under a regular item (a file of its own: ournotes-cli add FILE)");
    if (args.positionals.size() < 2)
        throw Error("give the files to attach");
    auto title = args.option("--title");
    if (!title.empty() && args.positionals.size() > 2)
        throw Error("--title is for one file");
    std::vector<fs::path> files;
    for (size_t i = 1; i < args.positionals.size(); ++i) {
        auto path = u8path(args.positionals[i]);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            throw Error("no file " + args.positionals[i]);
        files.push_back(fs::absolute(path, ec));
    }
    requireEditable(lib, id);
    for (auto& f : files)
        std::cout << jstr(lib.importAttachment(id, f, jstr(item, "key"), args.flag("--link"), title), "key") << "\n";
    return 0;
}

bool hasScheme(const std::string& url)
{
    auto colon = url.find(':');
    if (colon == std::string::npos || colon == 0 || !std::isalpha((unsigned char)url[0]))
        return false;
    return std::all_of(url.begin(), url.begin() + colon,
                       [](char c) { return std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.'; });
}

int cmdAttachUrl(Library& lib, Args& args)
{
    requirePositionals(args, 2);
    int64_t id = 0;
    std::string url, parent, collection;
    if (args.positionals.size() >= 2) {  // under an item
        auto [lid, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
        if (!isRegularItem(item))
            throw Error(jstr(item, "key") + " is a " + jstr(item, "itemType") + ": links go under a regular item");
        id = lid;
        parent = jstr(item, "key");
        url = args.positional(1);
    } else {  // a link of its own
        std::tie(id, collection) = args.libraryAndCollection(lib);
        url = args.positional(0);
    }
    if (!hasScheme(url))
        throw Error("give the link with its scheme, as in https://example.org/page");
    requireEditable(lib, id);
    auto att = lib.createLinkedUrl(id, url, args.option("--title"), parent);
    if (!collection.empty())
        lib.addToCollection(id, collection, {jstr(att, "key")});
    std::cout << jstr(att, "key") << "\n";
    return 0;
}

// Opens a file, a folder or a link with the program the system has for it (or with
// OURNOTES_OPENER, if set).
void openWithSystem(const std::string& target)
{
    const char* env = std::getenv("OURNOTES_OPENER");
    std::string opener = env ? env : "";
#if defined(_WIN32)
    if (opener.empty()) {
        auto w = u8path(target).wstring();  // (UTF-16)
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
            throw Error("nothing on this computer opens " + target);
        return;
    }
    auto command = u8path("\"" + opener + "\" \"" + target + "\"").wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        throw Error("could not run " + opener);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
#if defined(__APPLE__)
    if (opener.empty())
        opener = "open";
#else
    if (opener.empty())
        opener = "xdg-open";
#endif
    std::vector<std::string> words{opener, target};
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, opener.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        throw Error("could not run " + opener);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw Error(opener + " could not open " + target);
#endif
}

int cmdOpen(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto [id, item] = findItem(lib, args.positional(0), libraryOption(lib, args));
    if (jstr(item, "itemType") == "note" || jstr(item, "itemType") == "annotation")
        throw Error(jstr(item, "key") + " is a " + jstr(item, "itemType") + ": it is read with ournotes-cli text, or in the app");
    auto att = item;
    if (isRegularItem(item)) {
        auto best = lib.bestAttachment(id, jstr(item, "key"));
        if (!best)
            throw Error(jstr(item, "key") + " has no attachment");
        att = *best;
    }
    std::string target;
    if (jstr(att, "linkMode") == "linked_url") {
        if (args.flag("--folder"))
            throw Error(jstr(att, "key") + " is a link: it has no folder");
        target = jstr(att, "url");
    } else {
        requireFile(lib, id, att);
        auto path = lib.attachmentFile(id, att);
        target = pathString(args.flag("--folder") ? path.parent_path() : path);
    }
    openWithSystem(target);
    std::cout << target << "\n";
    return 0;
}

// --- tags -----------------------------------------------------------------------------------

int cmdTags(Library& lib, Args& args)
{
    requirePositionals(args, 0);
    Json out = Json::array();
    auto ids = libraryIds(lib, args);
    for (auto id : ids) {
        auto name = libraryName(lib, id);
        for (auto& t : lib.tags(id)) {
            if (args.flag("--json")) {
                Json j{{"library", name}, {"libraryId", id}, {"tag", t.name}, {"items", t.count}, {"automatic", t.type == 1}};
                if (!t.color.empty())
                    j["color"] = t.color;
                out.push_back(j);
                continue;
            }
            std::cout << t.name << "  (" << t.count << ")" << (t.color.empty() ? "" : "  " + t.color)
                      << (t.type == 1 ? "  automatic" : "") << (ids.size() > 1 ? "  (" + name + ")" : "") << "\n";
        }
    }
    if (args.flag("--json"))
        std::cout << out.dump(2) << "\n";
    return 0;
}

// The library a tag is changed in, as in the app: --library, or the only one that has it.
int64_t libraryWithTag(Library& lib, const Args& args, const std::string& tag)
{
    std::vector<int64_t> ids;
    for (auto id : libraryIds(lib, args)) {
        auto tags = lib.tags(id);
        if (std::any_of(tags.begin(), tags.end(), [&](const TagInfo& t) { return t.name == tag; }))
            ids.push_back(id);
    }
    if (ids.empty())
        throw Error("no tag \"" + tag + "\" (see: ournotes-cli tags)");
    if (ids.size() > 1) {
        std::string names;
        for (auto id : ids)
            names += (names.empty() ? "" : ", ") + libraryName(lib, id);
        throw Error("the tag \"" + tag + "\" is in " + names + ": say which with --library");
    }
    requireEditable(lib, ids[0]);
    return ids[0];
}

int cmdRenameTag(Library& lib, Args& args)
{
    requirePositionals(args, 2);
    auto from = args.positional(0), to = trim(args.positional(1));
    if (to.empty())
        throw Error("give the tag's new name");
    lib.renameTag(libraryWithTag(lib, args, from), from, to);
    std::cout << to << "\n";
    return 0;
}

int cmdDeleteTag(Library& lib, Args& args)
{
    requirePositionals(args, 1);
    auto tag = args.positional(0);
    auto id = libraryWithTag(lib, args, tag);
    requireYes(args, "this takes the tag \"" + tag + "\" off every item of \"" + libraryName(lib, id) + "\" that has it");
    lib.deleteTag(id, tag);
    std::cout << tag << "\n";
    return 0;
}

// A colour by its name (yellow, red...) or as #rrggbb.
std::string colorValue(const std::string& spec)
{
    for (auto& c : schema::kAnnotationColors)
        if (toLower(spec) == toLower(c.name))
            return c.hex;
    bool hex = spec.size() == 7 && spec[0] == '#' &&
               std::all_of(spec.begin() + 1, spec.end(), [](char c) { return std::isxdigit((unsigned char)c); });
    if (!hex) {
        std::string names;
        for (auto& c : schema::kAnnotationColors)
            names += (names.empty() ? "" : ", ") + toLower(c.name);
        throw Error("no colour \"" + spec + "\": give one of " + names + ", or #rrggbb");
    }
    return toLower(spec);
}

int cmdTagColor(Library& lib, Args& args)
{
    requirePositionals(args, 2);
    auto tag = args.positional(0), spec = args.positional(1);
    if (spec.empty())
        throw Error("give a colour (yellow, red, ... or #rrggbb), or none");
    auto color = toLower(spec) == "none" ? std::string() : colorValue(spec);
    int position = (int)numberOption(args, "--position", 0, 1, 1000) - 1;  // -1: after the coloured ones
    lib.setTagColor(libraryWithTag(lib, args, tag), tag, color, position);
    std::cout << tag << "\n";
    return 0;
}

} // namespace

std::string annotationColorValue(const std::string& spec) { return colorValue(spec); }

void registerEditCommands(CommandMap& commands, Config& config)
{
    commands["new-library"] = cmdNewLibrary;
    commands["rename-library"] = cmdRenameLibrary;
    commands["rename-collection"] = cmdRenameCollection;
    commands["move-collection"] = cmdMoveCollection;
    commands["delete-collection"] = cmdDeleteCollection;
    commands["new-item"] = cmdNewItem;
    commands["types"] = cmdTypes;
    commands["set-type"] = cmdSetType;
    commands["creators"] = cmdCreators;
    commands["duplicate"] = cmdDuplicate;
    commands["related"] = cmdRelated;
    commands["duplicates"] = cmdDuplicates;
    commands["merge"] = cmdMerge;
    commands["move"] = cmdMove;
    commands["copy"] = [&config](Library& lib, Args& args) { return cmdCopy(lib, args, config); };
    commands["delete"] = cmdDelete;
    commands["empty-trash"] = cmdEmptyTrash;
    commands["attach"] = cmdAttach;
    commands["attach-url"] = cmdAttachUrl;
    commands["open"] = cmdOpen;
    commands["tags"] = cmdTags;
    commands["rename-tag"] = cmdRenameTag;
    commands["delete-tag"] = cmdDeleteTag;
    commands["tag-color"] = cmdTagColor;
}
