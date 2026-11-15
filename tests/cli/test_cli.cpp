// ournotes-cli as an agent uses it (docs/agents.md): the commands run in-process, their
// output captured.
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <tuple>

#include "../core/test_helpers.h"
#include "../pdf/sample_pdf.h"
#include "cli/commands.h"
#include "cli/mcp_server.h"
#include "core/config.h"
#include "core/library.h"
#include "core/sync/history.h"
#include "pdf/document.h"

using namespace on;

namespace {

struct Cli {
    TempDir dir;
    Library lib{dir.path / "data"};
    Config config{dir.path / "config.json"};
    CommandMap commands;

    Cli()
    {
        registerAllCommands(commands, config);
    }

    // ournotes-cli <words...> with `input` on standard input: {exit code, output}
    std::pair<int, std::string> run(std::vector<std::string> words, const std::string& input = {})
    {
        words.insert(words.begin(), "ournotes-cli");
        std::vector<char*> argv;
        for (auto& w : words)
            argv.push_back(w.data());
        Args args((int)argv.size(), argv.data());
        std::ostringstream out;
        std::istringstream in(input);
        auto* oldOut = std::cout.rdbuf(out.rdbuf());
        auto* oldIn = std::cin.rdbuf(in.rdbuf());
        int code = 0;
        try {
            code = commands.at(args.command)(lib, args);
        } catch (const std::exception& e) {
            out << "error: " << e.what() << "\n";
            code = 1;
        }
        std::cout.rdbuf(oldOut);
        std::cin.rdbuf(oldIn);
        return {code, out.str()};
    }
    Json json(const std::vector<std::string>& words)
    {
        auto [code, out] = run(words);
        INFO(out);
        REQUIRE(code == 0);
        return Json::parse(out);
    }
};

} // namespace

TEST_CASE("cli: collections by path, items as JSON, in any library")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto course = lib.createLibrary("Course");
    auto fa26 = jstr(lib.createCollection(course, "FA26"), "key");
    auto cs8803 = jstr(lib.createCollection(course, "CS8803", fa26), "key");
    lib.createCollection(mine, "FA26");  // the same name in another library
    auto paper = lib.createItem(course, Json{{"itemType", "journalArticle"},
                                             {"title", "Paper One"},
                                             {"collections", Json::array({cs8803})}});
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto pdf = lib.importAttachment(course, pdfPath, jstr(paper, "key"));
    lib.createItem(course, Json{{"itemType", "annotation"},
                                {"parentItem", jstr(pdf, "key")},
                                {"annotationType", "highlight"},
                                {"annotationText", "Hello World"},
                                {"annotationComment", "the greeting"},
                                {"annotationPageLabel", "1"}});

    auto colls = cli.json({"collections", "--json"});
    bool listed = false;
    for (auto& c : colls)
        listed |= c["path"] == "FA26/CS8803" && c["library"] == "Course" && c["items"] == 1;
    CHECK(listed);

    auto items = cli.json({"items", "--collection", "FA26/CS8803", "--json"});
    REQUIRE(items.size() == 1);
    CHECK(items[0]["key"] == jstr(paper, "key"));
    CHECK(items[0]["library"] == "Course");
    REQUIRE(items[0]["attachments"].size() == 1);
    CHECK(items[0]["attachments"][0]["exists"] == true);
    CHECK(items[0]["attachments"][0]["annotations"] == 1);
    // in another case, with the library's name, by key
    CHECK(cli.json({"items", "--collection", "fa26/cs8803", "--json"}).size() == 1);
    CHECK(cli.json({"items", "--collection", "Course/FA26/CS8803", "--json"}).size() == 1);
    CHECK(cli.json({"items", "--collection", cs8803, "--json"}).size() == 1);
    // "FA26" is in two libraries: which, it asks
    auto [code, out] = cli.run({"items", "--collection", "FA26"});
    CHECK(code == 1);
    CHECK(out.find("Course/FA26") != std::string::npos);
    CHECK(out.find("My Library/FA26") != std::string::npos);
    CHECK(cli.json({"items", "--collection", "FA26", "--library", "course", "--json"}).size() == 1);

    // the PDF's text and its annotations, for a summary
    auto [textCode, text] = cli.run({"text", jstr(paper, "key")});
    CHECK(textCode == 0);
    CHECK(text.find("[page 1]") != std::string::npos);
    CHECK(text.find("Hello World from OurNotes") != std::string::npos);
    auto anns = cli.json({"annotations", jstr(paper, "key"), "--json"});
    REQUIRE(anns.size() == 1);
    CHECK(anns[0]["comment"] == "the greeting");
    // exported by key, from its library
    auto csl = cli.json({"export", jstr(paper, "key"), "--format", "csljson"});
    REQUIRE(csl.size() == 1);
    CHECK(csl[0]["title"] == "Paper One");
    auto shown = cli.json({"show", jstr(paper, "key"), "--json"});
    CHECK(shown["item"]["title"] == "Paper One");
    CHECK(shown["attachments"][0]["path"].get<std::string>().find("one.pdf") != std::string::npos);
}

TEST_CASE("cli: the guide for agents is built in")
{
    Cli cli;
    auto [code, out] = cli.run({"guide"});
    CHECK(code == 0);
    CHECK(out.find("# Working with an OurNotes library from an agent or a script") == 0);
    CHECK(out.find("ournotes-cli add-note") != std::string::npos);
    // ending with where things are on this computer
    CHECK(out.find("## This installation") != std::string::npos);
    auto [whereCode, where] = cli.run({"where"});
    CHECK(whereCode == 0);
    CHECK(where.find("data folder:   " + pathString(cli.lib.dataDir())) != std::string::npos);
    CHECK(where.find("settings:      " + pathString(cli.dir.path / "config.json")) != std::string::npos);
    CHECK(where.find("sync:          none set up") != std::string::npos);
    CHECK(where.find("ournotes-cli:  ") != std::string::npos);
}

TEST_CASE("cli: titles, tags, notes, collections and the trash, marked for the next sync")
{
    Cli cli;
    auto& lib = cli.lib;
    auto course = lib.createLibrary("Course");
    auto fa26 = jstr(lib.createCollection(course, "FA26"), "key");
    auto cs8803 = jstr(lib.createCollection(course, "CS8803", fa26), "key");
    std::vector<std::string> keys;
    for (auto t : {"lecture 1", "lecture 2"})
        keys.push_back(jstr(lib.createItem(course, Json{{"itemType", "report"},
                                                        {"title", t},
                                                        {"collections", Json::array({cs8803})}}),
                            "key"));
    auto synced = [&](const std::string& key) {
        for (auto& d : lib.dirtyObjects(course))
            if (d.key == key)
                return false;
        return true;
    };
    for (auto& d : lib.dirtyObjects(course))  // as after a sync
        if (d.key == keys[0])
            lib.markPushed(course, d.type, d.key, 1, d.rev);
    REQUIRE(synced(keys[0]));

    // rename every title in the collection
    for (auto& item : cli.json({"items", "--collection", "FA26/CS8803", "--json"})) {
        auto [code, out] = cli.run({"set", item["key"], "title=CS8803: " + item["title"].get<std::string>()});
        INFO(out);
        CHECK(code == 0);
    }
    CHECK(jstr(*lib.item(course, keys[0]), "title") == "CS8803: lecture 1");
    CHECK(!synced(keys[0]));  // goes with the next sync
    auto [badCode, bad] = cli.run({"set", keys[0], "bogus=1"});
    CHECK(badCode == 1);
    CHECK(bad.find("has no field \"bogus\"") != std::string::npos);

    // tags
    cli.run({"tag", keys[0], keys[1], "--add", "fa26, exam"});
    cli.run({"tag", keys[1], "--remove", "exam"});
    CHECK((*lib.item(course, keys[0]))["tags"] == Json::array({Json{{"tag", "fa26"}}, Json{{"tag", "exam"}}}));
    CHECK((*lib.item(course, keys[1]))["tags"] == Json::array({Json{{"tag", "fa26"}}}));

    // a note under an item, in Markdown on standard input; then more of it
    auto [noteCode, noteOut] = cli.run({"add-note", keys[0], "-"}, "## Summary\n\nIt is **short**: $x^2$.\n");
    REQUIRE(noteCode == 0);
    auto noteKey = trim(noteOut);
    auto note = lib.item(course, noteKey);
    REQUIRE(note);
    CHECK(jstr(*note, "parentItem") == keys[0]);
    CHECK(jstr(*note, "note") == "<div data-schema-version=\"9\"><h2>Summary</h2><p>It is <strong>short</strong>: "
                                 "<span class=\"math\">$x^2$</span>.</p></div>");
    cli.run({"edit-note", noteKey, "--append", "--text", "And more."});
    CHECK(jstr(*lib.item(course, noteKey), "note").find("</p><p>And more.</p></div>") != std::string::npos);
    // a note of its own, in the collection
    auto [loneCode, loneOut] = cli.run({"add-note", "--collection", "FA26/CS8803", "--text", "Exam on Friday"});
    REQUIRE(loneCode == 0);
    auto lone = lib.item(course, trim(loneOut));
    REQUIRE(lone);
    CHECK((*lone)["collections"] == Json::array({cs8803}));
    CHECK(!lone->contains("parentItem"));

    // a collection, items into it and out, the trash and back
    auto [newCode, newOut] = cli.run({"new-collection", "Week 1", "--parent", "FA26/CS8803"});
    REQUIRE(newCode == 0);
    auto week = trim(newOut);
    CHECK(jstr(*lib.collection(course, week), "parentCollection") == cs8803);
    cli.run({"file", keys[0], keys[1], "--collection", "FA26/CS8803/Week 1"});
    CHECK(lib.collectionItemKeys(course, week).size() == 2);
    cli.run({"unfile", keys[1], "--collection", week});
    CHECK(lib.collectionItemKeys(course, week).size() == 1);
    cli.run({"trash", keys[1]});
    CHECK(jbool(*lib.item(course, keys[1]), "deleted"));
    cli.run({"restore", keys[1]});
    CHECK(!jbool(*lib.item(course, keys[1]), "deleted"));
}

