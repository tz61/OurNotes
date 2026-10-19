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
