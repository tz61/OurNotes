#include <doctest.h>

#include "core/hash.h"
#include "core/schema.h"
#include "core/util.h"

using namespace fp;

TEST_CASE("md5 and sha256 test vectors")
{
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string million(1000000, 'a');
    CHECK(sha256Hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(md5Hex(million) == "7707d6ae4e027c70eea2a935c2296f21");
    // RFC 7914 section 11
    CHECK(toHex(pbkdf2Sha256("passwd", "salt", 1, 64)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST_CASE("normalize folds case and accents")
{
    CHECK(normalize("Ångström ÉCOLE Straße") == "angstrom ecole strasse");
    CHECK(normalize("Łódź Čapek") == "lodz capek");
    CHECK(normalize("ΑΒΓ") == "αβγ");
}

TEST_CASE("string helpers")
{
    CHECK(yearOf("2023-05-01") == "2023");
    CHECK(yearOf("May 12, 1999") == "1999");
    CHECK(yearOf("12345") == "");
    CHECK(searchTokens("foo \"Bar baz\"  qux") == std::vector<std::string>{"foo", "bar baz", "qux"});
    CHECK(htmlToText("<div><p>Hello &amp; <b>world</b></p><p>Second</p></div>") == "Hello & world\nSecond");
    CHECK(noteTitle("<h1>Title</h1><p>body</p>") == "Title");
    CHECK(safeFilename("a/b:c?.pdf") == "a_b_c_.pdf");
    CHECK(isoToEpoch("1970-01-02T00:00:00Z") == 86400);
    CHECK(epochToIso(86400) == "1970-01-02T00:00:00Z");
    CHECK(isValidKey(newKey()));
    CHECK(newKey().size() == 8);
}

TEST_CASE("creator summary")
{
    Json c = Json::parse(R"([{"creatorType":"author","firstName":"A","lastName":"Smith"},
                             {"creatorType":"author","firstName":"B","lastName":"Jones"},
                             {"creatorType":"editor","name":"Org"}])");
    CHECK(creatorSummary(c) == "Smith and Jones");
    c.push_back(Json{{"creatorType", "author"}, {"lastName", "Lee"}});
    CHECK(creatorSummary(c) == "Smith et al.");
}

TEST_CASE("schema")
{
    CHECK(schema::isValidType("journalArticle"));
    CHECK(schema::hasField("journalArticle", "DOI"));
    CHECK(schema::typeLabel("journalArticle") == "Journal Article");
    CHECK(schema::fieldLabel("abstractNote") == "Abstract");
    CHECK(schema::primaryCreatorType("computerProgram") == "programmer");
    Json d{{"itemType", "conferencePaper"}, {"title", "T"}, {"proceedingsTitle", "Proc"}};
    CHECK(schema::getField(d, "publicationTitle") == "Proc");
    Json dropped;
    auto j = schema::convertType(d, "journalArticle", &dropped);
    CHECK(j["publicationTitle"] == "Proc");
    CHECK(j["itemType"] == "journalArticle");
}