TEST_CASE("cli: a note's text that begins with '-' is its text; empty text leaves the note as it was")
{
    Cli cli;
    auto id = cli.lib.defaultLibrary();
    auto paper = jstr(cli.lib.createItem(id, Json{{"itemType", "book"}, {"title", "A Book"}}), "key");
    auto [code, out] = cli.run({"add-note", paper, "Kept"});
    REQUIRE(code == 0);
    auto note = trim(out);
    auto text = [&] { return jstr(*cli.lib.item(id, note), "note"); };

    // a Markdown list, front matter's rule: text (an option has no spaces in it)
    CHECK(cli.run({"edit-note", note, "- first point\n- second"}).first == 0);
    CHECK(text() == "<div data-schema-version=\"9\"><ul><li>first point</li><li>second</li></ul></div>");
    // after --, all is text
    CHECK(cli.run({"edit-note", note, "--append", "--", "-v2"}).first == 0);
    CHECK(text() == "<div data-schema-version=\"9\"><ul><li>first point</li><li>second</li></ul><p>-v2</p></div>");

    // empty ("$TEXT" with TEXT unset), or nothing on standard input: refused, the note kept
    auto before = text();
    for (auto words : std::vector<std::vector<std::string>>{
             {"edit-note", note, ""}, {"edit-note", note, "-"}, {"edit-note", note}, {"edit-note", note, " \n"},
             {"edit-note", note, "--append", ""}, {"edit-note", note, "--html", "<p> </p>"}}) {
        auto [emptyCode, emptyOut] = cli.run(words, " \n");
        CAPTURE(emptyOut);
        CHECK(emptyCode == 1);
        CHECK(emptyOut.find("empty") != std::string::npos);
    }
    CHECK(text() == before);
    // a note made: the same
    CHECK(cli.run({"add-note", paper, "- a list"}).first == 0);
    CHECK(cli.run({"add-note", paper, ""}).first == 1);
    CHECK(cli.lib.children(id, paper, "note").size() == 2);
}

TEST_CASE("cli: what it changes and syncs is in the app's sync history, as ournotes-cli's")
{
    Cli cli;
    cli.lib.setOrigin("ournotes-cli");  // as main() does
    auto lib = cli.lib.defaultLibrary();
    auto key = jstr(cli.lib.createItem(lib, Json{{"itemType", "book"}, {"title", "Old"}}), "key");
    CHECK(cli.run({"add-folder", pathString(cli.dir.path / "target")}).first == 0);
    CHECK(cli.run({"set", key, "title=New"}).first == 0);
    auto [code, out] = cli.run({"sync"});
    INFO(out);
    REQUIRE(code == 0);
    sync::SyncHistory history(cli.lib.dataDir() / "sync-history.json");
    REQUIRE(history.runs().size() == 1);
    auto& run = history.runs()[0];
    CHECK(run["by"] == "ournotes-cli");
    CHECK(run["target"] == "target");
    CHECK(run["ok"] == true);
    REQUIRE(run["changes"].size() == 1);
    CHECK(run["changes"][0]["key"] == key);
    CHECK(run["changes"][0]["title"] == "New");
    CHECK(run["changes"][0]["by"] == "ournotes-cli");

    // a sync that fails is in it too
    SyncTarget broken;
    broken.id = "broken";
    broken.name = "Broken";
    broken.kind = "local";
    broken.path = pathString(cli.dir.path / "data" / "ournotes.sqlite" / "target");  // under a file
    cli.config.upsertTarget(broken);
    auto failed = cli.run({"sync", "--target", "broken"});
    INFO(failed.second);
    CHECK(failed.first != 0);
    history.reload();
    REQUIRE(history.runs().size() == 2);
    CHECK(history.runs()[0]["ok"] == false);
    CHECK(history.runs()[0]["by"] == "ournotes-cli");
}

TEST_CASE("cli: libraries and collections made, renamed, moved and deleted, as in the app")
{
    Cli cli;
    auto& lib = cli.lib;
    constexpr auto npos = std::string::npos;
    lib.defaultLibrary();
    auto [newCode, newOut] = cli.run({"new-library", "Course"});
    REQUIRE(newCode == 0);
    auto course = std::stoll(trim(newOut));
    CHECK(lib.library(course)->name == "Course");
    CHECK(cli.run({"new-library", "course"}).first == 1);  // there is one by that name
    CHECK(cli.run({"rename-library", "Course", "Courses"}).first == 0);
    CHECK(lib.library(course)->name == "Courses");

    auto fa26 = trim(cli.run({"new-collection", "FA26", "--library", "Courses"}).second);
    auto cs = trim(cli.run({"new-collection", "CS8803", "--parent", "Courses/FA26"}).second);
    CHECK(cli.run({"rename-collection", "Courses/FA26/CS8803", "CS 8803"}).first == 0);
    CHECK(jstr(*lib.collection(course, cs), "name") == "CS 8803");
    CHECK(cli.run({"move-collection", fa26, "--parent", cs}).first == 1);  // into its own subcollection
    CHECK(cli.run({"move-collection", cs, "--top"}).first == 0);
    CHECK(!lib.collection(course, cs)->value("parentCollection", Json()).is_string());
    CHECK(cli.run({"move-collection", fa26, "--parent", cs}).first == 0);
    CHECK(jstr(*lib.collection(course, fa26), "parentCollection") == cs);

    auto item = jstr(lib.createItem(course, Json{{"itemType", "book"}, {"title", "Kept"}, {"collections", Json::array({fa26})}}),
                     "key");
    auto [askCode, ask] = cli.run({"delete-collection", cs});
    CHECK(askCode == 1);
    CHECK(ask.find("add --yes") != npos);
    CHECK(lib.collection(course, cs));
    CHECK(cli.run({"delete-collection", cs, "--yes"}).first == 0);
    CHECK(!lib.collection(course, cs));
    CHECK(!lib.collection(course, fa26));  // its subcollection with it
    REQUIRE(lib.item(course, item));       // the items stay
    CHECK(!jbool(*lib.item(course, item), "deleted"));

    // only an empty library goes, and never the last one
    CHECK(cli.run({"delete-library", "Courses", "--yes"}).first == 1);
    lib.eraseItems(course, {item});
    CHECK(cli.run({"delete-library", "Courses"}).first == 1);  // asks for --yes
    CHECK(cli.run({"delete-library", "Courses", "--yes"}).first == 0);
    CHECK(!lib.library(course));
    CHECK(cli.run({"delete-library", "My Library", "--yes"}).first == 1);
}

TEST_CASE("cli: items made from nothing, of another type, their authors, copied, related and merged")
{
    Cli cli;
    auto& lib = cli.lib;
    constexpr auto npos = std::string::npos;
    auto mine = lib.defaultLibrary();
    auto [code, out] = cli.run({"new-item", "journalArticle", "title=Attention Is All You Need", "date=2017",
                                "publicationTitle=NeurIPS"});
    INFO(out);
    REQUIRE(code == 0);
    auto key = trim(out);
    CHECK(jstr(*lib.item(mine, key), "title") == "Attention Is All You Need");
    CHECK(jstr(*lib.item(mine, key), "publicationTitle") == "NeurIPS");
    CHECK(cli.run({"new-item", "note"}).first == 1);
    CHECK(cli.run({"new-item", "book", "publicationTitle=x"}).first == 1);  // a book has none

    // types, and another one
    auto types = cli.json({"types", "--json"});
    CHECK(std::any_of(types.begin(), types.end(), [](const Json& t) { return t["type"] == "book"; }));
    auto book = cli.json({"types", "book", "--json"});
    CHECK(std::any_of(book["creatorTypes"].begin(), book["creatorTypes"].end(),
                      [](const Json& c) { return c["creatorType"] == "author" && c["primary"] == true; }));
    auto [typeCode, typeOut] = cli.run({"set-type", key, "book"});
    CHECK(typeCode == 1);  // a book has no publicationTitle
    CHECK(typeOut.find("publicationTitle") != npos);
    CHECK(cli.run({"set-type", key, "book", "--force"}).first == 0);
    CHECK(jstr(*lib.item(mine, key), "itemType") == "book");
    CHECK(!lib.item(mine, key)->contains("publicationTitle"));

    // authors
    CHECK(cli.run({"creators", key, "Vaswani, Ashish", "editor:Shazeer, Noam", "Google Brain"}).first == 0);
    auto creators = (*lib.item(mine, key))["creators"];
    REQUIRE(creators.size() == 3);
    CHECK(creators[0] == Json{{"creatorType", "author"}, {"firstName", "Ashish"}, {"lastName", "Vaswani"}});
    CHECK(creators[1]["creatorType"] == "editor");
    CHECK(creators[2] == Json{{"creatorType", "author"}, {"name", "Google Brain"}});
    CHECK(cli.run({"creators", key, "Parmar, Niki", "--add"}).first == 0);
    CHECK((*lib.item(mine, key))["creators"].size() == 4);
    // added in the order given, --add before, after or among them
    CHECK(cli.run({"creators", key, "Uszkoreit, Jakob", "--add", "Jones, Llion"}).first == 0);
    CHECK(cli.run({"creators", key, "--add", "Gomez, Aidan", "Kaiser, Lukasz"}).first == 0);
    creators = (*lib.item(mine, key))["creators"];
    REQUIRE(creators.size() == 8);
    CHECK(creators[4]["lastName"] == "Uszkoreit");
    CHECK(creators[5]["lastName"] == "Jones");
    CHECK(creators[6]["lastName"] == "Gomez");
    CHECK(creators[7]["lastName"] == "Kaiser");
    CHECK(cli.run({"creators", key}).second.find("editor: Shazeer, Noam") != npos);
    CHECK(cli.run({"creators", key, "interviewer:Doe, Jo"}).first == 1);  // not a book's
    CHECK(cli.run({"creators", key, "--clear"}).first == 0);
    CHECK((*lib.item(mine, key))["creators"].empty());

    // a copy, related to it; duplicates merged into the oldest
    lib.updateItem(mine, key, [](Json& d) { d["dateAdded"] = "2020-01-01T00:00:00Z"; });
    auto copy = trim(cli.run({"duplicate", key}).second);
    CHECK(copy != key);
    CHECK(jstr(*lib.item(mine, copy), "title") == "Attention Is All You Need");
    CHECK(cli.json({"related", key, "--json"})[0]["key"] == copy);
    auto groups = cli.json({"duplicates", "--json"});
    REQUIRE(groups.size() == 1);
    CHECK(groups[0]["items"].size() == 2);
    lib.createNote(mine, "<p>on the copy</p>", copy);
    CHECK(trim(cli.run({"merge", copy, key}).second) == key);
    CHECK(jbool(*lib.item(mine, copy), "deleted"));  // the other one to the trash
    CHECK(lib.children(mine, key, "note").size() == 1);  // its note moved over

    auto other = jstr(lib.createItem(mine, Json{{"itemType", "book"}, {"title", "Other"}}), "key");
    CHECK(cli.run({"related", key, "--add", other}).first == 0);
    auto related = lib.relatedKeys(mine, key);
    CHECK(std::find(related.begin(), related.end(), other) != related.end());
    CHECK(cli.run({"related", key, "--remove", other}).first == 0);
    related = lib.relatedKeys(mine, key);
    CHECK(std::find(related.begin(), related.end(), other) == related.end());
}

