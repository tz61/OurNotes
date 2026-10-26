#include <doctest.h>

#include <cstdlib>
#include <fstream>
#include <functional>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "core/config.h"
#include "core/fulltext.h"
#include "core/library.h"
#include "test_helpers.h"

using namespace on;

TEST_CASE("items: create, edit, derived columns, search")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    CHECK(lib.libraries().size() == 1);

    auto item = lib.createItem(id, Json{{"itemType", "journalArticle"},
                                        {"title", "Attention Is All You Need"},
                                        {"date", "2017-06-12"},
                                        {"publicationTitle", "NeurIPS"},
                                        {"creators", Json::array({Json{{"creatorType", "author"},
                                                                       {"firstName", "Ashish"},
                                                                       {"lastName", "Vaswani"}}})}});
    auto key = jstr(item, "key");
    CHECK(key.size() == 8);
    auto rows = lib.itemRows(id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].title == "Attention Is All You Need");
    CHECK(rows[0].creator == "Vaswani");
    CHECK(rows[0].year == "2017");
    CHECK(rows[0].publication == "NeurIPS");

    CHECK(lib.search(id, "attention vaswani", SearchMode::TitleCreatorYear).count(key));
    CHECK(lib.search(id, "neurips", SearchMode::TitleCreatorYear).empty());
    CHECK(lib.search(id, "neurips", SearchMode::AllFields).count(key));

    auto note = lib.createNote(id, "<p>Transformers rock</p>", key);
    CHECK(lib.children(id, key).size() == 1);
    CHECK(lib.search(id, "rock", SearchMode::AllFields).count(jstr(note, "key")));
    CHECK(lib.topLevelKey(id, jstr(note, "key")) == key);

    lib.updateItem(id, key, [](Json& d) { d["title"] = "Changed"; });
    CHECK(jstr(*lib.item(id, key), "title") == "Changed");

    lib.trashItems(id, {key});
    CHECK(jbool(*lib.item(id, key), "deleted"));
    lib.emptyTrash(id);
    CHECK(!lib.item(id, key));
    CHECK(!lib.item(id, jstr(note, "key")));  // children erased too
}

TEST_CASE("libraries: a database from before synced names is upgraded")
{
    TempDir tmp;
    {
        Library lib(tmp.path);
        lib.defaultLibrary();
        lib.db().exec("ALTER TABLE libraries DROP COLUMN sync_name");
        lib.db().setUserVersion(1);
    }
    Library lib(tmp.path);
    auto info = lib.libraries().at(0);
    CHECK(info.name == Library::kDefaultName);
    CHECK(info.syncName.empty());
    lib.setSyncName(info.id, "Main");
    CHECK(lib.library(info.id)->syncName == "Main");
}

TEST_CASE("items: notes and attachments move to another item, or out on their own")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto coll = jstr(lib.createCollection(id, "Datasheet"), "key");
    auto book = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "TMS"}}), "key");
    auto other = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "Other"}}), "key");
    auto pdf = jstr(lib.createItem(id, Json{{"itemType", "attachment"}, {"linkMode", "linked_file"}, {"title", "tms.pdf"}}),
                    "key");
    lib.addToCollection(id, coll, {pdf});
    auto note = jstr(lib.createNote(id, "<p>n</p>", other), "key");

    CHECK(lib.moveToParent(id, {pdf, note, other}, book) == 2);  // a regular item stays where it is
    CHECK(jstr(*lib.item(id, pdf), "parentItem") == book);
    CHECK(jstr(*lib.item(id, note), "parentItem") == book);
    CHECK(!lib.item(id, pdf)->contains("collections"));  // in its parent's collections now
    CHECK(!lib.collectionItemKeys(id, coll).count(pdf));
    CHECK(jstr(*lib.item(id, other), "parentItem").empty());

    CHECK(lib.moveToParent(id, {pdf}, note) == 0);  // only a regular item takes children
    CHECK(lib.moveToParent(id, {pdf}, "", coll) == 1);
    CHECK(jstr(*lib.item(id, pdf), "parentItem").empty());
    CHECK(lib.collectionItemKeys(id, coll).count(pdf));
    CHECK(lib.dirtyObjects(id).size() >= 2);  // synced like any edit
}

