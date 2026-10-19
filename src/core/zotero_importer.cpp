#include "core/zotero_importer.h"

#include <cstdlib>
#include <map>
#include <regex>
#include <set>

#include "core/hash.h"
#include "core/schema.h"
#include "core/sqlite.h"

namespace fp {

namespace {

const char* kLinkModes[] = {"imported_file", "imported_url", "linked_file", "linked_url", "embedded_image"};
const char* kAnnotationTypes[] = {"", "highlight", "note", "image", "ink", "underline", "text"};

std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Zotero profile directories (where prefs.js lives) per platform.
std::vector<fs::path> profileRoots()
{
    std::vector<fs::path> roots;
#if defined(_WIN32)
    if (!env("APPDATA").empty())
        roots.push_back(u8path(env("APPDATA")) / "Zotero" / "Zotero" / "Profiles");
#elif defined(__APPLE__)
    if (!env("HOME").empty())
        roots.push_back(u8path(env("HOME")) / "Library" / "Application Support" / "Zotero" / "Profiles");
#else
    if (!env("HOME").empty())
        roots.push_back(u8path(env("HOME")) / ".zotero" / "zotero");
#endif
    return roots;
}

std::map<std::string, std::string> readPrefs()
{
    std::map<std::string, std::string> prefs;
    static const std::regex re(R"re(user_pref\("([^"]+)",\s*("(?:[^"\\]|\\.)*"|true|false|-?\d+)\);)re");
    for (auto& root : profileRoots()) {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            continue;
        for (auto& entry : fs::directory_iterator(root, ec)) {
            auto prefsFile = entry.path() / "prefs.js";
            if (!fs::is_regular_file(prefsFile, ec))
                continue;
            std::string text;
            try {
                text = readFile(prefsFile);
            } catch (...) {
                continue;
            }
            for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
                std::string value = (*it)[2];
                if (value.size() >= 2 && value.front() == '"') {
                    try {
                        value = Json::parse(value).get<std::string>();
                    } catch (...) {
                        value = value.substr(1, value.size() - 2);
                    }
                }
                prefs[(*it)[1]] = value;
            }
        }
    }
    return prefs;
}

int64_t fileMtimeMs(const fs::path& p)
{
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec)
        return 0;
    auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(ft - fs::file_time_type::clock::now() +
                                                                       std::chrono::system_clock::now());
    return sys.time_since_epoch().count();
}

// Copy (or hard-link) the contents of one Zotero storage folder, skipping Zotero's
// own cache files. Returns bytes copied.
int64_t copyStorageDir(const fs::path& src, const fs::path& dst, ZoteroFileMode mode, int& files)
{
    int64_t bytes = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        auto name = pathString(it->path().filename());
        if (startsWith(name, ".zotero")) {
            if (it->is_directory(ec))
                it.disable_recursion_pending();
            continue;
        }
        auto rel = fs::relative(it->path(), src, ec);
        auto target = dst / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        auto size = (int64_t)it->file_size(ec);
        std::error_code ec2;
        if (fs::exists(target, ec2) && (int64_t)fs::file_size(target, ec2) == size)
            continue;
        fs::create_directories(target.parent_path(), ec2);
        bool done = false;
        if (mode == ZoteroFileMode::Hardlink) {
            fs::remove(target, ec2);
            fs::create_hard_link(it->path(), target, ec2);
            done = !ec2;
        }
        if (!done) {
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec2);
            if (ec2)
                throw Error("copy failed: " + pathString(it->path()) + ": " + ec2.message());
            // keep the original modification time
            fs::last_write_time(target, fs::last_write_time(it->path(), ec2), ec2);
        }
        bytes += size;
        ++files;
    }
    return bytes;
}

struct ZLibrary {
    int64_t zoteroId = 0;
    int64_t localId = 0;
    std::string type;
    int64_t groupId = 0;
    std::string uriPrefix;  // http://zotero.org/users/123 or /groups/456
};

} // namespace

