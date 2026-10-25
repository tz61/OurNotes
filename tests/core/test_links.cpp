#include <doctest.h>

#include "core/library.h"
#include "core/links.h"
#include "test_helpers.h"

using namespace on;

TEST_CASE("links: ournotes://item/ links made and read back")
{
    ItemLink link{"zotero-user-11", "ABCD2345", 3, std::array<double, 4>{72, 690.25, 160.04, -0.01}, "WXYZ6789"};
    auto url = itemLinkUrl(link);
    CHECK(url == "ournotes://item/zotero-user-11/ABCD2345?page=3&rect=72,690.3,160,0&annotation=WXYZ6789");
    auto back = parseItemLink(url);
    REQUIRE(back);
    CHECK(back->library == "zotero-user-11");
    CHECK(back->key == "ABCD2345");
    CHECK(back->page == 3);
    REQUIRE(back->rect);
    CHECK((*back->rect)[1] == doctest::Approx(690.3));
    CHECK(back->annotation == "WXYZ6789");

    // the library alone, its uid as it is (an odd one: encoded)
    CHECK(itemLinkUrl({"a b/c", "ABCD2345", 0, std::nullopt, {}}) == "ournotes://item/a%20b%2Fc/ABCD2345");
    CHECK(parseItemLink("ournotes://item/a%20b%2Fc/ABCD2345")->library == "a b/c");
    // the scheme in capitals, a slash at the end, what does not read left out
    back = parseItemLink("OurNotes://item/lib/ABCD2345/?page=x&rect=1,2,3&annotation=b%21d&other=1");
    REQUIRE(back);
    CHECK(back->page == 0);
    CHECK(!back->rect);
    CHECK(back->annotation.empty());
    // not one
    for (auto* other : {"https://example.com", "ournotes://item/lib", "ournotes://item//ABCD2345",
                        "ournotes://item/lib/not%20a%20key", "zotero://select/library/items/ABCD2345"})
        CHECK_MESSAGE(!parseItemLink(other), other);
}

TEST_CASE("links: references to an item, a note, a PDF's page and an annotation")
{
    TempDir tmp;
    Library lib(tmp.path);
    auto id = lib.defaultLibrary();
    auto uid = lib.library(id)->uid;
    auto paper = lib.createItem(id, Json{{"itemType", "journalArticle"},
                                         {"title", "A Paper"},
                                         {"date", "2020-05-01"},
                                         {"creators", Json::array({Json{{"creatorType", "author"},
                                                                        {"firstName", "Ann"},
                                                                        {"lastName", "Smith"}},
                                                                   Json{{"creatorType", "author"},
                                                                        {"firstName", "Bo"},
                                                                        {"lastName", "Jones"}}})}});
    auto key = jstr(paper, "key");
    auto pdf = lib.createItem(id, Json{{"itemType", "attachment"},
                                       {"parentItem", key},
                                       {"linkMode", "linked_file"},
                                       {"title", "Full Text PDF"},
                                       {"contentType", "application/pdf"},
                                       {"path", "/nowhere/paper.pdf"}});
    auto pdfKey = jstr(pdf, "key");
    auto ann = lib.createItem(id, Json{{"itemType", "annotation"},
                                       {"parentItem", pdfKey},
                                       {"annotationType", "highlight"},
                                       {"annotationText", "Hello"},
                                       {"annotationColor", "#ffd400"},
                                       {"annotationPageLabel", "iii"},
                                       {"annotationPosition", Json{{"pageIndex", 2}, {"rects", Json::array()}}}});
    auto note = lib.createNote(id, "<h1>Reading plan</h1><p>more</p>", key);

    auto ref = referenceTo(lib, id, key);
    CHECK(ref.url == "ournotes://item/" + uid + "/" + key);
    CHECK(ref.text == "(Smith and Jones, 2020)");
    // a page of its PDF: by its label, else its number; with a place on it
    ref = referenceTo(lib, id, pdfKey, 5, "v", std::array<double, 4>{1, 2, 3, 4});
    CHECK(ref.url == "ournotes://item/" + uid + "/" + pdfKey + "?page=5&rect=1,2,3,4");
    CHECK(ref.text == "(Smith and Jones, 2020, p. v)");
    CHECK(referenceTo(lib, id, pdfKey, 5).text == "(Smith and Jones, 2020, p. 5)");
    // an annotation: its PDF, at its page
    ref = referenceTo(lib, id, jstr(ann, "key"));
    CHECK(ref.url == "ournotes://item/" + uid + "/" + pdfKey + "?page=3&annotation=" + jstr(ann, "key"));
    CHECK(ref.text == "(Smith and Jones, 2020, p. iii)");
    // a note: its title
    ref = referenceTo(lib, id, jstr(note, "key"));
    CHECK(ref.url == "ournotes://item/" + uid + "/" + jstr(note, "key"));
    CHECK(ref.text == "Reading plan");
    CHECK_THROWS(referenceTo(lib, id, "ZZZZZZZZ"));

    // as a note has it, in Markdown, as text
    ref = referenceTo(lib, id, pdfKey, 5);
    CHECK(referenceHtml(ref, "a < b") ==
          "“a &lt; b” <a href=\"" + ref.url + "\">(Smith and Jones, 2020, p. 5)</a>");
    CHECK(referenceHtml(ref) == "<a href=\"" + ref.url + "\">(Smith and Jones, 2020, p. 5)</a>");
    CHECK(referenceMarkdown(ref, "quoted") == "“quoted” [(Smith and Jones, 2020, p. 5)](" + ref.url + ")");
    CHECK(referenceText(ref, "quoted") == "“quoted” (Smith and Jones, 2020, p. 5)");
    CHECK(referenceMarkdown({"ournotes://item/l/ABCD2345", "[draft] plan"}) == "[(draft) plan](ournotes://item/l/ABCD2345)");
}