TEST_CASE("collections and tags")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto a = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "A"}}), "key");
    auto b = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "B"}}), "key");
    auto parent = lib.createCollection(id, "Parent");
    auto child = lib.createCollection(id, "Child", jstr(parent, "key"));
    lib.addToCollection(id, jstr(child, "key"), {a});
    lib.addToCollection(id, jstr(parent, "key"), {b});
    CHECK(lib.collectionItemKeys(id, jstr(parent, "key")).size() == 1);
    CHECK(lib.collectionItemKeys(id, jstr(parent, "key"), true).size() == 2);
    CHECK(lib.unfiledItemKeys(id).empty());

    lib.addTag(id, {a, b}, "ml");
    lib.addTag(id, {a}, "nlp");
    CHECK(lib.itemKeysWithTags(id, {"ml"}).size() == 2);
    CHECK(lib.itemKeysWithTags(id, {"ml", "nlp"}) == std::set<std::string>{a});
    lib.setTagColor(id, "ml", "#ff6666");
    lib.renameTag(id, "ml", "machine learning");
    CHECK(lib.itemKeysWithTags(id, {"machine learning"}).size() == 2);
    CHECK(lib.tagColors(id).at("machine learning") == "#ff6666");

    lib.deleteCollection(id, jstr(parent, "key"));
    CHECK(lib.collections(id).empty());
    CHECK(lib.unfiledItemKeys(id).size() == 2);
}

TEST_CASE("attachments are copied into storage")
{
    TempDir tmp;
    Library lib(tmp.path / "data");
    auto id = lib.defaultLibrary();
    auto src = tmp.path / "paper.pdf";
    {
        std::ofstream f(src, std::ios::binary);
        f << "%PDF-1.4 fake";
    }
    auto att = lib.importAttachment(id, src);
    CHECK(jstr(att, "linkMode") == "imported_file");
    CHECK(jstr(att, "contentType") == "application/pdf");
    CHECK(jstr(att, "md5") == md5Hex("%PDF-1.4 fake"));
    auto path = lib.attachmentFile(id, att);
    CHECK(fs::exists(path));
    CHECK(lib.bestAttachment(id, jstr(att, "key")).has_value());
    {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f << " modified";
    }
    CHECK(lib.refreshAttachmentFile(id, jstr(att, "key")));
    lib.eraseItems(id, {jstr(att, "key")});
    CHECK(!fs::exists(path));
}

TEST_CASE("sync bookkeeping: dirty objects, tombstones, markPushed")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto key = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "A"}}), "key");
    auto dirty = lib.dirtyObjects(id);
    REQUIRE(dirty.size() == 1);
    lib.markPushed(id, "item", key, 5, dirty[0].rev);
    CHECK(lib.dirtyObjects(id).empty());
    CHECK(lib.getObject(id, "item", key)->version == 5);

    // an edit between snapshot and markPushed keeps the object dirty
    lib.updateItem(id, key, [](Json& d) { d["title"] = "B"; });
    auto rev = lib.dirtyObjects(id)[0].rev;
    lib.updateItem(id, key, [](Json& d) { d["title"] = "C"; });
    lib.markPushed(id, "item", key, 6, rev);
    CHECK(lib.dirtyObjects(id).size() == 1);

    lib.eraseItems(id, {key});
    auto tombs = lib.tombstones(id);
    REQUIRE(tombs.size() == 1);
    CHECK(tombs[0].version == 6);

    // never-synced objects leave no tombstone
    auto k2 = jstr(lib.createItem(id, Json{{"itemType", "book"}}), "key");
    lib.eraseItems(id, {k2});
    CHECK(lib.tombstones(id).size() == 1);
}