TEST_CASE("cli: files and links attached, notes moved, items copied elsewhere, deleted for good with --yes")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto other = lib.createLibrary("Other");
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    auto book = jstr(lib.createItem(mine, Json{{"itemType", "book"}, {"title", "Book"}}), "key");
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto [code, out] = cli.run({"attach", paper, pathString(pdfPath)});
    INFO(out);
    REQUIRE(code == 0);
    auto pdf = trim(out);
    auto att = *lib.item(mine, pdf);
    CHECK(jstr(att, "parentItem") == paper);
    CHECK(jstr(att, "linkMode") == "imported_file");
    CHECK(lib.attachmentFileExists(mine, att));
    auto linked = trim(cli.run({"attach", paper, pathString(pdfPath), "--link", "--title", "Linked"}).second);
    CHECK(jstr(*lib.item(mine, linked), "linkMode") == "linked_file");
    CHECK(jstr(*lib.item(mine, linked), "title") == "Linked");
    CHECK(cli.run({"attach", paper, pathString(cli.dir.path / "none.pdf")}).first == 1);
    CHECK(cli.run({"attach", pdf, pathString(pdfPath)}).first == 1);  // not under an attachment
    auto url = trim(cli.run({"attach-url", paper, "https://arxiv.org/abs/1706.03762", "--title", "arXiv"}).second);
    CHECK(jstr(*lib.item(mine, url), "url") == "https://arxiv.org/abs/1706.03762");
    CHECK(jstr(*lib.item(mine, url), "parentItem") == paper);
    CHECK(cli.run({"attach-url", paper, "arxiv.org/abs/1"}).first == 1);  // no scheme
    auto lone = trim(cli.run({"attach-url", "https://example.org"}).second);
    CHECK(jstr(*lib.item(mine, lone), "parentItem").empty());

#ifndef _WIN32
    // opened by the system's program (here one that writes down what it was given)
    auto opener = cli.dir.path / "opener.sh";
    auto opened = cli.dir.path / "opened.txt";
    writeFileAtomic(opener, "#!/bin/sh\necho \"$1\" > '" + pathString(opened) + "'\n");
    fs::permissions(opener, fs::perms::owner_all);
    setenv("OURNOTES_OPENER", pathString(opener).c_str(), 1);
    CHECK(cli.run({"open", paper}).first == 0);
    CHECK(trim(readFile(opened)) == pathString(lib.attachmentFile(mine, *lib.bestAttachment(mine, paper))));
    CHECK(cli.run({"open", pdf, "--folder"}).first == 0);
    CHECK(trim(readFile(opened)) == pathString(lib.attachmentFile(mine, att).parent_path()));
    CHECK(cli.run({"open", url}).first == 0);
    CHECK(trim(readFile(opened)) == "https://arxiv.org/abs/1706.03762");
    unsetenv("OURNOTES_OPENER");
#endif

    // a note under another item, then of its own in a collection
    auto note = jstr(lib.createNote(mine, "<p>a note</p>", paper), "key");
    CHECK(cli.run({"move", note, "--to", book}).first == 0);
    CHECK(jstr(*lib.item(mine, note), "parentItem") == book);
    CHECK(cli.run({"move", book, "--to", paper}).first == 1);  // a regular item has no parent
    auto loose = jstr(lib.createCollection(mine, "Loose"), "key");
    CHECK(cli.run({"move", note, "--standalone", "--collection", "Loose"}).first == 0);
    CHECK(!lib.item(mine, note)->contains("parentItem"));
    CHECK((*lib.item(mine, note))["collections"] == Json::array({loose}));

    // copied into another library, with what is under it
    auto [copyCode, copyOut] = cli.run({"copy", paper, "--to-library", "Other"});
    REQUIRE(copyCode == 0);
    auto copied = trim(copyOut);
    CHECK(jstr(*lib.item(other, copied), "title") == "Paper");
    CHECK(lib.children(other, copied, "attachment").size() == 3);

    // deleted for good: only with --yes
    CHECK(cli.run({"delete", paper}).first == 1);
    CHECK(lib.item(mine, paper));
    CHECK(cli.run({"delete", paper, "--yes"}).first == 0);
    CHECK(!lib.item(mine, paper));
    CHECK(!lib.item(mine, pdf));  // and what was under it
    cli.run({"trash", book});
    CHECK(cli.run({"empty-trash"}).first == 1);
    CHECK(cli.run({"empty-trash", "--yes"}).first == 0);
    CHECK(!lib.item(mine, book));
}

TEST_CASE("cli: items copied to another library take their files, one not on this computer downloaded first")
{
    constexpr auto npos = std::string::npos;
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto group = lib.createLibrary("Group");
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto pdf = lib.importAttachment(mine, pdfPath, paper);
    auto usb = cli.dir.path / "usb";
    REQUIRE(cli.run({"add-folder", pathString(usb)}).first == 0);
    REQUIRE(cli.run({"sync"}).first == 0);
    auto here = lib.attachmentFile(mine, pdf);
    fs::remove_all(here.parent_path());  // synced, not downloaded (files on demand)

    // the target's drive not there: nothing copied (the copy would be without its file for good)
    fs::rename(usb, cli.dir.path / "usb-away");
    auto [failed, failedOut] = cli.run({"copy", paper, "--to-library", "Group"});
    INFO(failedOut);
    CHECK(failed == 1);
    CHECK(failedOut.find("nothing was copied") != npos);
    CHECK(lib.allItems(group).empty());
    fs::remove_all(usb);
    fs::rename(cli.dir.path / "usb-away", usb);
    // there: downloaded, then copied with it
    auto [code, out] = cli.run({"copy", paper, "--to-library", "Group"});
    INFO(out);
    REQUIRE(code == 0);
    auto copies = lib.children(group, trim(out));
    REQUIRE(copies.size() == 1);
    CHECK(lib.attachmentFileExists(group, copies[0]));
    CHECK(fs::exists(here));
}

TEST_CASE("cli: tags listed, renamed, coloured and deleted; items by tag, unfiled or in the trash")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto c = jstr(lib.createCollection(mine, "C"), "key");
    auto a = jstr(lib.createItem(mine, Json{{"itemType", "book"},
                                            {"title", "A"},
                                            {"tags", Json::array({Json{{"tag", "exam"}}, Json{{"tag", "week-3"}}})},
                                            {"collections", Json::array({c})}}),
                  "key");
    auto b = jstr(lib.createItem(mine, Json{{"itemType", "book"}, {"title", "B"}, {"tags", Json::array({Json{{"tag", "exam"}}})}}),
                  "key");
    auto tags = cli.json({"tags", "--json"});
    REQUIRE(tags.size() == 2);
    CHECK(tags[0]["tag"] == "exam");
    CHECK(tags[0]["items"] == 2);
    CHECK(cli.run({"rename-tag", "exam", "final"}).first == 0);
    CHECK(lib.itemKeysWithTags(mine, {"final"}).size() == 2);
    CHECK(cli.run({"rename-tag", "nope", "x"}).first == 1);
    CHECK(cli.run({"tag-color", "final", "red"}).first == 0);
    CHECK(lib.tagColors(mine)["final"] == "#ff6666");
    CHECK(cli.run({"tag-color", "final", "blurple"}).first == 1);
    CHECK(cli.run({"tag-color", "final", "none"}).first == 0);
    CHECK(!lib.tagColors(mine).count("final"));
    auto both = cli.json({"items", "--tag", "final,week-3", "--json"});
    REQUIRE(both.size() == 1);
    CHECK(both[0]["key"] == a);
    auto unfiled = cli.json({"items", "--unfiled", "--json"});
    REQUIRE(unfiled.size() == 1);
    CHECK(unfiled[0]["key"] == b);
    cli.run({"trash", b});
    auto trashed = cli.json({"items", "--trash", "--json"});
    REQUIRE(trashed.size() == 1);
    CHECK(trashed[0]["key"] == b);
    CHECK(cli.run({"delete-tag", "week-3"}).first == 1);
    CHECK(cli.run({"delete-tag", "week-3", "--yes"}).first == 0);
    CHECK(lib.itemKeysWithTags(mine, {"week-3"}).empty());
}

