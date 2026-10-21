#include <doctest.h>

#include "formats/metadata.h"

using namespace on;
using namespace on::formats;

namespace {

std::vector<Identifier> ids(std::initializer_list<std::pair<const char*, const char*>> list)
{
    std::vector<Identifier> out;
    for (auto& [kind, value] : list)
        out.push_back({kind, value});
    return out;
}

std::string dump(const std::vector<Identifier>& v)
{
    std::string s;
    for (auto& id : v)
        s += id.kind + ":" + id.value + " ";
    return s;
}

} // namespace

TEST_CASE("parseIdentifiers")
{
    auto check = [](const std::string& text, const std::vector<Identifier>& expected) {
        auto got = parseIdentifiers(text);
        CAPTURE(text);
        CAPTURE(dump(got));
        CHECK(got == expected);
    };
    check("10.1038/nature14539", ids({{"doi", "10.1038/nature14539"}}));
    check("https://doi.org/10.1038/NATURE14539", ids({{"doi", "10.1038/NATURE14539"}}));
    check("http://dx.doi.org/10.1002/%28SICI%291097-4636", ids({{"doi", "10.1002/(SICI)1097-4636"}}));
    check("doi:10.1000/xyz123.", ids({{"doi", "10.1000/xyz123"}}));
    check("DOI: 10.1000/abc", ids({{"doi", "10.1000/abc"}}));
    check("(10.1016/S0140-6736(20)30183-5)", ids({{"doi", "10.1016/S0140-6736(20)30183-5"}}));
    check("arXiv:2301.12345v2", ids({{"arxiv", "2301.12345"}}));
    check("2301.12345", ids({{"arxiv", "2301.12345"}}));
    check("arXiv: 1706.03762", ids({{"arxiv", "1706.03762"}}));
    check("https://arxiv.org/abs/2301.08243v3", ids({{"arxiv", "2301.08243"}}));
    check("https://arxiv.org/pdf/2301.08243v1.pdf", ids({{"arxiv", "2301.08243"}}));
    check("hep-th/9901001", ids({{"arxiv", "hep-th/9901001"}}));
    check("arXiv:math.GT/0309136v1", ids({{"arxiv", "math.GT/0309136"}}));
    check("978-0-262-03561-3", ids({{"isbn", "9780262035613"}}));
    check("ISBN 0-262-03561-8", ids({{"isbn", "0262035618"}}));
    check("ISBN:080442957X", ids({{"isbn", "080442957X"}}));
    check("9780262035614", {});            // bad ISBN-13 checksum
    check("0-262-03561-9", {});            // bad ISBN-10 checksum
    check("PMID: 26017442", ids({{"pmid", "26017442"}}));
    check("pmid:26017442", ids({{"pmid", "26017442"}}));
    check("26017442", {});                 // bare numbers are not PMIDs
    check("2399.12345 1234.5678 hello", {});  // invalid month / too short
    check("10.1038/nature14539, 10.1038/NATURE14539\narXiv:1706.03762  1706.03762\n\n9780262035613,PMID: 1",
          ids({{"doi", "10.1038/nature14539"},
               {"arxiv", "1706.03762"},
               {"isbn", "9780262035613"},
               {"pmid", "1"}}));
}

TEST_CASE("findIdentifiersInText on a first page")
{
    const std::string arxivPage =
        "Published as a conference paper at CVPR 2023\n"
        "arXiv:2301.08243v3  [cs.CV]  13 Apr 2023\n"
        "Self-Supervised Learning from Images with a\n"
        "Joint-Embedding Predictive Architecture\n"
        "Mahmoud Assran1,2,3∗ Quentin Duval1 Ishan Misra1 Piotr Bojanowski1\n"
        "1Meta AI (FAIR) 2McGill University 3Mila, Quebec AI Institute\n"
        "Abstract\n"
        "This paper demonstrates an approach for learning highly semantic image representations without "
        "relying on hand-crafted data-augmentations (see https://doi.org/10.1109/CVPR52729.2023.01499).\n"
        "Code: https://github.com/facebookresearch/ijepa. Prior work (doi:10.1038/nature14539, "
        "ISBN-13: 978-0-262-03561-3; PMID: 26017442).\n";
    auto found = findIdentifiersInText(arxivPage);
    CAPTURE(dump(found));
    CHECK(found == ids({{"arxiv", "2301.08243"},
                        {"doi", "10.1109/CVPR52729.2023.01499"},
                        {"doi", "10.1038/nature14539"},
                        {"isbn", "9780262035613"},
                        {"pmid", "26017442"}}));

    auto journal = findIdentifiersInText("Nature | Vol 521 | 28 May 2015 doi:10.1038/nature14539.\n"
                                         "Received 25 February; accepted 1 May 2015. arXiv.org/abs/hep-th/9901001");
    CHECK(journal == ids({{"doi", "10.1038/nature14539"}, {"arxiv", "hep-th/9901001"}}));
    CHECK(findIdentifiersInText("version 110.12345/abc and 10.12/short").empty());
    CHECK(findIdentifiersInText("10.1234/" + std::string(1000000, ')')).empty());
    CHECK(parseIdentifiers(std::string(1000000, '(') + "10.1234/x" + std::string(1000000, ')')) ==
          ids({{"doi", "10.1234/x"}}));
}