std::string ZoteroImportReport::summary() const
{
    std::string s = std::to_string(libraries) + " libraries, " + std::to_string(collections) + " collections, " +
                    std::to_string(items) + " items, " + std::to_string(notes) + " notes, " +
                    std::to_string(attachments) + " attachments, " + std::to_string(annotations) + " annotations";
    if (updated || unchanged)
        s += " (" + std::to_string(updated) + " updated, " + std::to_string(unchanged) + " unchanged)";
    s += "; " + std::to_string(filesCopied) + " files copied (" + humanSize((uint64_t)bytesCopied) + ")";
    if (filesMissing)
        s += ", " + std::to_string(filesMissing) + " files missing";
    if (styles)
        s += ", " + std::to_string(styles) + " citation styles";
    return s;
}

fs::path findZoteroDataDir()
{
    auto prefs = readPrefs();
    if (prefs["extensions.zotero.useDataDir"] == "true" && !prefs["extensions.zotero.dataDir"].empty())
        return u8path(prefs["extensions.zotero.dataDir"]);
#ifdef _WIN32
    auto home = env("USERPROFILE");
#else
    auto home = env("HOME");
#endif
    return u8path(home) / "Zotero";
}

ZoteroImportReport importZotero(Library& library, const ZoteroImportOptions& opt)
{
    ZoteroImportReport report;
    auto log = [&](const std::string& m) {
        if (opt.log)
            opt.log(m);
    };
    auto progress = [&](const std::string& stage, int done, int total) {
        if (opt.progress)
            opt.progress(stage, done, total);
    };
    auto cancelled = [&] { return opt.cancelled && opt.cancelled(); };

    fs::path zdir = opt.zoteroDir.empty() ? findZoteroDataDir() : opt.zoteroDir;
    fs::path zdb = zdir / "zotero.sqlite";
    std::error_code ec;
    if (!fs::is_regular_file(zdb, ec))
        throw Error("No Zotero database found at " + pathString(zdb));

    // Snapshot the database: Zotero holds an exclusive lock while it runs.
    fs::path tmp = fs::temp_directory_path() / ("freepen-zotero-" + randomToken(6));
    fs::create_directories(tmp);
    struct Cleanup {
        fs::path p;
        ~Cleanup()
        {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{tmp};
    fs::copy_file(zdb, tmp / "zotero.sqlite");
    for (const char* suffix : {"-wal", "-journal"}) {
        auto extra = zdb;
        extra += suffix;
        if (fs::is_regular_file(extra, ec) && fs::file_size(extra, ec) > 0)
            fs::copy_file(extra, tmp / (std::string("zotero.sqlite") + suffix));
    }
    sql::Db z(tmp / "zotero.sqlite");
    log("Reading Zotero database " + pathString(zdb));

    auto prefs = readPrefs();
    fs::path baseAttachmentPath = prefs.count("extensions.zotero.baseAttachmentPath")
                                      ? u8path(prefs["extensions.zotero.baseAttachmentPath"])
                                      : fs::path();

    // --- lookup tables ------------------------------------------------------------------
    std::map<int64_t, std::string> fields, itemTypes, creatorTypes, charsets, predicates;
    for (auto [table, map] : std::initializer_list<std::pair<const char*, std::map<int64_t, std::string>*>>{
             {"SELECT fieldID, fieldName FROM fields", &fields},
             {"SELECT itemTypeID, typeName FROM itemTypes", &itemTypes},
             {"SELECT creatorTypeID, creatorType FROM creatorTypes", &creatorTypes},
             {"SELECT charsetID, charset FROM charsets", &charsets},
             {"SELECT predicateID, predicate FROM relationPredicates", &predicates}}) {
        auto st = z.query(table);
        while (st.step())
            (*map)[st.i64(0)] = st.str(1);
    }
    // Zotero 7 custom fields/types (rare) are kept under their names too.
    try {
        auto st = z.query("SELECT customFieldID, fieldName FROM customFields");
        while (st.step())
            fields[st.i64(0)] = st.str(1);
    } catch (const sql::Error&) {
    }

    std::string userId = z.scalarStr("SELECT value FROM settings WHERE setting='account' AND key='userID'").value_or("");
    std::string localUserKey =
        z.scalarStr("SELECT value FROM settings WHERE setting='account' AND key='localUserKey'").value_or("");

    // --- libraries --------------------------------------------------------------------
    std::map<int64_t, ZLibrary> libs;
    {
        std::map<int64_t, std::pair<int64_t, std::string>> groups;
        auto gst = z.query("SELECT libraryID, groupID, name FROM groups");
        while (gst.step())
            groups[gst.i64(0)] = {gst.i64(1), gst.str(2)};
        auto st = z.query("SELECT libraryID, type FROM libraries");
        while (st.step()) {
            ZLibrary l;
            l.zoteroId = st.i64(0);
            l.type = st.str(1);
            std::string uid, name;
            if (l.type == "user") {
                uid = userId.empty() ? "zotero-local" : "zotero-user-" + userId;
                name = "My Library";
                l.uriPrefix = userId.empty() ? "http://zotero.org/users/local/" + localUserKey
                                             : "http://zotero.org/users/" + userId;
            } else if (l.type == "group" && groups.count(l.zoteroId)) {
                l.groupId = groups[l.zoteroId].first;
                uid = "zotero-group-" + std::to_string(l.groupId);
                name = groups[l.zoteroId].second;
                l.uriPrefix = "http://zotero.org/groups/" + std::to_string(l.groupId);
            } else {
                continue;  // feeds are not imported
            }
            auto existing = library.libraryByUid(uid);
            l.localId = existing ? existing->id : library.createLibrary(name, l.type == "group" ? "group" : "user", uid);
            libs[l.zoteroId] = l;
            ++report.libraries;
            log((existing ? "Updating library " : "Creating library ") + name);
        }
    }

    // --- collections --------------------------------------------------------------------
    std::map<int64_t, std::string> collectionKeys;
    std::set<int64_t> deletedCollections;
    {
        auto st = z.query("SELECT collectionID FROM deletedCollections");
        while (st.step())
            deletedCollections.insert(st.i64(0));
        auto ks = z.query("SELECT collectionID, key FROM collections");
        while (ks.step())
            collectionKeys[ks.i64(0)] = ks.str(1);
    }
    std::vector<std::pair<int64_t, Json>> collections;
    {
        auto st = z.query("SELECT collectionID, collectionName, parentCollectionID, clientDateModified, libraryID, key "
                          "FROM collections");
        while (st.step()) {
            if (!libs.count(st.i64(4)) || deletedCollections.count(st.i64(0)))
                continue;
            Json c{{"key", st.str(5)}, {"name", st.str(1)}};
            c["parentCollection"] = st.isNull(2) || !collectionKeys.count(st.i64(2)) || deletedCollections.count(st.i64(2))
                                        ? Json(false)
                                        : Json(collectionKeys[st.i64(2)]);
            c["relations"] = Json::object();
            c["dateModified"] = sqlToIso(st.str(3));
            collections.emplace_back(libs[st.i64(4)].localId, c);
        }
    }

    // --- items --------------------------------------------------------------------------
    struct ZItem {
        int64_t id;
        int64_t lib;
        Json data;
    };
    std::map<int64_t, ZItem> items;
    std::map<int64_t, std::string> keyOf;
    std::set<int64_t> deletedItems;
    {
        auto st = z.query("SELECT itemID FROM deletedItems");
        while (st.step())
            deletedItems.insert(st.i64(0));
    }
    {
        auto st = z.query("SELECT itemID, itemTypeID, libraryID, key, dateAdded, dateModified FROM items");
        while (st.step()) {
            int64_t id = st.i64(0);
            keyOf[id] = st.str(3);
            if (!libs.count(st.i64(2)))
                continue;
            if (!opt.includeTrash && deletedItems.count(id))
                continue;
            auto type = itemTypes[st.i64(1)];
            if (!schema::isValidType(type)) {
                report.warnings.push_back("skipped item " + st.str(3) + " of unknown type " + type);
                continue;
            }
            Json d{{"key", st.str(3)}, {"itemType", type}};
            if (schema::isRegularType(type))
                d["creators"] = Json::array();
            d["tags"] = Json::array();
            d["relations"] = Json::object();
            d["dateAdded"] = sqlToIso(st.str(4));
            d["dateModified"] = sqlToIso(st.str(5));
            if (deletedItems.count(id))
                d["deleted"] = true;
            items[id] = {id, st.i64(2), d};
        }
    }
    auto findItem = [&](int64_t id) -> Json* {
        auto it = items.find(id);
        return it == items.end() ? nullptr : &it->second.data;
    };
    {
        // Zotero stores date fields as "YYYY-MM-DD original text" (a sortable form plus
        // what the user typed) and accessDate as "YYYY-MM-DD HH:MM:SS" in UTC.
        static const std::regex multipart(R"(^\d{4}-\d{2}-\d{2} (.+)$)");
        auto st = z.query("SELECT d.itemID, d.fieldID, v.value FROM itemData d JOIN itemDataValues v USING(valueID)");
        while (st.step())
            if (auto* d = findItem(st.i64(0)))
                if (fields.count(st.i64(1))) {
                    const auto& field = fields[st.i64(1)];
                    std::string value = st.str(2);
                    std::smatch m;
                    if (field == "accessDate")
                        value = sqlToIso(value);
                    else if (std::regex_match(value, m, multipart) && m[1].str().find(':') == std::string::npos)
                        value = m[1].str();
                    (*d)[field] = value;
                }
    }
    {
        auto st = z.query("SELECT ic.itemID, ic.creatorTypeID, c.firstName, c.lastName, c.fieldMode "
                          "FROM itemCreators ic JOIN creators c USING(creatorID) ORDER BY ic.itemID, ic.orderIndex");
        while (st.step())
            if (auto* d = findItem(st.i64(0))) {
                Json c{{"creatorType", creatorTypes[st.i64(1)]}};
                if (st.i64(4) == 1) {
                    c["name"] = st.str(3);
                } else {
                    c["firstName"] = st.str(2);
                    c["lastName"] = st.str(3);
                }
                (*d)["creators"].push_back(c);
            }
    }
    {
        auto st = z.query("SELECT it.itemID, t.name, it.type FROM itemTags it JOIN tags t USING(tagID)");
        while (st.step())
            if (auto* d = findItem(st.i64(0))) {
                Json t{{"tag", st.str(1)}};
                if (st.i64(2))
                    t["type"] = st.i64(2);
                (*d)["tags"].push_back(t);
            }
    }
    {
        auto st = z.query("SELECT itemID, collectionID FROM collectionItems ORDER BY orderIndex");
        while (st.step())
            if (auto* d = findItem(st.i64(0)))
                if (collectionKeys.count(st.i64(1)) && !deletedCollections.count(st.i64(1))) {
                    if (!d->contains("collections"))
                        (*d)["collections"] = Json::array();
                    (*d)["collections"].push_back(collectionKeys[st.i64(1)]);
                }
    }
    {
        auto st = z.query("SELECT itemID, predicateID, object FROM itemRelations");
        while (st.step()) {
            auto it = items.find(st.i64(0));
            if (it == items.end())
                continue;
            auto& lib = libs[it->second.lib];
            std::string value = st.str(2);
            // Same-library item URIs become bare keys.
            if (startsWith(value, lib.uriPrefix + "/items/"))
                value = value.substr(lib.uriPrefix.size() + 7);
            auto& rel = it->second.data["relations"];
            auto pred = predicates[st.i64(1)];
            if (!rel.contains(pred))
                rel[pred] = Json::array();
            rel[pred].push_back(value);
        }
    }
    {
        auto st = z.query("SELECT itemID, parentItemID, note FROM itemNotes");
        while (st.step())
            if (auto* d = findItem(st.i64(0))) {
                if (jstr(*d, "itemType") == "note") {
                    (*d)["note"] = st.str(2);
                    if (!st.isNull(1) && keyOf.count(st.i64(1)))
                        (*d)["parentItem"] = keyOf[st.i64(1)];
                } else if (!st.str(2).empty()) {
                    (*d)["note"] = st.str(2);  // attachment note
                }
            }
    }
    std::map<std::string, std::pair<int64_t, Json*>> storedAttachments;  // key -> (lib, data)
    {
        auto st = z.query("SELECT itemID, parentItemID, linkMode, contentType, charsetID, path, storageModTime, "
                          "storageHash FROM itemAttachments");
        while (st.step()) {
            auto* d = findItem(st.i64(0));
            if (!d)
                continue;
            int mode = st.i32(2);
            std::string linkMode = mode >= 0 && mode < 5 ? kLinkModes[mode] : "imported_file";
            (*d)["linkMode"] = linkMode;
            (*d)["contentType"] = st.str(3);
            if (!st.isNull(4) && charsets.count(st.i64(4)))
                (*d)["charset"] = charsets[st.i64(4)];
            if (!st.isNull(1) && keyOf.count(st.i64(1)))
                (*d)["parentItem"] = keyOf[st.i64(1)];
            std::string path = st.str(5);
            if (startsWith(path, "storage:")) {
                (*d)["filename"] = path.substr(8);
            } else if (linkMode == "linked_file") {
                if (startsWith(path, "attachments:") && !baseAttachmentPath.empty())
                    path = pathString(baseAttachmentPath / u8path(path.substr(12)));
                (*d)["path"] = path;
                (*d)["filename"] = pathString(u8path(path).filename());
            }
            if (!st.isNull(7))
                (*d)["md5"] = st.str(7);
            if (!st.isNull(6))
                (*d)["mtime"] = st.i64(6);
            if (!d->contains("title"))
                (*d)["title"] = jstr(*d, "filename");
            if (schema::isStoredLinkMode(linkMode))
                storedAttachments[jstr(*d, "key")] = {items[st.i64(0)].lib, d};
        }
    }
    int external = 0;
    {
        auto st = z.query("SELECT itemID, parentItemID, type, authorName, text, comment, color, pageLabel, sortIndex, "
                          "position, isExternal FROM itemAnnotations");
        while (st.step()) {
            auto* d = findItem(st.i64(0));
            if (!d)
                continue;
            if (st.i64(10)) {
                // Stored inside the PDF file itself; read-only, like in Zotero.
                (*d)["annotationIsExternal"] = true;
                ++external;
            }
            int type = st.i32(2);
            (*d)["parentItem"] = keyOf[st.i64(1)];
            (*d)["annotationType"] = type >= 1 && type <= 6 ? kAnnotationTypes[type] : "highlight";
            if (!st.isNull(3) && !st.str(3).empty())
                (*d)["annotationAuthorName"] = st.str(3);
            (*d)["annotationText"] = st.str(4);
            (*d)["annotationComment"] = st.str(5);
            (*d)["annotationColor"] = st.isNull(6) ? "#ffd400" : st.str(6);
            (*d)["annotationPageLabel"] = st.str(7);
            (*d)["annotationSortIndex"] = st.str(8);
            try {
                (*d)["annotationPosition"] = Json::parse(st.str(9));
            } catch (...) {
                (*d)["annotationPosition"] = Json::object();
            }
        }
    }
    if (external)
        log(std::to_string(external) + " annotations come from PDF files and are read-only");

    // --- files --------------------------------------------------------------------------
    if (opt.fileMode != ZoteroFileMode::None) {
        int done = 0;
        for (auto& [key, entry] : storedAttachments) {
            if (cancelled())
                throw Error("Import cancelled");
            progress("Copying attachment files", ++done, (int)storedAttachments.size());
            auto [libId, d] = entry;
            auto src = zdir / "storage" / key;
            auto filename = jstr(*d, "filename");
            if (!fs::is_directory(src, ec) || filename.empty() || !fs::is_regular_file(src / u8path(filename), ec)) {
                ++report.filesMissing;
                continue;
            }
            auto dst = library.storageDir() / key;
            int files = 0;
            try {
                report.bytesCopied += copyStorageDir(src, dst, opt.fileMode, files);
            } catch (const std::exception& e) {
                report.warnings.push_back(e.what());
                continue;
            }
            report.filesCopied += files;
            auto main = dst / u8path(filename);
            (*d)["md5"] = library.fileMd5(libId, key, main);
            (*d)["mtime"] = fileMtimeMs(main);
        }
    }

    // --- settings: tag colours and last-read pages ----------------------------------------
    std::vector<std::tuple<int64_t, std::string, Json>> settings;
    {
        std::map<int64_t, int64_t> libByGroup;
        int64_t userLib = 0;
        for (auto& [zid, l] : libs) {
            if (l.type == "user")
                userLib = l.localId;
            else
                libByGroup[l.groupId] = l.localId;
        }
        auto st = z.query("SELECT setting, libraryID, value FROM syncedSettings");
        while (st.step()) {
            auto name = st.str(0);
            Json value;
            try {
                value = Json::parse(st.str(2));
            } catch (...) {
                continue;
            }
            if (name == "tagColors" && libs.count(st.i64(1))) {
                settings.emplace_back(libs[st.i64(1)].localId, "tagColors", value);
            } else if (startsWith(name, "lastPageIndex_u_") && userLib) {
                settings.emplace_back(userLib, "lastPageIndex_" + name.substr(16), value);
            } else if (startsWith(name, "lastPageIndex_g")) {
                auto rest = name.substr(15);
                auto us = rest.find('_');
                if (us != std::string::npos) {
                    auto gid = std::atoll(rest.substr(0, us).c_str());
                    if (libByGroup.count(gid))
                        settings.emplace_back(libByGroup[gid], "lastPageIndex_" + rest.substr(us + 1), value);
                }
            }
        }
        // Local reader state is more recent than the synced setting.
        for (auto& [key, entry] : storedAttachments) {
            auto state = zdir / "storage" / key / ".zotero-reader-state";
            if (!fs::is_regular_file(state, ec))
                continue;
            try {
                auto j = Json::parse(readFile(state));
                if (j.contains("pageIndex") && j["pageIndex"].is_number())
                    settings.emplace_back(entry.first, "lastPageIndex_" + key, j["pageIndex"]);
            } catch (...) {
            }
        }
    }

    // --- write everything -------------------------------------------------------------------
    {
        Library::BatchGuard batch(library);
        sql::Transaction tx(library.db());
        int done = 0;
        int total = (int)(collections.size() + items.size());
        for (auto& [libId, c] : collections) {
            progress("Importing collections", ++done, total);
            auto existing = library.collection(libId, jstr(c, "key"));
            if (existing && jstr(*existing, "dateModified") >= jstr(c, "dateModified")) {
                ++report.unchanged;
                continue;
            }
            library.importObject(libId, "collection", c);
            ++report.collections;
        }
        for (auto& [id, it] : items) {
            if (done % 100 == 0)
                progress("Importing items", done, total);
            ++done;
            auto libId = libs[it.lib].localId;
            auto& d = it.data;
            auto type = jstr(d, "itemType");
            if (type == "annotation" && jstr(d, "parentItem").empty())
                continue;
            if (auto existing = library.item(libId, jstr(d, "key"))) {
                if (jstr(*existing, "dateModified") >= jstr(d, "dateModified") &&
                    jstr(*existing, "md5") == jstr(d, "md5")) {
                    ++report.unchanged;
                    continue;
                }
                ++report.updated;
            }
            library.importObject(libId, "item", d);
            if (type == "note")
                ++report.notes;
            else if (type == "attachment")
                ++report.attachments;
            else if (type == "annotation")
                ++report.annotations;
            else
                ++report.items;
        }
        for (auto& [libId, name, value] : settings) {
            if (name == "tagColors" || library.setting(libId, name).is_null())
                library.setSetting(libId, name, value);
        }
        tx.commit();
    }

    // --- citation styles ------------------------------------------------------------------
    if (opt.importStyles && fs::is_directory(zdir / "styles", ec)) {
        auto dst = library.dataDir() / "styles";
        fs::create_directories(dst);
        for (auto& e : fs::directory_iterator(zdir / "styles", ec)) {
            if (e.path().extension() == ".csl") {
                fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing, ec);
                if (!ec)
                    ++report.styles;
            }
        }
    }
    log("Zotero import finished: " + report.summary());
    return report;
}

} // namespace fp