TEST_CASE("cli: annotations made on a PDF and changed; a note of them; the PDF with them; where it opens")
{
    Cli cli;
    auto& lib = cli.lib;
    constexpr auto npos = std::string::npos;
    auto mine = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"},
                                                {"title", "Paper"},
                                                {"date", "2020"},
                                                {"creators", Json::array({Json{{"creatorType", "author"},
                                                                               {"firstName", "Jane"},
                                                                               {"lastName", "Smith"}}})}}),
                      "key");
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto pdf = jstr(lib.importAttachment(mine, pdfPath, paper), "key");

    // a passage highlighted, found by its words
    auto [code, out] = cli.run({"annotate", paper, "--quote", "World from", "--comment", "a greeting", "--tags", "hello",
                                "--color", "green"});
    INFO(out);
    REQUIRE(code == 0);
    auto hlKey = trim(out);
    auto hl = *lib.item(mine, hlKey);
    CHECK(jstr(hl, "parentItem") == pdf);
    CHECK(jstr(hl, "annotationType") == "highlight");
    CHECK(jstr(hl, "annotationText") == "World from");
    CHECK(jstr(hl, "annotationColor") == "#5fb236");
    CHECK(jstr(hl, "annotationPageLabel") == "1");
    CHECK(hl["tags"] == Json::array({Json{{"tag", "hello"}}}));
    CHECK(hl["annotationPosition"]["pageIndex"] == 0);
    REQUIRE(hl["annotationPosition"]["rects"].size() == 1);
    auto rect = hl["annotationPosition"]["rects"][0];  // in the PDF's space: the line at y = 700
    CHECK(rect[1].get<double>() < 705);
    CHECK(rect[3].get<double>() > 700);
    CHECK(cli.run({"annotate", paper, "--quote", "not in it"}).first == 1);
    auto under = trim(cli.run({"annotate", pdf, "--quote", "second line", "--type", "underline"}).second);
    CHECK(jstr(*lib.item(mine, under), "annotationType") == "underline");

    // a note beside a passage, a text box, an area, a drawing
    auto note = trim(cli.run({"annotate", paper, "--type", "note", "--quote", "second line", "--comment", "why?"}).second);
    CHECK(jstr(*lib.item(mine, note), "annotationComment") == "why?");
    auto text = trim(cli.run({"annotate", paper, "--type", "text", "--at", "72,300", "--comment", "Summary"}).second);
    auto textPos = (*lib.item(mine, text))["annotationPosition"];
    CHECK(textPos["fontSize"] == 14);
    CHECK(textPos["rotation"] == 0);
    CHECK(cli.run({"annotate", paper, "--type", "text", "--at", "72,300"}).first == 1);  // a text box needs its text
    auto area = trim(cli.run({"annotate", paper, "--type", "image", "--rect", "72,100,300,200"}).second);
    CHECK((*lib.item(mine, area))["annotationPosition"]["rects"].size() == 1);
    CHECK(cli.run({"annotate", paper, "--type", "image", "--rect", "72,100,3000,200"}).first == 1);  // off the page
    auto ink = trim(cli.run({"annotate", paper, "--type", "ink", "--ink", "100,400 150,420 200,400;100,450 200,450"}).second);
    auto inkPos = (*lib.item(mine, ink))["annotationPosition"];
    CHECK(inkPos["paths"].size() == 2);
    CHECK(inkPos["paths"][0].size() == 6);
    CHECK(inkPos["width"] == 2);
    CHECK(lib.annotations(mine, pdf).size() == 6);

    // changed; one from the PDF file itself is not
    CHECK(cli.run({"annotation", hlKey, "--comment", "hello there", "--color", "#123456", "--tags", "a,b"}).first == 0);
    hl = *lib.item(mine, hlKey);
    CHECK(jstr(hl, "annotationComment") == "hello there");
    CHECK(jstr(hl, "annotationColor") == "#123456");
    CHECK(hl["tags"].size() == 2);
    CHECK(cli.json({"annotation", hlKey})["comment"] == "hello there");
    auto external = jstr(lib.createItem(mine, Json{{"itemType", "annotation"},
                                                   {"parentItem", pdf},
                                                   {"annotationType", "highlight"},
                                                   {"annotationIsExternal", true}}),
                         "key");
    CHECK(cli.run({"annotation", external, "--comment", "x"}).first == 1);
    CHECK(cli.run({"delete", external, "--yes"}).first == 1);
    // moved and resized, as dragged in the app
    CHECK(cli.run({"annotation", area, "--rect", "100,100,250,180"}).first == 0);
    CHECK((*lib.item(mine, area))["annotationPosition"]["rects"][0][0] == 100.0);
    CHECK(cli.run({"annotation", text, "--font-size", "20"}).first == 0);
    CHECK((*lib.item(mine, text))["annotationPosition"]["fontSize"] == 20.0);
    CHECK(cli.run({"annotation", note, "--at", "30,30"}).first == 0);
    CHECK(cli.run({"annotation", hlKey, "--rect", "1,1,50,50"}).first == 1);  // a highlight stays on its words
    CHECK(cli.run({"annotation", area, "--font-size", "12"}).first == 1);

    // a note quoting them, under the paper; and the same added to a note
    auto noteKey = trim(cli.run({"note-from-annotations", paper}).second);
    auto made = *lib.item(mine, noteKey);
    CHECK(jstr(made, "parentItem") == paper);
    INFO(jstr(made, "note"));
    // (each quote's reference a link to its annotation, as the app makes them)
    auto uid = lib.library(mine)->uid;
    auto linkTo = [&](const std::string& annotation) {
        return "<a href=\"ournotes://item/" + uid + "/" + pdf + "?page=1&amp;annotation=" + annotation + "\">";
    };
    CHECK(jstr(made, "note").find("“World from” " + linkTo(hlKey) + "(Smith, 2020, p. 1)</a> hello there") != npos);
    CHECK(jstr(made, "note").find("Summary <a href=") != npos);
    CHECK(cli.run({"note-from-annotations", pdf, "--to", noteKey}).first == 0);
    auto html = jstr(*lib.item(mine, noteKey), "note");
    CHECK(html.find("World from") != html.rfind("World from"));  // twice
    auto one = trim(cli.run({"note-from-annotations", hlKey}).second);  // as the reader's Add to Note
    CHECK(jstr(*lib.item(mine, one), "note") == "<div data-schema-version=\"9\"><p>\u201cWorld from\u201d " + linkTo(hlKey) +
                                                    "(Smith, 2020, p. 1)</a></p><p>hello there</p></div>");
    CHECK(jstr(*lib.item(mine, one), "parentItem") == paper);

    // links to them, for notes: the PDF at a page (the item's), an annotation, a note
    CHECK(trim(cli.run({"link", paper}).second) == "ournotes://item/" + uid + "/" + paper);
    CHECK(trim(cli.run({"link", paper, "--page", "1", "--markdown"}).second) ==
          "[(Smith, 2020, p. 1)](ournotes://item/" + uid + "/" + pdf + "?page=1)");
    CHECK(trim(cli.run({"link", hlKey, "--markdown", "--quote", "World from"}).second) ==
          "\u201cWorld from\u201d [(Smith, 2020, p. 1)](ournotes://item/" + uid + "/" + pdf + "?page=1&annotation=" + hlKey + ")");
    CHECK(trim(cli.run({"link", one, "--html"}).second) ==
          "<a href=\"ournotes://item/" + uid + "/" + one + "\">\u201cWorld from\u201d (Smith, 2020, p. 1)</a>");
    CHECK(cli.json({"link", pdf, "--page", "1", "--json"})["text"] == "(Smith, 2020, p. 1)");
    CHECK(cli.run({"link", paper, "--page", "9"}).first == 1);  // it has one page
    CHECK(cli.run({"link", one, "--page", "1"}).first == 1);    // a note has none

    // a copy of the PDF with them in it
    auto exported = cli.dir.path / "annotated.pdf";
    CHECK(cli.run({"export-pdf", paper, "-o", pathString(exported)}).first == 0);
    CHECK(pdf::Document::open(exported)->embeddedAnnotations().size() == 6);  // (the PDF's own one has no place)
    CHECK(cli.run({"export-pdf", paper, "-o", pathString(lib.attachmentFile(mine, *lib.item(mine, pdf)))}).first == 1);

    // where the app opens it
    CHECK(cli.run({"view-state", paper, "--page", "1", "--zoom", "150"}).first == 0);
    CHECK(lib.readerState(pdf)["scale"] == 150.0);
    CHECK(lib.readerState(pdf)["pageIndex"] == 0);
    CHECK(lib.setting(mine, "lastPageIndex_" + pdf) == 0);
    CHECK(cli.run({"view-state", paper, "--page", "9"}).first == 1);  // it has one page
    CHECK(cli.run({"view-state", paper, "--zoom", "page-fit"}).first == 0);
    auto state = cli.json({"view-state", paper, "--json"});
    CHECK(state["zoom"] == "page-fit");
    CHECK(state["page"] == 1);
}

TEST_CASE("cli: the app's preferences, what the library takes on disk, its syncs and conflicts")
{
    Cli cli;
    constexpr auto npos = std::string::npos;
    auto mine = cli.lib.defaultLibrary();
    auto prefs = cli.json({"config", "--json"});
    CHECK(std::any_of(prefs.begin(), prefs.end(), [](const Json& p) { return p["name"] == "note-zoom" && p["value"] == 100; }));
    CHECK(cli.run({"config", "note-zoom", "125"}).first == 0);
    CHECK(Config(cli.dir.path / "config.json").get("/notes/zoom", 0) == 125);  // saved
    CHECK(cli.run({"config", "note-zoom", "big"}).first == 1);
    CHECK(cli.run({"config", "appearance", "purple"}).first == 1);
    CHECK(cli.run({"config", "appearance", "dark"}).first == 0);
    CHECK(cli.run({"config", "palette", "red,green,#123456"}).first == 0);
    CHECK(cli.config.get("/reader/palette", Json()) == Json::array({"#ff6666", "#5fb236", "#123456"}));
    CHECK(cli.run({"config", "citation-style", "nope"}).first == 1);
    CHECK(trim(cli.run({"config", "sync-auto"}).second) == "true");
    CHECK(cli.run({"config", "bogus"}).first == 1);

    CHECK(cli.run({"add-folder", pathString(cli.dir.path / "usb"), "--name", "USB stick"}).first == 0);
    CHECK(cli.config.targets().back().name == "USB stick");

    auto usage = cli.json({"storage", "--json"});
    REQUIRE(usage["libraries"].size() == 1);
    CHECK(usage["libraries"][0]["name"] == "My Library");

    CHECK(cli.json({"sync-history", "--json"}).empty());
    cli.lib.recordConflict(mine, "item", "ABCD2345", Json{{"title", "here"}}, Json{{"title", "there"}}, "remote");
    auto conflicts = cli.json({"conflicts", "--json"});
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0]["kept"] == "remote");
    CHECK(cli.run({"conflicts"}).second.find("there") != npos);
    CHECK(cli.run({"conflicts", "--clear"}).first == 0);
    CHECK(cli.json({"conflicts", "--json"}).empty());
}

