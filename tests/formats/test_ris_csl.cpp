#include <doctest.h>

#include "core/util.h"
#include "fixtures.h"
#include "formats/common.h"
#include "formats/csljson.h"
#include "formats/formats.h"
#include "formats/ris.h"

using namespace on;
using namespace on::formats;

TEST_CASE("RIS round trip")
{
    auto conf = fixtures::conferencePaper();
    conf.erase("conferenceName");  // no RIS tag
    auto pre = fixtures::preprint();
    pre.erase("extra");
    std::vector<Json> items = {fixtures::journalArticle(), conf, fixtures::book(), fixtures::bookSection(), pre,
                               fixtures::thesis(), fixtures::webpage()};
    auto ris = exportRis(items);
    CAPTURE(ris);
    CHECK(startsWith(ris, "TY  - JOUR\r\nTI  - Deep learning\r\nAU  - LeCun, Yann\r\n"));
    CHECK(ris.find("DA  - 2015/05/28/\r\nPY  - 2015\r\n") != std::string::npos);
    CHECK(ris.find("SP  - 436\r\nEP  - 444\r\n") != std::string::npos);
    CHECK(ris.find("TY  - CPAPER\r\n") != std::string::npos);
    CHECK(ris.find("TY  - UNPB\r\n") != std::string::npos);
    CHECK(ris.find("A2  - Lovelace, Ada\r\n") != std::string::npos);
    CHECK(ris.find("AU  - World Health Organization\r\n") != std::string::npos);
    CHECK(ris.find("ER  - \r\n") != std::string::npos);

    auto back = importRis(ris);
    REQUIRE(back.size() == items.size());
    for (size_t i = 0; i < items.size(); ++i)
        fixtures::checkSameItem(items[i], back[i]);
    CHECK(back[0]["abstractNote"] == items[0]["abstractNote"]);
    CHECK(back[0]["tags"].size() == 2);
    CHECK(back[6]["accessDate"] == "2026-10-18");
}

TEST_CASE("RIS import of typical database output")
{
    std::string ris = "\xEF\xBB\xBFTY  - JOUR\r\n"
                      "T1  - A study of things\r\n"
                      "A1  - Smith, John\r\n"
                      "A1  - Doe, Jane A.\r\n"
                      "JF  - Journal of Stuff\r\n"
                      "JA  - J. Stuff\r\n"
                      "Y1  - 2019/03/15/\r\n"
                      "PY  - 2019\r\n"
                      "VL  - 7\r\n"
                      "IS  - 2\r\n"
                      "SP  - 123\r\n"
                      "EP  - 130\r\n"
                      "N2  - First line of abstract\r\n"
                      "continued here.\r\n"
                      "KW  - one\r\n"
                      "KW  - two\r\n"
                      "SN  - 1234-5678\r\n"
                      "DO  - https://doi.org/10.1234/stuff.2019\r\n"
                      "UR  - https://example.org/stuff\r\n"
                      "N1  - A plain note\r\n"
                      "L1  - file:///home/user/My%20Papers/stuff.pdf\r\n"
                      "ER  - \r\n"
                      "\r\n"
                      "TY  - CHAP\r\n"
                      "TI  - Chapter one\r\n"
                      "AU  - Brown, Bob\r\n"
                      "ED  - Editor, Eve\r\n"
                      "BT  - The big book\r\n"
                      "PB  - Big Press\r\n"
                      "CY  - London\r\n"
                      "PY  - 2010///\r\n"
                      "SP  - 1-20\r\n"
                      "ER  -\r\n"
                      "TY  - CONF\n"
                      "TI  - Conf paper\n"
                      "AU  - Lee, Kim\n"
                      "T2  - Proc. Conf.\n"
                      "DA  - 2021/07/01\n"
                      "ER  - \n"
                      "TY  - UNPB\n"
                      "TI  - A preprint\n"
                      "AU  - Ng, Andrew\n"
                      "DO  - 10.48550/arXiv.2101.00001\n"
                      "ER  - \n"
                      "TY  - UNPB\n"
                      "TI  - Unpublished manuscript\n"
                      "ER  - \n"
                      "TY  - BOOK\n"
                      "BT  - Book via BT\n"
                      "SN  - 978-0-262-03561-3\n"
                      "ER  - \n";
    auto items = importRis(ris);
    REQUIRE(items.size() == 6);
    auto& a = items[0];
    CAPTURE(a.dump(2));
    CHECK(a["itemType"] == "journalArticle");
    CHECK(a["title"] == "A study of things");
    CHECK(a["creators"][1] == Json{{"creatorType", "author"}, {"firstName", "Jane A."}, {"lastName", "Doe"}});
    CHECK(a["publicationTitle"] == "Journal of Stuff");
    CHECK(a["journalAbbreviation"] == "J. Stuff");
    CHECK(a["date"] == "2019-03-15");
    CHECK(a["pages"] == "123-130");
    CHECK(a["abstractNote"] == "First line of abstract\ncontinued here.");
    CHECK(a["ISSN"] == "1234-5678");
    CHECK(a["DOI"] == "10.1234/stuff.2019");
    CHECK(a["tags"].size() == 2);
    CHECK(a["_notes"][0] == textToNoteHtml("A plain note"));
    CHECK(a["_files"] == Json::array({"/home/user/My Papers/stuff.pdf"}));

    CHECK(items[1]["itemType"] == "bookSection");
    CHECK(items[1]["bookTitle"] == "The big book");
    CHECK(items[1]["creators"][1]["creatorType"] == "editor");
    CHECK(items[1]["date"] == "2010");
    CHECK(items[1]["pages"] == "1-20");
    CHECK(items[2]["itemType"] == "conferencePaper");
    CHECK(items[2]["proceedingsTitle"] == "Proc. Conf.");
    CHECK(items[2]["date"] == "2021-07-01");
    CHECK(items[3]["itemType"] == "preprint");
    CHECK(items[4]["itemType"] == "manuscript");
    CHECK(items[5]["itemType"] == "book");
    CHECK(items[5]["title"] == "Book via BT");
    CHECK(items[5]["ISBN"] == "978-0-262-03561-3");
}