TEST_CASE("libraries: a program done writing is counted, for the app writing at the same time")
{
    TempDir tmp;
    Library app(tmp.path);
    auto id = app.defaultLibrary();
    auto key = jstr(app.createItem(id, Json{{"itemType", "book"}, {"title", "A"}}), "key");
    CHECK(app.programWrites() == 0);
    auto program = [&](const std::function<void(Library&)>& does) {
        Library cli(tmp.path);
        cli.setOrigin("ournotes-cli");
        does(cli);
    };
    program([&](Library& cli) { cli.updateItem(id, key, [](Json& d) { d["title"] = "B"; }); });
    CHECK(app.programWrites() == 1);
    program([&](Library& cli) { CHECK(cli.item(id, key)); });  // read only
    CHECK(app.programWrites() == 1);
    program([&](Library& cli) {
        cli.updateItem(id, key, [](Json& d) { d["title"] = "C"; });
        cli.createItem(id, Json{{"itemType", "book"}});
    });
    CHECK(app.programWrites() == 2);
    {
        Library thread(tmp.path);  // the app's, on another connection
        thread.updateItem(id, key, [](Json& d) { d["title"] = "D"; });
    }
    CHECK(app.programWrites() == 2);
}

TEST_CASE("duplicates and merge")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto a = jstr(lib.createItem(id, Json{{"itemType", "journalArticle"}, {"title", "Deep Residual Learning"}, {"date", "2016"}}), "key");
    auto b = jstr(lib.createItem(id, Json{{"itemType", "journalArticle"}, {"title", "Deep residual learning."}, {"DOI", "10.1/x"}}), "key");
    lib.createItem(id, Json{{"itemType", "journalArticle"}, {"title", "Something else entirely"}});
    lib.createNote(id, "<p>child</p>", b);
    auto groups = lib.duplicateGroups(id);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].size() == 2);
    lib.mergeItems(id, a, {b});
    CHECK(jstr(*lib.item(id, a), "DOI") == "10.1/x");
    CHECK(lib.children(id, a).size() == 1);
    CHECK(jbool(*lib.item(id, b), "deleted"));
}

TEST_CASE("fulltext search")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto key = jstr(lib.createItem(id, Json{{"itemType", "attachment"}, {"linkMode", "imported_file"}, {"title", "PDF"}}), "key");
    lib.fulltext().store(id, key, "abc", "The mitochondria is the powerhouse of the cell", 1);
    CHECK(lib.fulltext().isIndexed(id, key, "abc"));
    CHECK(lib.search(id, "powerhouse mito", SearchMode::Everything).count(key));
    CHECK(lib.search(id, "powerhouse", SearchMode::AllFields).empty());
    CHECK(lib.search(id, "\"the cell\"", SearchMode::Everything).count(key));
}

TEST_CASE("search minding case: each word as typed, in the fields and the full text")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto upper = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "Robot Arms"}}), "key");
    auto lower = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "robot legs"}, {"publisher", "MIT Press"}}), "key");
    auto pdf = jstr(lib.createItem(id, Json{{"itemType", "attachment"}, {"linkMode", "imported_file"}, {"title", "PDF"}}), "key");
    lib.fulltext().store(id, pdf, "abc", "Training a Transformer on DNA", 1);
    CHECK(lib.search(id, "robot", SearchMode::TitleCreatorYear) == std::set<std::string>{upper, lower});
    CHECK(lib.search(id, "Robot", SearchMode::TitleCreatorYear, true) == std::set<std::string>{upper});
    CHECK(lib.search(id, "robot", SearchMode::TitleCreatorYear, true) == std::set<std::string>{lower});
    CHECK(lib.search(id, "robot Arms", SearchMode::TitleCreatorYear, true).empty());  // every word
    CHECK(lib.search(id, "MIT", SearchMode::AllFields, true) == std::set<std::string>{lower});
    CHECK(lib.search(id, "mit", SearchMode::AllFields, true).empty());
    CHECK(lib.search(id, "DNA", SearchMode::Everything, true) == std::set<std::string>{pdf});
    CHECK(lib.search(id, "dna", SearchMode::Everything, true).empty());
    CHECK(lib.search(id, "dna", SearchMode::Everything) == std::set<std::string>{pdf});
}