TEST_CASE("cli: a library the user may only read is left as it is")
{
    Cli cli;
    auto& lib = cli.lib;
    constexpr auto npos = std::string::npos;
    auto group = lib.createLibrary("Group", "group");
    auto key = jstr(lib.createItem(group, Json{{"itemType", "book"}, {"title", "Read only"}}), "key");
    auto coll = jstr(lib.createCollection(group, "C"), "key");
    lib.setLibraryEditable(group, false);
    for (auto words : std::vector<std::vector<std::string>>{{"set", key, "title=Changed"},
                                                            {"tag", key, "--add", "x"},
                                                            {"add-note", key, "a note"},
                                                            {"creators", key, "Doe, Jo"},
                                                            {"new-item", "book", "--library", "Group"},
                                                            {"new-collection", "D", "--library", "Group"},
                                                            {"rename-collection", coll, "E"},
                                                            {"file", key, "--collection", coll},
                                                            {"trash", key},
                                                            {"delete", key, "--yes"},
                                                            {"duplicate", key},
                                                            {"empty-trash", "--library", "Group", "--yes"}}) {
        auto [code, out] = cli.run(words);
        INFO(words[0] << ": " << out);
        CHECK(code == 1);
        CHECK(out.find("can only be read") != npos);
    }
    CHECK(jstr(*lib.item(group, key), "title") == "Read only");
}

TEST_CASE("cli: what a review found: scope, partial changes, odd input, as the app does")
{
    Cli cli;
    auto& lib = cli.lib;
    constexpr auto npos = std::string::npos;
    auto mine = lib.defaultLibrary();

    // a new library is a group one, as the app makes it: the next sync keeps it
    auto [newCode, newOut] = cli.run({"new-library", "Group"});
    REQUIRE(newCode == 0);
    auto group = std::stoll(trim(newOut));
    CHECK(lib.library(group)->type == "group");

    // what is left over is no silent no-op
    auto a = jstr(lib.createItem(mine, Json{{"itemType", "book"}, {"title", "A"}}), "key");
    auto b = jstr(lib.createItem(mine, Json{{"itemType", "book"}, {"title", "B"}}), "key");
    auto c = jstr(lib.createItem(mine, Json{{"itemType", "book"}, {"title", "C"}}), "key");
    CHECK(cli.run({"related", a, "--add", b, c}).first == 1);
    CHECK(lib.relatedKeys(mine, a).empty());
    CHECK(cli.run({"related", a, "--add", b + "," + c}).first == 0);
    CHECK(lib.relatedKeys(mine, a).size() == 2);
    // --add before the authors' names
    CHECK(cli.run({"creators", a, "--add", "Doe, Jane", "Roe, Rick"}).first == 0);
    CHECK((*lib.item(mine, a))["creators"].size() == 2);
    CHECK(cli.run({"merge", a, a}).first == 1);

    // a tag in two libraries is changed in the one said
    auto g = jstr(lib.createItem(group, Json{{"itemType", "book"}, {"title", "G"}, {"tags", Json::array({Json{{"tag", "exam"}}})}}),
                  "key");
    cli.run({"tag", a, "--add", "exam"});
    auto [tagCode, tagOut] = cli.run({"delete-tag", "exam", "--yes"});
    CHECK(tagCode == 1);
    CHECK(tagOut.find("say which with --library") != npos);
    CHECK(cli.run({"delete-tag", "exam", "--library", "Group", "--yes"}).first == 0);
    CHECK(lib.itemKeysWithTags(group, {"exam"}).empty());
    CHECK(lib.itemKeysWithTags(mine, {"exam"}).size() == 1);

    // the trash of the library named; annotations are deleted, not trashed
    cli.run({"trash", g});
    cli.run({"trash", c});
    CHECK(cli.run({"empty-trash", "Group", "--yes"}).first == 0);
    CHECK(!lib.item(group, g));
    CHECK(lib.item(mine, c));  // the other library's trash as it was
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto pdf = jstr(lib.importAttachment(mine, pdfPath, b), "key");
    auto anno = trim(cli.run({"annotate", pdf, "--quote", "Hello"}).second);
    CHECK(cli.run({"trash", anno}).first == 1);
    CHECK(!jbool(*lib.item(mine, anno), "deleted"));

    // a passage over two lines, whole; on a page turned a quarter, the words found
    auto [twoCode, two] = cli.run({"annotate", pdf, "--quote", "OurNotes second line"});
    REQUIRE(twoCode == 0);
    auto both = *lib.item(mine, trim(two));
    CHECK(jstr(both, "annotationText") == "OurNotes second line");
    CHECK(both["annotationPosition"]["rects"].size() == 2);
    auto turnedPath = cli.dir.path / "turned.pdf";
    writeSamplePdf(turnedPath, 90);
    auto turned = jstr(lib.importAttachment(mine, turnedPath, b), "key");
    auto [turnedCode, turnedOut] = cli.run({"annotate", turned, "--quote", "World from"});
    REQUIRE(turnedCode == 0);
    CHECK(jstr(*lib.item(mine, trim(turnedOut)), "annotationText") == "World from");

    // no place that is not a number, or off the page
    CHECK(cli.run({"annotate", pdf, "--type", "image", "--rect", "nan,100,300,200"}).first == 1);
    CHECK(cli.run({"annotate", pdf, "--type", "note", "--at", "nan,nan"}).first == 1);
    CHECK(cli.run({"annotate", pdf, "--type", "ink", "--ink", "nan,nan 10,10"}).first == 1);
    CHECK(cli.run({"view-state", pdf, "--zoom", "nan"}).first == 1);
    CHECK(cli.run({"annotate", pdf, "--type", "text", "--at", "700,100", "--comment", "x"}).first == 1);
    CHECK(cli.run({"annotate", pdf, "--type", "text", "--at", "600,100", "--comment", "x"}).first == 1);  // no room
    // moved: sorted as the app sorts one it moved
    auto area = trim(cli.run({"annotate", pdf, "--type", "image", "--rect", "72,100,300,200"}).second);
    CHECK(cli.run({"annotation", area, "--rect", "72,300,300,400"}).first == 0);
    CHECK(jstr(*lib.item(mine, area), "annotationSortIndex") == pdf::sortIndex(0, 0, 300));

    // copied: not an annotation alone, not from the trash, not within its own library
    auto other = lib.createLibrary("Other");
    CHECK(cli.run({"copy", anno, "--to-library", "Other"}).first == 1);
    CHECK(cli.run({"copy", c, "--to-library", "Other"}).first == 1);  // in the trash
    CHECK(cli.run({"copy", b, "--to-library", "My Library"}).first == 1);
    CHECK(lib.libraryContents(other).empty());

    // the trash listed by what matches in it, a note on its own too
    auto note = jstr(lib.createNote(mine, "<p>zebra stripes</p>", b), "key");
    cli.run({"trash", note});
    auto found = cli.json({"items", "--trash", "--search", "zebra", "--json"});
    REQUIRE(found.size() == 1);
    CHECK(found[0]["key"] == note);

    // several items: none changed when one of them may not be
    auto locked = lib.createLibrary("Locked", "group");
    auto l = jstr(lib.createItem(locked, Json{{"itemType", "book"}, {"title", "L"}}), "key");
    lib.setLibraryEditable(locked, false);
    CHECK(cli.run({"trash", b, l}).first == 1);
    CHECK(!jbool(*lib.item(mine, b), "deleted"));
    CHECK(cli.run({"tag", b, l, "--add", "x"}).first == 1);
    CHECK(lib.itemKeysWithTags(mine, {"x"}).empty());

    // a linked file that is not where it was is not "not downloaded"
    auto gone = cli.dir.path / "gone.pdf";
    writeSamplePdf(gone);
    auto linked = trim(cli.run({"attach", b, pathString(gone), "--link"}).second);
    fs::remove(gone);
    auto [linkCode, linkOut] = cli.run({"export-pdf", linked, "-o", pathString(cli.dir.path / "x.pdf")});
    CHECK(linkCode == 1);
    CHECK(linkOut.find("is not at") != npos);

    // exported without -o: not over a file that is there
    auto cwd = fs::current_path();
    fs::current_path(cli.dir.path);
    CHECK(cli.run({"export-pdf", pdf}).first == 0);
    CHECK(cli.run({"export-pdf", pdf}).first == 1);
    CHECK(cli.run({"export-pdf", pdf, "--force"}).first == 0);
    fs::current_path(cwd);
}

namespace {
// A PNG's width, height and colour type (2: RGB, 6: RGB with alpha).
std::tuple<int, int, int> pngInfo(const fs::path& file)
{
    auto d = readFile(file);
    REQUIRE(d.size() > 26);
    REQUIRE(d.substr(1, 3) == "PNG");
    auto be = [&](size_t at) {
        return (int)((unsigned char)d[at] << 24 | (unsigned char)d[at + 1] << 16 | (unsigned char)d[at + 2] << 8 |
                     (unsigned char)d[at + 3]);
    };
    return {be(16), be(20), (unsigned char)d[25]};
}
} // namespace