TEST_CASE("date parsing")
{
    auto check = [](const char* s, int y, int m, int d) {
        CAPTURE(s);
        auto p = parseDate(s);
        CHECK(p.year == y);
        CHECK(p.month == m);
        CHECK(p.day == d);
    };
    check("2023-05-01", 2023, 5, 1);
    check("2023-05", 2023, 5, 0);
    check("May 2023", 2023, 5, 0);
    check("12 May 2023", 2023, 5, 12);
    check("May 12, 2023", 2023, 5, 12);
    check("Sept. 3rd, 1999", 1999, 9, 3);
    check("2023", 2023, 0, 0);
    check("5/12/2023", 2023, 5, 12);
    check("13/05/2023", 2023, 5, 13);
    check("12.05.2023", 2023, 5, 12);
    check("2015 May 28", 2015, 5, 28);
    check("2023/05/01/", 2023, 5, 1);
    check("2010///", 2010, 0, 0);
    check("2026-10-18T09:00:00Z", 2026, 10, 18);
    check("Spring 2020", 2020, 0, 0);
    check("2023-13-45", 2023, 0, 0);
    check("n.d.", 0, 0, 0);
    check("", 0, 0, 0);
    CHECK(isoDate(parseDate("May 12, 2023")) == "2023-05-12");
    CHECK(isoDate(parseDate("May 2023")) == "2023-05");
}