TEST_CASE("an arXiv abstract page as an item")
{
    // as arxiv.org/abs/hep-th/9711200 has it (trimmed)
    std::string page = R"(<!DOCTYPE html><html lang="en"><head>
<title>[hep-th/9711200] The Large N Limit of Superconformal Field Theories and Supergravity</title>
<meta name="citation_title" content="The Large N Limit of Superconformal Field Theories and Supergravity" />
<meta name="citation_author" content="Maldacena, Juan M." />
<meta name="citation_author" content="O&#39;Brien, Second" />
<meta name="citation_doi" content="10.1023/A:1026654312961" />
<meta name="citation_date" content="1997/11/27" />
<meta name="citation_online_date" content="1998/01/22" />
<meta name="citation_pdf_url" content="https://arxiv.org/pdf/hep-th/9711200" />
<meta name="citation_arxiv_id" content="hep-th/9711200" />
<meta name="citation_abstract" content="We show that the large $N$ limit
   of certain conformal field theories &amp; more." />
</head><body>
<div class="subheader"><h1>High Energy Physics - Theory</h1></div>
<table summary="Additional metadata"><tr>
          <td class="tablecell label">Comments:</td>
          <td class="tablecell comments mathjax">20 pages, harvmac, v2: section on AdS_2 corrected</td>
        </tr>
<tr>
          <td class="tablecell label">Subjects:</td>
          <td class="tablecell subjects">
            <span class="primary-subject">High Energy Physics - Theory (hep-th)</span></td>
        </tr><tr>
          <td class="tablecell label">Report&nbsp;number:</td>
          <td class="tablecell jref">HUTP-98/A097</td>
        </tr><tr>
          <td class="tablecell label">Journal&nbsp;reference:</td>
          <td class="tablecell jref">Adv.Theor.Math.Phys.2:231-252,1998</td>
        </tr></table></body></html>)";
    auto item = arxivAbsPageToItem(page, "hep-th/9711200v3");
    CAPTURE(item.dump(2));
    CHECK(item["itemType"] == "preprint");
    CHECK(item["title"] == "The Large N Limit of Superconformal Field Theories and Supergravity");
    REQUIRE(item["creators"].size() == 2);
    CHECK(item["creators"][0] == Json{{"creatorType", "author"}, {"firstName", "Juan M."}, {"lastName", "Maldacena"}});
    CHECK(item["creators"][1]["lastName"] == "O'Brien");
    CHECK(item["date"] == "1997-11-27");  // the first version's
    CHECK(item["abstractNote"] == "We show that the large $N$ limit of certain conformal field theories & more.");
    CHECK(item["url"] == "https://arxiv.org/abs/hep-th/9711200");
    CHECK(item["DOI"] == "10.48550/arXiv.hep-th/9711200");
    CHECK(item["archiveID"] == "arXiv:hep-th/9711200");
    CHECK(item["libraryCatalog"] == "arXiv.org");
    CHECK(item["extra"] == "arXiv:hep-th/9711200 [hep-th]\nJournal reference: Adv.Theor.Math.Phys.2:231-252,1998\n"
                           "Published version DOI: 10.1023/A:1026654312961");
    REQUIRE(item.contains("_notes"));
    CHECK(item["_notes"][0] == "<p>Comment: 20 pages, harvmac, v2: section on AdS_2 corrected</p>");

    // a page that is not a paper's (a sign-in wall, an error page)
    CHECK_THROWS_AS(arxivAbsPageToItem("<html><head><title>Just a moment</title></head></html>", "2301.08243"),
                    MetadataError);
}