TEST_CASE("cli: a passage across a page break, highlighted as one annotation on both pages")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    auto pdfPath = cli.dir.path / "linked.pdf";
    writeLinkedPdf(pdfPath);  // page 1: "See [1] for the details.", page 2: "Back to page one, from here."
    auto pdf = jstr(lib.importAttachment(mine, pdfPath, paper), "key");

    // its first words at the end of page 1, the rest at the top of page 2 (in any case)
    auto [code, out] = cli.run({"annotate", paper, "--quote", "for the details. back to page", "--type", "underline"});
    INFO(out);
    REQUIRE(code == 0);
    auto made = *lib.item(mine, trim(out));
    auto position = made["annotationPosition"];
    CHECK(position["pageIndex"] == 0);
    CHECK(position["rects"].size() == 1);
    REQUIRE(position["nextPageRects"].size() == 1);
    CHECK(jstr(made, "annotationText") == "for the details. Back to page");
    // (on page 2, high on it: where "Back" is)
    CHECK(position["nextPageRects"][0][3].get<double>() > 690);
    // starting where --page says; one that is nowhere, nor across any break, is refused
    CHECK(cli.run({"annotate", paper, "--quote", "from here. left column words", "--page", "2"}).first == 0);
    CHECK(cli.run({"annotate", paper, "--quote", "from here. left column words", "--page", "1"}).first == 1);
    CHECK(cli.run({"annotate", paper, "--quote", "the details. nowhere at all"}).first == 1);
    CHECK(cli.run({"annotate", paper, "--quote", "See [1] for a"}).first == 1);  // (not the "a" in "Back")
    CHECK(lib.annotations(mine, pdf).size() == 2);

    // a figure's long caption at the top of the next page, a page number at the foot: left out
    auto brokenPath = cli.dir.path / "broken.pdf";
    writeBrokenSentencePdf(brokenPath);
    auto other = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "Other"}}), "key");
    auto broken = jstr(lib.importAttachment(mine, brokenPath, other), "key");
    auto [code2, out2] = cli.run({"annotate", other, "--quote", "However, the resulting representations are typically"});
    INFO(out2);
    REQUIRE(code2 == 0);
    auto across = *lib.item(mine, trim(out2));
    CHECK(jstr(across, "annotationText") == "However, the resulting representations are typically");
    CHECK(across["annotationPosition"]["nextPageRects"].size() == 1);
    CHECK(lib.annotations(mine, broken).size() == 1);
}

TEST_CASE("cli: a page with its annotations as a picture, or a doodle's place on it, or the doodle alone")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto pdf = jstr(lib.importAttachment(mine, pdfPath, paper), "key");
    cli.run({"annotate", paper, "--quote", "Hello World"});
    auto ink = trim(cli.run({"annotate", paper, "--type", "ink", "--color", "red", "--ink", "100,200 150,250 200,200"}).second);

    // the page, at 72 dots per inch: as large as it is in points (612 by 792)
    auto page = cli.dir.path / "page.png";
    auto [code, out] = cli.run({"render", paper, "--dpi", "72", "-o", pathString(page)});
    INFO(out);
    REQUIRE(code == 0);
    CHECK(trim(out) == pathString(page));
    auto [w, h, type] = pngInfo(page);
    CHECK(w == 612);
    CHECK(h == 792);
    CHECK(type == 2);
    // the doodle's place: it and 18 points around it, on the page
    auto place = cli.dir.path / "place.png";
    CHECK(cli.run({"render", ink, "--dpi", "72", "-o", pathString(place)}).first == 0);
    auto [pw, ph, ptype] = pngInfo(place);
    CHECK(std::abs(pw - (100 + 2 * 18 + 4)) <= 2);  // the stroke 100 wide, its width, the margin
    CHECK(std::abs(ph - (50 + 2 * 18 + 4)) <= 2);
    CHECK(ptype == 2);
    // the doodle alone, on clear
    auto alone = cli.dir.path / "alone.png";
    CHECK(cli.run({"render", ink, "--no-page", "--margin", "0", "-o", pathString(alone)}).first == 0);
    CHECK(std::get<2>(pngInfo(alone)) == 6);
    // a part of the page; without -o, named after the PDF and its page, not over another
    auto cwd = fs::current_path();
    fs::current_path(cli.dir.path);
    CHECK(cli.run({"render", pdf, "--rect", "72,72,300,150"}).first == 0);
    CHECK(fs::exists(cli.dir.path / "one p1.png"));
    CHECK(cli.run({"render", pdf, "--rect", "72,72,300,150"}).first == 1);
    CHECK(cli.run({"render", pdf, "--rect", "72,72,300,150", "--force"}).first == 0);
    fs::current_path(cwd);
    CHECK(cli.run({"render", pdf, "--page", "2"}).first == 1);                // it has one page
    CHECK(cli.run({"render", pdf, "--rect", "72,72,3000,150"}).first == 1);   // off the page
    CHECK(cli.run({"render", ink, "--page", "1"}).first == 1);                // an annotation is where it is
    CHECK(cli.run({"render", pdf, "--dpi", "6000"}).first == 1);
}

TEST_CASE("cli: pictures in notes, from files or data: URIs, in Markdown too, kept as the app keeps them")
{
    Cli cli;
    auto& lib = cli.lib;
    constexpr auto npos = std::string::npos;
    auto mine = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto pdf = jstr(lib.importAttachment(mine, pdfPath, paper), "key");
    auto figure = cli.dir.path / "figure.png";
    REQUIRE(cli.run({"render", pdf, "--dpi", "36", "-o", pathString(figure)}).first == 0);

    // ![what it shows](file) in Markdown
    auto [code, out] = cli.run({"add-note", paper, "-"}, "# Figures\n\n![the first page](" + pathString(figure) + ")\n");
    INFO(out);
    REQUIRE(code == 0);
    auto note = trim(out);
    auto html = jstr(*lib.item(mine, note), "note");
    INFO(html);
    auto pictures = lib.children(mine, note, "attachment");
    REQUIRE(pictures.size() == 1);
    auto picture = pictures[0];
    CHECK(jstr(picture, "linkMode") == "embedded_image");
    CHECK(jstr(picture, "contentType") == "image/png");
    CHECK(lib.attachmentFileExists(mine, picture));
    CHECK(html.find("<img data-attachment-key=\"" + jstr(picture, "key") + "\" alt=\"the first page\">") != npos);

    // more at the end: a file, and a data: URI
    auto base64 = [](const std::string& bytes) {
        static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3) {
            unsigned v = (unsigned char)bytes[i] << 16 | (unsigned char)bytes[i + 1] << 8 | (unsigned char)bytes[i + 2];
            for (int k = 3; k >= 0; --k)
                b64 += digits[(v >> (6 * k)) & 63];
        }
        if (i < bytes.size()) {
            unsigned v = (unsigned char)bytes[i] << 16 | (i + 1 < bytes.size() ? (unsigned char)bytes[i + 1] << 8 : 0);
            b64 += digits[(v >> 18) & 63];
            b64 += digits[(v >> 12) & 63];
            b64 += i + 1 < bytes.size() ? digits[(v >> 6) & 63] : '=';
            b64 += '=';
        }
        return b64;
    };
    auto bytes = readFile(figure);
    auto b64 = base64(bytes);
    CHECK(cli.run({"add-image", note, pathString(figure), "data:image/png;base64," + b64, "--width", "200"}).first == 0);
    pictures = lib.children(mine, note, "attachment");
    REQUIRE(pictures.size() == 3);
    for (auto& p : pictures)
        CHECK(readFile(lib.attachmentFile(mine, p)) == bytes);
    html = jstr(*lib.item(mine, note), "note");
    CHECK(html.find("width=\"200\"></p></div>") != npos);
    // and in a note edited
    CHECK(cli.run({"edit-note", note, "--append", "![again](" + pathString(figure) + ")"}).first == 0);
    CHECK(lib.children(mine, note, "attachment").size() == 4);

    // read: where each picture's file is, to be looked at
    auto [textCode, text] = cli.run({"text", note});
    CHECK(textCode == 0);
    CHECK(text.find("[picture: " + pathString(lib.attachmentFile(mine, picture)) + "]") != npos);

    // what is no picture, or not there, leaves the note as it was
    auto before = jstr(*lib.item(mine, note), "note");
    CHECK(cli.run({"add-image", note, pathString(cli.dir.path / "gone.png")}).first == 1);
    CHECK(cli.run({"add-image", note, pathString(pdfPath)}).first == 1);
    CHECK(cli.run({"add-image", note, "data:image/png;base64,bm90IGEgcGljdHVyZQ=="}).first == 1);
    CHECK(cli.run({"add-image", paper, pathString(figure)}).first == 1);  // not a note
    CHECK(jstr(*lib.item(mine, note), "note") == before);
    CHECK(lib.children(mine, note, "attachment").size() == 4);
    auto [badCode, badOut] = cli.run({"add-note", paper, "![x](" + pathString(cli.dir.path / "gone.png") + ")"});
    CHECK(badCode == 1);
    CHECK(lib.children(mine, paper, "note").size() == 1);  // no note made

    // side by side, as high as each other, with a caption under them (as in the app)
    CHECK(cli.run({"add-image", note, pathString(figure), pathString(figure), "--row", "--height", "120", "--caption",
                   "Two <figures>"})
              .first == 0);
    std::string keys[2];
    int n = 0;
    for (auto& p : lib.children(mine, note, "attachment"))
        if (before.find(jstr(p, "key")) == npos && n < 2)
            keys[n++] = jstr(p, "key");
    REQUIRE(n == 2);
    html = jstr(*lib.item(mine, note), "note");
    auto row = [&](const std::string& a, const std::string& b) {
        auto img = [](const std::string& key) { return "<img data-attachment-key=\"" + key + "\" height=\"120\">"; };
        return "<p>" + img(a) + img(b) + "</p><p class=\"caption\">Two &lt;figures&gt;</p></div>";
    };
    CHECK_MESSAGE((html.find(row(keys[0], keys[1])) != npos || html.find(row(keys[1], keys[0])) != npos), html);

    // a picture of some 40 KB as data: (std::regex read the tags, and overflowed the stack
    // from about 20 KB)
    auto large = cli.dir.path / "large.png";
    REQUIRE(cli.run({"render", pdf, "--dpi", "600", "-o", pathString(large)}).first == 0);
    auto largeData = "data:image/png;base64," + base64(readFile(large));
    REQUIRE(largeData.size() > 30000);
    CHECK(cli.run({"add-image", note, largeData}).first == 0);
    CHECK(cli.run({"add-note", paper, "![large](" + largeData + ")"}).first == 0);

    // a note's HTML with another note's pictures (`show --json`, a note merged into another):
    // copied, as the app copies one pasted, so they go with this note (deleted, copied)
    auto group = lib.createLibrary("Group");
    auto other = jstr(lib.createItem(group, Json{{"itemType", "book"}, {"title", "Other"}}), "key");
    auto source = jstr(*lib.item(mine, note), "note");
    auto ownKeys = [&](int64_t id, const std::string& key) {
        std::set<std::string> out;
        for (auto& d : lib.children(id, key, "attachment"))
            out.insert(jstr(d, "key"));
        return out;
    };
    auto [copyCode, copyOut] = cli.run({"add-note", other, "--html", source});
    INFO(copyOut);
    REQUIRE(copyCode == 0);
    auto copy = trim(copyOut);
    auto copied = ownKeys(group, copy);
    CHECK(copied.size() == ownKeys(mine, note).size());
    for (auto& key : copied) {
        CHECK(ownKeys(mine, note).count(key) == 0);
        CHECK(jstr(*lib.item(group, copy), "note").find(key) != npos);
    }
    for (auto& key : ownKeys(mine, note))
        CHECK(jstr(*lib.item(group, copy), "note").find(key) == npos);
    // its own kept as they are when the note is given again
    CHECK(cli.run({"edit-note", copy, "--html", jstr(*lib.item(group, copy), "note")}).first == 0);
    CHECK(ownKeys(group, copy) == copied);
    // the first note gone for good: the copy's pictures still there
    CHECK(cli.run({"delete", note, "--yes"}).first == 0);
    for (auto& d : lib.children(group, copy, "attachment"))
        CHECK(lib.attachmentFileExists(group, d));
    // one whose file is not here: refused, nothing made
    auto notes = lib.children(group, other, "note").size();
    auto [goneCode, goneOut] = cli.run({"add-note", other, "--html", "<p>x</p><img data-attachment-key=\"ZZZZ9999\">"});
    CHECK(goneCode == 1);
    CHECK(goneOut.find("not on this computer") != npos);
    CHECK(lib.children(group, other, "note").size() == notes);
    // and a note's picture moves with its note only
    auto picture2 = *lib.children(group, copy, "attachment").begin();
    auto [moveCode, moveOut] = cli.run({"move", jstr(picture2, "key"), "--standalone"});
    CHECK(moveCode == 1);
    CHECK(moveOut.find("a picture in a note") != npos);
    CHECK(jstr(*lib.item(group, jstr(picture2, "key")), "parentItem") == copy);
}

