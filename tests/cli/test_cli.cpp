// ournotes-cli as an agent uses it (docs/agents.md): the commands run in-process, their
// output captured.
#include <doctest.h>

#include <iostream>
#include <sstream>

#include "../core/test_helpers.h"
#include "../pdf/sample_pdf.h"
#include "cli/commands.h"
#include "core/config.h"
#include "core/library.h"

using namespace on;

namespace {

struct Cli {
    TempDir dir;
    Library lib{dir.path / "data"};
    Config config{dir.path / "config.json"};
    CommandMap commands;

    Cli()
    {
        registerAgentCommands(commands);
        registerExtraCommands(commands, config);
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
