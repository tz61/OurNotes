#include <doctest.h>

#include <fstream>

#include "core/fulltext.h"
#include "core/library.h"
#include "test_helpers.h"

using namespace fp;

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