namespace {

// A conversation with `ournotes-cli mcp`: the messages sent, one per line; the replies.
std::vector<Json> mcp(Cli& cli, const std::vector<Json>& messages, bool write = false)
{
    std::string sent;
    for (auto& m : messages)
        sent += m.dump() + "\n";
    std::istringstream in(sent);
    std::ostringstream out;
    runMcpServer(cli.commands, {cli.lib.dataDir(), write}, in, out);
    std::vector<Json> replies;
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);)
        replies.push_back(Json::parse(line));
    return replies;
}

Json call(int id, const std::string& tool, const Json& arguments)
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                {"params", Json{{"name", tool}, {"arguments", arguments}}}};
}

// A tool's answer: {its text, whether it is an error}.
std::pair<std::string, bool> answer(const Json& reply)
{
    INFO(reply.dump());
    REQUIRE(reply.contains("result"));
    auto& r = reply["result"];
    return {r["content"][0]["text"].get<std::string>(), r.value("isError", false)};
}

} // namespace

TEST_CASE("cli: mcp: the protocol, and only the tools that read unless --write")
{
    Cli cli;
    auto r = mcp(cli, {Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                            {"params", Json{{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()}}}},
                       Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},  // no reply
                       Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}},
                       Json{{"jsonrpc", "2.0"}, {"id", "three"}, {"method", "tools/list"}},
                       Json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "no/such"}},
                       call(5, "no_such_tool", Json::object()),
                       call(6, "add_note", Json{{"text", "x"}}),
                       Json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "initialize"},
                            {"params", Json{{"protocolVersion", "1999-01-01"}}}}});
    REQUIRE(r.size() == 7);
    CHECK(r[0]["id"] == 1);
    CHECK(r[0]["result"]["protocolVersion"] == "2025-06-18");
    CHECK(r[0]["result"]["serverInfo"]["name"] == "ournotes");
    CHECK(r[0]["result"]["capabilities"].contains("tools"));
    CHECK(!jstr(r[0]["result"], "instructions").empty());
    CHECK(r[1]["id"] == 2);
    CHECK(r[1]["result"] == Json::object());
    CHECK(r[2]["id"] == "three");
    std::set<std::string> names;
    for (auto& t : r[2]["result"]["tools"]) {
        names.insert(jstr(t, "name"));
        CHECK(t["annotations"]["readOnlyHint"] == true);
        CHECK(t["inputSchema"]["type"] == "object");
        CHECK(!jstr(t, "description").empty());
    }
    CHECK(names.count("search_items"));
    CHECK(names.count("read_text"));
    CHECK(!names.count("add_note"));
    CHECK(!names.count("trash_items"));
    CHECK(r[3]["error"]["code"] == -32601);
    CHECK(r[4]["error"]["code"] == -32602);
    CHECK(r[5]["error"]["code"] == -32602);
    CHECK(jstr(r[5]["error"], "message").find("--write") != std::string::npos);
    CHECK(r[6]["result"]["protocolVersion"] == "2025-11-25");  // (the newest it speaks)

    // with --write, the tools that change the library too, and said to
    auto w = mcp(cli, {Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}}, true);
    REQUIRE(w.size() == 1);
    std::map<std::string, Json> tools;
    for (auto& t : w[0]["result"]["tools"])
        tools[jstr(t, "name")] = t;
    REQUIRE(tools.count("add_note"));
    CHECK(tools["add_note"]["annotations"]["readOnlyHint"] == false);
    CHECK(tools["add_note"]["annotations"]["destructiveHint"] == false);
    CHECK(tools["trash_items"]["annotations"]["destructiveHint"] == true);
    CHECK(tools["add_items"]["annotations"]["openWorldHint"] == true);
    CHECK(tools["search_items"]["annotations"]["readOnlyHint"] == true);

    // what is not JSON, and a blank line
    std::istringstream in("not json\n\n{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\r\n");
    std::ostringstream out;
    runMcpServer(cli.commands, {cli.lib.dataDir(), false}, in, out);
    std::istringstream lines(out.str());
    std::string first, second;
    std::getline(lines, first);
    std::getline(lines, second);
    CHECK(Json::parse(first)["error"]["code"] == -32700);
    CHECK(Json::parse(first)["id"].is_null());
    CHECK(Json::parse(second)["id"] == 9);
}

TEST_CASE("cli: mcp: items searched and read, a PDF in parts, its annotations, citations")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto fa26 = jstr(lib.createCollection(mine, "FA26"), "key");
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"},
                                                {"title", "Attention Is All You Need"},
                                                {"date", "2017"},
                                                {"creators", Json::array({Json{{"creatorType", "author"},
                                                                               {"lastName", "Vaswani"},
                                                                               {"firstName", "Ashish"}}})},
                                                {"tags", Json::array({Json{{"tag", "transformers"}}})},
                                                {"collections", Json::array({fa26})}}),
                      "key");
    lib.createItem(mine, Json{{"itemType", "book"}, {"title", "Deep Learning"}});
    auto pdfPath = cli.dir.path / "long.pdf";
    writeManyPagesPdf(pdfPath, 30);
    auto pdf = jstr(lib.importAttachment(mine, pdfPath, paper), "key");
    lib.createItem(mine, Json{{"itemType", "annotation"},
                              {"parentItem", pdf},
                              {"annotationType", "highlight"},
                              {"annotationText", "Page 3"},
                              {"annotationComment", "the third"},
                              {"annotationPageLabel", "3"}});

    auto r = mcp(cli, {call(1, "search_items", Json{{"query", "attention"}}),
                       call(2, "search_items", Json{{"limit", 1}}),
                       call(3, "search_items", Json{{"collection", "FA26"}, {"tags", Json::array({"transformers"})}}),
                       call(4, "get_item", Json{{"key", paper}}),
                       call(5, "read_text", Json{{"key", paper}}),
                       call(6, "read_text", Json{{"key", paper}, {"from_page", 29}}),
                       call(7, "get_annotations", Json{{"key", paper}}),
                       call(8, "format_citations", Json{{"keys", Json::array({paper})}}),
                       call(9, "get_link", Json{{"key", paper}, {"page", 3}}),
                       call(10, "get_item", Json::object()),
                       call(11, "get_item", Json{{"key", "NOSUCHKY"}}),
                       call(12, "search_items", Json{{"query", "nothing like it"}}),
                       call(13, "export_items", Json{{"keys", paper}}),
                       call(14, "format_citations", Json::object())});
    REQUIRE(r.size() == 14);
    for (size_t i = 0; i < r.size(); ++i)
        CHECK(r[i]["id"] == (int)i + 1);

    auto [found, e1] = answer(r[0]);
    CHECK(!e1);
    auto items = Json::parse(found);
    REQUIRE(items.size() == 1);
    CHECK(items[0]["key"] == paper);

    auto [limited, e2] = answer(r[1]);
    CHECK(!e2);
    CHECK(limited.find("(1 of 2 items") != std::string::npos);
    CHECK(Json::parse(limited.substr(0, limited.find('\n'))).size() == 1);

    auto [filed, e3] = answer(r[2]);
    CHECK(Json::parse(filed).size() == 1);

    auto [shown, e4] = answer(r[3]);
    CHECK(Json::parse(shown)["item"]["title"] == "Attention Is All You Need");

    auto [first, e5] = answer(r[4]);  // 20 pages, then where they are in the whole
    CHECK(first.find("[page 20]") != std::string::npos);
    CHECK(first.find("[page 21]") == std::string::npos);
    CHECK(first.find("[pages 1-20 of 30]") != std::string::npos);
    auto [last, e6] = answer(r[5]);
    CHECK(last.find("[page 28]") == std::string::npos);
    CHECK(last.find("Page 29") != std::string::npos);
    CHECK(last.find("[pages 29-30 of 30]") != std::string::npos);

    auto [marked, e7] = answer(r[6]);
    CHECK(marked.find("the third") != std::string::npos);
    auto [cited, e8] = answer(r[7]);
    CHECK(cited.find("Vaswani") != std::string::npos);
    auto [link, e9] = answer(r[8]);
    CHECK(link.find("ournotes://") != std::string::npos);
    CHECK(link.find("p. 3") != std::string::npos);

    auto [missing, e10] = answer(r[9]);
    CHECK(e10);
    CHECK(missing.find("missing argument: key") != std::string::npos);
    auto [unknown, e11] = answer(r[10]);
    CHECK(e11);
    CHECK(!unknown.empty());
    auto [none, e12] = answer(r[11]);
    CHECK(!e12);
    CHECK(none.find("(no items)") != std::string::npos);
    auto [bibtex, e13] = answer(r[12]);  // (one key given alone, not in a list)
    CHECK(!e13);
    CHECK(bibtex.find("@article") != std::string::npos);
    auto [nothing, e14] = answer(r[13]);
    CHECK(e14);
}