TEST_CASE("reader state is kept next to the attachment, like Zotero's")
{
    TempDir tmp;
    Library lib(tmp.path);
    CHECK(lib.readerState("ABCD2345").empty());
    Json state{{"pageIndex", 8}, {"scale", "page-width"}, {"top", 515.3}, {"left", -6.9}};
    lib.setReaderState("ABCD2345", state);
    CHECK(fs::exists(lib.storageDir() / "ABCD2345" / ".zotero-reader-state"));
    CHECK(lib.readerState("ABCD2345") == state);
    // keys that are not item keys never address files
    lib.setReaderState("../evil", state);
    CHECK(!fs::exists(tmp.path / "evil"));
    CHECK(lib.readerState("../evil").empty());
}

#ifndef _WIN32
// where freePen kept its settings under the home directory $HOME names
static fs::path freePenConfigDir(const fs::path& home)
{
#ifdef __APPLE__
    return home / "Library" / "Application Support" / "freePen";
#else
    return home / ".config" / "freepen";
#endif
}

TEST_CASE("settings and library from freePen, the name before OurNotes, are taken over")
{
    // a home directory of its own, so the real one is never touched
    TempDir home;
    std::string oldHome = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char* oldXdg = std::getenv("XDG_CONFIG_HOME");
    std::string savedXdg = oldXdg ? oldXdg : "";
    const char* oldData = std::getenv("OURNOTES_DATA_DIR");
    std::string savedData = oldData ? oldData : "";
    setenv("HOME", pathString(home.path).c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("OURNOTES_DATA_DIR");

    std::string key;
    {
        Library old(home.path / "freePen");
        key = jstr(old.createItem(old.defaultLibrary(), Json{{"itemType", "book"}, {"title", "Kept"}}), "key");
    }
    fs::rename(home.path / "freePen" / "ournotes.sqlite", home.path / "freePen" / "freepen.sqlite");
    fs::create_directories(freePenConfigDir(home.path));
    writeFileAtomic(freePenConfigDir(home.path) / "config.json", R"({"ui": {"scale": 175}, "dataDir": null})");

    migrateFromFreePen();
    auto config = configDir() / "config.json";
    CHECK(fs::exists(config));
    CHECK(fs::exists(freePenConfigDir(home.path) / "config.json"));  // the old settings stay as they were
    CHECK(Config(config).get("/ui/scale").get<int>() == 175);
    CHECK(!fs::exists(home.path / "freePen"));
    REQUIRE(fs::exists(home.path / "OurNotes" / "freepen.sqlite"));
    {
        Library lib(Config(config).dataDir());
        CHECK(fs::exists(home.path / "OurNotes" / "ournotes.sqlite"));
        CHECK(!fs::exists(home.path / "OurNotes" / "freepen.sqlite"));
        auto libs = lib.libraries();
        REQUIRE(!libs.empty());
        REQUIRE(lib.item(libs[0].id, key));
        CHECK(jstr(*lib.item(libs[0].id, key), "title") == "Kept");
    }
    // a second start changes nothing
    migrateFromFreePen();
    CHECK(fs::exists(home.path / "OurNotes" / "ournotes.sqlite"));

    // a data directory chosen in the settings is left where it is
    TempDir home2;
    setenv("HOME", pathString(home2.path).c_str(), 1);
    fs::create_directories(home2.path / "freePen");
    fs::create_directories(freePenConfigDir(home2.path));
    writeFileAtomic(freePenConfigDir(home2.path) / "config.json",
                    Json{{"dataDir", pathString(home2.path / "papers")}}.dump());
    migrateFromFreePen();
    CHECK(fs::exists(home2.path / "freePen"));
    CHECK(!fs::exists(home2.path / "OurNotes"));

    setenv("HOME", oldHome.c_str(), 1);
    if (oldXdg)
        setenv("XDG_CONFIG_HOME", savedXdg.c_str(), 1);
    if (oldData)
        setenv("OURNOTES_DATA_DIR", savedData.c_str(), 1);
}
#endif

TEST_CASE("items copied into another library, with their notes, images, files and annotations")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto from = lib.defaultLibrary();
    auto to = lib.createLibrary("Group");
    auto into = jstr(lib.createCollection(to, "Reading"), "key");
    auto other = jstr(lib.createItem(from, Json{{"itemType", "book"}, {"title", "Other"}}), "key");
    auto paper = lib.createItem(from, Json{{"itemType", "journalArticle"},
                                           {"title", "Paper"},
                                           {"tags", Json::array({Json{{"tag", "robots"}}})}});
    auto paperKey = jstr(paper, "key");
    lib.addRelated(from, paperKey, other);
    // a PDF with an annotation
    auto pdfPath = tmp.path / "paper.pdf";
    std::ofstream(pdfPath, std::ios::binary) << "%PDF-1.4 the paper";
    auto pdf = lib.importAttachment(from, pdfPath, paperKey);
    auto ann = lib.createItem(from, Json{{"itemType", "annotation"},
                                         {"parentItem", jstr(pdf, "key")},
                                         {"annotationType", "highlight"},
                                         {"annotationComment", "Why?"}});
    // a note with an image of its own, and one in the trash
    auto note = lib.createNote(from, "", paperKey);
    auto imageKey = newKey();
    fs::create_directories(lib.storageDir() / imageKey);
    std::ofstream(lib.storageDir() / imageKey / "image.png", std::ios::binary) << "PNG";
    lib.createItem(from, Json{{"key", imageKey},
                              {"itemType", "attachment"},
                              {"linkMode", "embedded_image"},
                              {"parentItem", jstr(note, "key")},
                              {"contentType", "image/png"},
                              {"filename", "image.png"}});
    lib.updateItem(from, jstr(note, "key"), [&](Json& d) {
        d["note"] = "<p>See</p><img data-attachment-key=\"" + imageKey + "\">";
    });
    auto trashed = lib.createNote(from, "<p>gone</p>", paperKey);
    lib.trashItems(from, {jstr(trashed, "key")});
    // a PDF of another item, copied on its own
    auto lonePath = tmp.path / "lone.pdf";
    std::ofstream(lonePath, std::ios::binary) << "%PDF-1.4 lone";
    auto lone = lib.importAttachment(from, lonePath, other);

    // the paper and its PDF (under it, so once), and the other's PDF
    auto copied = lib.copyItems(from, {paperKey, jstr(pdf, "key"), jstr(lone, "key")}, to, into);
    REQUIRE(copied.size() == 2);
    auto copy = lib.item(to, copied[0]);
    REQUIRE(copy);
    CHECK(copied[0] != paperKey);
    CHECK(jstr(*copy, "title") == "Paper");
    CHECK((*copy)["tags"] == paper["tags"]);
    CHECK((*copy)["collections"] == Json::array({into}));
    CHECK(lib.relatedKeys(to, copied[0]).empty());
    auto kids = lib.children(to, copied[0]);
    REQUIRE(kids.size() == 2);  // the PDF and the note, not the trashed note
    auto pdfCopy = jstr(kids[0], "itemType") == "attachment" ? kids[0] : kids[1];
    auto noteCopy = jstr(kids[0], "itemType") == "note" ? kids[0] : kids[1];
    CHECK(jstr(pdfCopy, "key") != jstr(pdf, "key"));
    CHECK(fs::exists(lib.attachmentFile(to, pdfCopy)));
    CHECK(lib.attachmentFile(to, pdfCopy) != lib.attachmentFile(from, pdf));
    CHECK(jstr(pdfCopy, "md5") == jstr(pdf, "md5"));
    auto anns = lib.annotations(to, jstr(pdfCopy, "key"));
    REQUIRE(anns.size() == 1);
    CHECK(jstr(anns[0], "annotationComment") == "Why?");
    CHECK(jstr(anns[0], "key") != jstr(ann, "key"));
    auto images = lib.children(to, jstr(noteCopy, "key"));
    REQUIRE(images.size() == 1);
    auto imageCopy = jstr(images[0], "key");
    CHECK(imageCopy != imageKey);
    CHECK(jstr(noteCopy, "note") == "<p>See</p><img data-attachment-key=\"" + imageCopy + "\">");
    CHECK(fs::exists(lib.storageDir() / imageCopy / "image.png"));
    // the other's PDF: on its own, in the collection
    auto loneCopy = lib.item(to, copied[1]);
    REQUIRE(loneCopy);
    CHECK(!loneCopy->contains("parentItem"));
    CHECK((*loneCopy)["collections"] == Json::array({into}));
    CHECK(fs::exists(lib.attachmentFile(to, *loneCopy)));
    // and nothing changed where they came from
    CHECK(lib.children(from, paperKey).size() == 3);
    CHECK(lib.relatedKeys(from, paperKey) == std::vector<std::string>{other});
    CHECK(jstr(*lib.item(from, jstr(lone, "key")), "parentItem") == other);
}