TEST_CASE("CSL JSON conversion")
{
    auto csl = toCsl(fixtures::journalArticle());
    CAPTURE(csl.dump(2));
    CHECK(csl["id"] == "ABCD2345");
    CHECK(csl["type"] == "article-journal");
    CHECK(csl["title"] == "Deep learning");
    CHECK(csl["container-title"] == "Nature");
    CHECK(csl["page"] == "436-444");
    CHECK(csl["volume"] == "521");
    CHECK(csl["issue"] == "7553");
    CHECK(csl["DOI"] == "10.1038/nature14539");
    CHECK(csl["URL"] == "https://www.nature.com/articles/nature14539");
    CHECK(csl["issued"] == Json::parse(R"({"date-parts":[[2015,5,28]]})"));
    CHECK(csl["author"][0] == Json{{"family", "LeCun"}, {"given", "Yann"}});
    CHECK(csl["keyword"] == "machine learning, neural networks");

    auto conf = toCsl(fixtures::conferencePaper());
    CHECK(conf["type"] == "paper-conference");
    CHECK(conf["container-title"] == "Proceedings of the IEEE Conference on Computer Vision and Pattern Recognition");
    CHECK(conf["event-title"] == "CVPR 2016");
    CHECK(conf["publisher-place"] == "Las Vegas, NV, USA");
    CHECK(conf["issued"] == Json::parse(R"({"date-parts":[[2016,6]]})"));

    auto pre = toCsl(fixtures::preprint());
    CHECK(pre["type"] == "article");
    CHECK(pre["number"] == "arXiv:1706.03762");
    CHECK(pre["publisher"] == "arXiv");
    CHECK(pre["note"] == "arXiv:1706.03762 [cs.CL]");

    auto web = toCsl(fixtures::webpage());
    CHECK(web["author"][0] == Json{{"literal", "World Health Organization"}});
    CHECK(web["accessed"] == Json::parse(R"({"date-parts":[[2026,10,18]]})"));
    CHECK(web["container-title"] == "WHO");
    CHECK_FALSE(web.contains("issued"));

    auto th = toCsl(fixtures::thesis());
    CHECK(th["genre"] == "PhD thesis");
    CHECK(th["publisher"] == "University of Cambridge");

    Json dated = fixtures::book();
    for (auto [input, expected] : std::vector<std::pair<std::string, std::string>>{
             {"May 2023", R"({"date-parts":[[2023,5]]})"},
             {"12 May 2023", R"({"date-parts":[[2023,5,12]]})"},
             {"5/12/2023", R"({"date-parts":[[2023,5,12]]})"},
             {"2023", R"({"date-parts":[[2023]]})"},
             {"sometime", R"({"raw":"sometime"})"}}) {
        dated["date"] = input;
        CHECK(toCsl(dated)["issued"] == Json::parse(expected));
    }
    CHECK(toCsl(Json{{"itemType", "book"}, {"title", "x"}})["id"].get<std::string>().size() == 8);
}

TEST_CASE("CSL JSON round trip and import")
{
    std::vector<Json> items = {fixtures::journalArticle(), fixtures::conferencePaper(), fixtures::book(),
                               fixtures::bookSection(), fixtures::preprint(),       fixtures::thesis(),
                               fixtures::webpage()};
    auto json = exportCslJson(items);
    CHECK(startsWith(json, "[\n  {\n    \"id\": \"ABCD2345\""));
    auto back = importCslJson(json);
    REQUIRE(back.size() == items.size());
    for (size_t i = 0; i < items.size(); ++i)
        fixtures::checkSameItem(items[i], back[i]);
    CHECK(back[4]["extra"] == "arXiv:1706.03762 [cs.CL]");
    CHECK(back[0]["tags"].size() == 2);

    auto imported = importCslJson(R"({"type":"chapter","title":"T","container-title":"Book",
        "issued":{"date-parts":[["2020","3"]]},
        "author":[{"family":"Beethoven","given":"Ludwig","non-dropping-particle":"van"},{"literal":"ACME"}],
        "editor":[{"family":"Ed","given":"E."}],"page":"5-9","unknown-var":"x"})");
    REQUIRE(imported.size() == 1);
    auto& c = imported[0];
    CAPTURE(c.dump(2));
    CHECK(c["itemType"] == "bookSection");
    CHECK(c["bookTitle"] == "Book");
    CHECK(c["date"] == "2020-03");
    CHECK(c["pages"] == "5-9");
    CHECK(c["creators"][0]["lastName"] == "van Beethoven");
    CHECK(c["creators"][1]["name"] == "ACME");
    CHECK(c["creators"][2]["creatorType"] == "editor");
    CHECK_FALSE(c.contains("unknown-var"));

    CHECK_THROWS_AS(importCslJson("not json"), on::Error);
    CHECK(importCslJson("[]").empty());
}

TEST_CASE("format registry and detection")
{
    REQUIRE(allFormats().size() == 3);
    REQUIRE(findFormat("ris") != nullptr);
    CHECK(findFormat("ris")->extension == ".ris");
    CHECK(findFormat("nope") == nullptr);
    auto items = std::vector<Json>{fixtures::book()};
    for (auto& f : allFormats()) {
        auto text = f.exporter(items);
        CAPTURE(f.id);
        CHECK(detectFormat(text) == f.id);
        auto back = f.importer(text);
        REQUIRE(back.size() == 1);
        CHECK(back[0]["title"] == "Deep learning");
    }
    CHECK(detectFormat("  \n@Article { key,\n title = {x}}") == "bibtex");
    CHECK(detectFormat("\xEF\xBB\xBF\r\nTY  - JOUR\r\nER  - \r\n") == "ris");
    CHECK(detectFormat("[{\"type\":\"book\",\"title\":\"x\"}]") == "csljson");
    CHECK(detectFormat("hello world") == "");
    CHECK(detectFormat("") == "");
}