TEST_CASE("cli: mcp: notes, fields, tags and collections changed with --write, as ournotes-cli mcp's")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "Old"}}), "key");
    // standard input holds something: no tool reads it (it is the protocol's)
    std::istringstream stdinText("from standard input");
    auto* oldIn = std::cin.rdbuf(stdinText.rdbuf());
    auto r = mcp(cli,
                 {call(1, "add_note", Json{{"item_key", paper}, {"text", "--json is not an option here"}}),
                  call(2, "add_note", Json{{"text", "-"}}),
                  call(3, "set_fields", Json{{"key", paper}, {"fields", Json{{"title", "New"}, {"date", "2020"}}}}),
                  call(4, "tag_items", Json{{"keys", Json::array({paper})}, {"add", Json::array({"one", "two"})}}),
                  call(5, "create_collection", Json{{"name", "Reading"}}),
                  call(6, "file_items", Json{{"keys", Json::array({paper})}, {"collection", "Reading"}}),
                  call(7, "set_fields", Json{{"key", paper}, {"fields", Json{{"noSuchField", "x"}}}}),
                  call(8, "tag_items", Json{{"keys", Json::array({paper})}, {"add", Json::array({"a,b"})}}),
                  call(9, "add_note", Json{{"text", ""}})},
                 true);
    std::cin.rdbuf(oldIn);
    REQUIRE(r.size() == 9);
    CHECK(std::string(std::istreambuf_iterator<char>(stdinText), {}) == "from standard input");

    auto [noteKey, e1] = answer(r[0]);
    CHECK(!e1);
    auto notes = lib.children(mine, paper, "note");
    REQUIRE(notes.size() == 1);
    CHECK(jstr(notes[0], "key") == trim(noteKey));
    CHECK(jstr(notes[0], "note").find("--json is not an option here") != std::string::npos);
    CHECK(lib.changedBy(mine, "item", trim(noteKey)) == "ournotes-cli mcp");

    auto [dash, e2] = answer(r[1]);  // a note of its own: "-" is its text, not "read standard input"
    CHECK(!e2);
    auto standalone = lib.item(mine, trim(dash));
    REQUIRE(standalone);
    CHECK(jstr(*standalone, "note").find("-") != std::string::npos);
    CHECK(jstr(*standalone, "note").find("from standard input") == std::string::npos);

    CHECK(!answer(r[2]).second);
    auto item = *lib.item(mine, paper);
    CHECK(jstr(item, "title") == "New");
    CHECK(jstr(item, "date") == "2020");
    CHECK(!answer(r[3]).second);
    CHECK(lib.item(mine, paper)->at("tags").size() == 2);
    CHECK(!answer(r[4]).second);
    CHECK(!answer(r[5]).second);
    CHECK(lib.item(mine, paper)->at("collections").size() == 1);
    CHECK(answer(r[6]).second);  // refused, with the fields it has
    CHECK(answer(r[7]).second);
    CHECK(answer(r[8]).second);  // an empty note: missing
}

TEST_CASE("cli: mcp: coloured notes, in Markdown or as their HTML, kept as the app keeps them")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "L04"}}), "key");
    const std::string red = "<span style=\"color: #ff6666\">";
    // the user's note, coloured in the app (a formula's colour too)
    auto mineHtml = "<div data-schema-version=\"9\"><h1>L04</h1><p>" + red +
                    "max</span> moves</p><pre class=\"math\" style=\"color: #ff6666\">$$Q$$</pre></div>";
    auto note = jstr(lib.createItem(mine, Json{{"itemType", "note"}, {"parentItem", paper}, {"note", mineHtml}}), "key");

    auto r = mcp(cli,
                 {call(1, "add_note", Json{{"item_key", paper},
                                           {"text", "# Colours\n\n" + red + "red = risk</span>, "
                                                    "<span style=\"color: #2ea8e5\">**blue = key**</span>"}}),
                  // a Markdown note that begins with a colour is Markdown still
                  call(2, "add_note", Json{{"item_key", paper}, {"text", red + "first</span> and **bold**"}}),
                  call(3, "get_item", Json{{"key", note}}),
                  call(4, "edit_note", Json{{"note_key", note}, {"format", "html"},
                                            {"text", replaceAll(mineHtml, "moves", "moves to Q's output")}}),
                  call(5, "edit_note", Json{{"note_key", note}, {"format", "rtf"}, {"text", "x"}}),
                  call(6, "add_note", Json{{"item_key", paper}, {"format", "markdown"}, {"text", "<p>not HTML</p>"}})},
                 true);
    REQUIRE(r.size() == 6);
    auto noteHtml = [&](const Json& reply) { return jstr(*lib.item(mine, trim(answer(reply).first)), "note"); };
    auto first = noteHtml(r[0]);
    CHECK(first.find("<h1>Colours</h1>") != std::string::npos);
    CHECK(first.find(red + "red = risk</span>") != std::string::npos);
    CHECK(first.find("<span style=\"color: #2ea8e5\"><strong>blue = key</strong></span>") != std::string::npos);
    CHECK(noteHtml(r[1]).find("<p>" + red + "first</span> and <strong>bold</strong></p>") != std::string::npos);
    // the note's HTML read, changed in one place, sent back whole: its colours stay
    CHECK(answer(r[2]).first.find("color: #ff6666") != std::string::npos);
    CHECK(!answer(r[3]).second);
    auto edited = jstr(*lib.item(mine, note), "note");
    CHECK(edited.find(red + "max</span> moves to Q's output") != std::string::npos);
    CHECK(edited.find("<pre class=\"math\" style=\"color: #ff6666\">") != std::string::npos);
    CHECK(answer(r[4]).second);
    CHECK(noteHtml(r[5]).find("&lt;p&gt;not HTML&lt;/p&gt;") != std::string::npos);
}

TEST_CASE("cli: mcp: annotations deleted, and only annotations, each told what it was")
{
    Cli cli;
    auto& lib = cli.lib;
    auto mine = lib.defaultLibrary();
    auto paper = jstr(lib.createItem(mine, Json{{"itemType", "journalArticle"}, {"title", "L05"}}), "key");
    auto pdfPath = cli.dir.path / "one.pdf";
    writeSamplePdf(pdfPath);
    auto pdf = jstr(lib.importAttachment(mine, pdfPath, paper), "key");
    auto mark = [&](const std::string& text, const std::string& comment) {
        return jstr(lib.createItem(mine, Json{{"itemType", "annotation"},
                                              {"parentItem", pdf},
                                              {"annotationType", "highlight"},
                                              {"annotationText", text},
                                              {"annotationComment", comment},
                                              {"annotationColor", "#ffd400"},
                                              {"annotationPageLabel", "2"}}),
                    "key");
    };
    auto one = mark("Bellman backup", "why backup"), two = mark("projection", ""), kept = mark("kept", "");

    // without --write: not offered
    auto ro = mcp(cli, {call(1, "delete_annotations", Json{{"keys", Json::array({one})}})});
    CHECK(ro[0]["error"]["code"] == -32602);
    CHECK(lib.item(mine, one));

    // an item among them: none deleted
    auto mixed = mcp(cli, {call(1, "delete_annotations", Json{{"keys", Json::array({two, paper})}})}, true);
    auto [refused, e2] = answer(mixed[0]);
    CHECK(e2);
    CHECK(refused.find("not an annotation") != std::string::npos);
    CHECK(lib.item(mine, two));
    CHECK(lib.item(mine, paper));

    auto r = mcp(cli,
                 {Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                  call(2, "trash_items", Json{{"keys", Json::array({one})}}),  // no trash for annotations
                  call(3, "delete_annotations", Json{{"keys", Json::array({one, two})}})},
                 true);
    REQUIRE(r.size() == 3);
    bool listed = false;
    for (auto& t : r[0]["result"]["tools"])
        if (jstr(t, "name") == "delete_annotations") {
            listed = true;
            CHECK(t["annotations"]["destructiveHint"] == true);
        }
    CHECK(listed);
    auto [trashed, e3] = answer(r[1]);
    CHECK(e3);
    CHECK(trashed.find("delete") != std::string::npos);
    auto [deleted, e4] = answer(r[2]);
    INFO(deleted);
    CHECK(!e4);
    CHECK(deleted.find(one + "  highlight  p. 2  #ffd400  \"Bellman backup\"  comment: why backup") != std::string::npos);
    CHECK(deleted.find(two + "  highlight  p. 2  #ffd400  \"projection\"") != std::string::npos);
    CHECK(!lib.item(mine, one));
    CHECK(!lib.item(mine, two));
    CHECK(lib.item(mine, kept));
    CHECK(lib.item(mine, pdf));
}