TEST_CASE("items copied into another library: files not here listed first; one that cannot be copied leaves no copies")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto from = lib.defaultLibrary();
    auto to = lib.createLibrary("Group");
    auto paper = jstr(lib.createItem(from, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    auto file = tmp.path / "paper.pdf";
    std::ofstream(file, std::ios::binary) << "%PDF-1.4 the paper";
    auto first = lib.importAttachment(from, file, paper);
    auto second = lib.importAttachment(from, file, paper);
    auto linked = lib.createItem(from, Json{{"itemType", "attachment"},
                                            {"parentItem", paper},
                                            {"linkMode", "linked_file"},
                                            {"path", pathString(tmp.path / "elsewhere.pdf")}});
    CHECK(lib.filesNotHere(from, {paper}).empty());
    // synced, not downloaded: its folder not here (a linked file is not the library's to fetch)
    fs::remove_all(lib.attachmentFile(from, second).parent_path());
    CHECK(lib.filesNotHere(from, {paper}) == std::vector<std::string>{jstr(second, "key")});
    CHECK(lib.filesNotHere(from, {jstr(second, "key"), paper}) == std::vector<std::string>{jstr(second, "key")});
    CHECK(lib.filesNotHere(from, {jstr(first, "key"), jstr(linked, "key")}).empty());
    lib.trashItems(from, {jstr(second, "key")});
    CHECK(lib.filesNotHere(from, {paper}).empty());  // (not copied either)
    lib.trashItems(from, {jstr(second, "key")}, false);

#ifndef _WIN32
    if (geteuid() != 0) {  // (root reads it all the same)
        // the second's file there again, but not to be read: nothing copied, no files left
        fs::create_directories(lib.attachmentFile(from, second).parent_path());
        std::ofstream(lib.attachmentFile(from, second), std::ios::binary) << "%PDF-1.4 locked";
        fs::permissions(lib.attachmentFile(from, second), fs::perms::none);
        auto folders = [&] {
            size_t n = 0;
            for (auto& e : fs::directory_iterator(lib.storageDir()))
                n += e.is_directory();
            return n;
        };
        auto before = folders();
        CHECK_THROWS(lib.copyItems(from, {paper}, to));
        fs::permissions(lib.attachmentFile(from, second), fs::perms::owner_all);
        CHECK(lib.allItems(to).empty());
        CHECK(folders() == before);
        // and once it can be read, all of it
        REQUIRE(lib.copyItems(from, {paper}, to).size() == 1);
        CHECK(lib.allItems(to).size() == 4);
        CHECK(folders() == before + 2);
    }
#endif
}

TEST_CASE("disk usage: each library's items and files, the databases, the disk")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "Paper"}}), "key");
    lib.createNote(id, "<p>a note</p>", paper);
    auto pdfPath = tmp.path / "paper.pdf";
    std::ofstream(pdfPath, std::ios::binary) << std::string(5000, 'x');
    auto pdf = lib.importAttachment(id, pdfPath, paper);
    lib.createItem(id, Json{{"itemType", "annotation"}, {"parentItem", jstr(pdf, "key")}, {"annotationType", "highlight"}});
    // a file not downloaded (synced "on demand"), and an item in the trash
    lib.createItem(id, Json{{"itemType", "attachment"}, {"linkMode", "imported_file"}, {"filename", "away.pdf"}});
    auto gone = jstr(lib.createItem(id, Json{{"itemType", "book"}, {"title", "Gone"}}), "key");
    lib.trashItems(id, {gone});
    auto usage = lib.diskUsage();
    CAPTURE(usage.dump(2));
    REQUIRE(usage["libraries"].size() == 1);
    auto& l = usage["libraries"][0];
    CHECK(l["items"] == 1);
    CHECK(l["notes"] == 1);
    CHECK(l["attachments"] == 2);
    CHECK(l["annotations"] == 1);
    CHECK(l["trashed"] == 1);
    CHECK(l["files"] == 1);
    CHECK(l["fileBytes"] == 5000);
    CHECK(l["missingFiles"] == 1);
    CHECK(l["missing"] == Json::array({"away.pdf"}));
    CHECK(usage["storage"]["files"] == 1);
    CHECK(usage["storage"]["bytes"] == 5000);
    CHECK(usage["database"].get<int64_t>() > 0);
    CHECK(usage["disk"]["capacity"].get<int64_t>() > 0);
    CHECK(usage["disk"]["available"].get<int64_t>() <= usage["disk"]["capacity"].get<int64_t>());
}

TEST_CASE("items: an attachment whose data cannot be read leaves the item list readable")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(id, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    lib.createItem(id, Json{{"itemType", "attachment"},
                            {"parentItem", paper},
                            {"linkMode", "linked_file"},
                            {"title", "Fine"},
                            {"filename", "fine.pdf"},
                            {"path", "/somewhere/fine.pdf"}});
    // one nested deeper than SQLite reads JSON (from an old build, or a synced folder)
    Json deep = Json::array();
    for (int i = 0; i < 1100; ++i)
        deep = Json::array({deep});
    lib.createItem(id, Json{{"itemType", "attachment"}, {"parentItem", paper}, {"linkMode", "linked_file"},
                            {"title", "Deep"}, {"filename", "deep.pdf"}, {"deep", deep}});
    std::vector<ItemRow> rows;
    REQUIRE_NOTHROW(rows = lib.itemRows(id));
    CHECK(rows.size() == 3);
    bool fine = false;
    for (auto& r : rows)
        fine |= r.filename == "fine.pdf" && r.path == "/somewhere/fine.pdf";
    CHECK(fine);
}

TEST_CASE("items: a stored file kept under its own long name (as Zotero's importer copies it) is found")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    std::string title;
    for (int i = 0; i < 80; ++i)
        title += "\u957f";  // (3 bytes each: 240)
    auto name = title + ".pdf";
    auto att = lib.createItem(id, Json{{"itemType", "attachment"}, {"linkMode", "imported_file"}, {"title", "Long"},
                                       {"contentType", "application/pdf"}, {"filename", name}});
    auto key = jstr(att, "key");
    auto dir = lib.storageDir() / key;
    fs::create_directories(dir);
    std::ofstream(dir / u8path(name)) << "%PDF-1.4";
    CHECK(lib.attachmentFileExists(id, att));
    CHECK(lib.attachmentFile(id, att) == dir / u8path(name));
    // one under the shortened name is still the one: the file this app keeps
    std::ofstream(dir / u8path(safeFilename(name))) << "%PDF-1.4";
    CHECK(lib.attachmentFile(id, att) == dir / u8path(safeFilename(name)));
    // and no name leads out of the item's folder
    auto out = att;
    out["filename"] = "../" + std::string(250, 'x') + ".pdf";
    std::ofstream(lib.storageDir() / (std::string(250, 'x') + ".pdf")) << "x";
    CHECK(lib.attachmentFile(id, out).parent_path() == dir);
}
