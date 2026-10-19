// Live lookups against Crossref, arXiv and Open Library. Set OURNOTES_NO_NETWORK=1 to skip.
#include <doctest.h>

#include <cstdlib>

#include "core/util.h"
#include "formats/metadata.h"

using namespace on;
using namespace on::formats;

namespace {

bool noNetwork()
{
    const char* v = std::getenv("OURNOTES_NO_NETWORK");
    return v && std::string(v) == "1";
}

} // namespace

TEST_SUITE("network" * doctest::skip(noNetwork()))
{
    TEST_CASE("DOI lookup via Crossref")
    {
        auto item = lookup({"doi", "10.1038/nature14539"});
        CAPTURE(item.dump(2));
        CHECK(item["itemType"] == "journalArticle");
        CHECK(item["title"] == "Deep learning");
        REQUIRE(item["creators"].size() == 3);
        CHECK(item["creators"][0] == Json{{"creatorType", "author"}, {"firstName", "Yann"}, {"lastName", "LeCun"}});
        CHECK(item["publicationTitle"] == "Nature");
        CHECK(item["volume"] == "521");
        CHECK(item["issue"] == "7553");
        CHECK(item["pages"] == "436-444");
        CHECK(startsWith(item.value("date", ""), "2015-05"));
        CHECK(item["DOI"] == "10.1038/nature14539");
        CHECK(item["libraryCatalog"] == "DOI.org (Crossref)");
        CHECK(item.contains("ISSN"));
        CHECK(item.contains("accessDate"));
        CHECK_FALSE(item.contains("key"));
    }

    TEST_CASE("arXiv lookup")
    {
        auto item = lookup({"arxiv", "2301.08243"});
        CAPTURE(item.dump(2));
        CHECK(item["itemType"] == "preprint");
        CHECK(item["title"] ==
              "Self-Supervised Learning from Images with a Joint-Embedding Predictive Architecture");
        CHECK(item["creators"].size() >= 8);
        CHECK(item["creators"][0] == Json{{"creatorType", "author"}, {"firstName", "Mahmoud"}, {"lastName", "Assran"}});
        CHECK(item["date"] == "2023-01-19");
        CHECK(item["url"] == "https://arxiv.org/abs/2301.08243");
        CHECK(item["DOI"] == "10.48550/arXiv.2301.08243");
        CHECK(item["repository"] == "arXiv");
        CHECK(item["archiveID"] == "arXiv:2301.08243");
        CHECK(item["libraryCatalog"] == "arXiv.org");
        CHECK(startsWith(item.value("extra", ""), "arXiv:2301.08243 [cs.CV]"));
        CHECK(item.value("abstractNote", "").find("\n") == std::string::npos);
        REQUIRE(item.contains("_notes"));
        CHECK(startsWith(item["_notes"][0].get<std::string>(), "<p>Comment: "));

        // arXiv DOIs are resolved through arXiv as well
        auto viaDoi = lookup({"doi", "10.48550/arXiv.2301.08243"});
        CHECK(viaDoi["title"] == item["title"]);
        CHECK_THROWS_AS(lookup({"arxiv", "2312.99999"}), MetadataError);
    }

    TEST_CASE("ISBN lookup via Open Library")
    {
        auto item = lookup({"isbn", "9780262035613"});
        CAPTURE(item.dump(2));
        CHECK(item["itemType"] == "book");
        CHECK(item["title"] == "Deep Learning");
        REQUIRE(item["creators"].size() >= 3);  // Open Library also lists the series editor
        CHECK(item["creators"][0] == Json{{"creatorType", "author"}, {"firstName", "Ian"}, {"lastName", "Goodfellow"}});
        CHECK(item.value("publisher", "").find("MIT") != std::string::npos);
        CHECK(startsWith(item.value("date", ""), "201"));
        CHECK(item.contains("numPages"));
        CHECK(item["ISBN"] == "9780262035613");
        CHECK(startsWith(item.value("url", ""), "http"));
        CHECK_THROWS_AS(lookup({"isbn", "9798888888884"}), MetadataError);
    }

    TEST_CASE("PubMed lookup")
    {
        auto item = lookup({"pmid", "26017442"});
        CAPTURE(item.dump(2));
        CHECK(item["itemType"] == "journalArticle");
        CHECK(item["title"] == "Deep learning");
        CHECK(item["creators"][0] == Json{{"creatorType", "author"}, {"firstName", "Yann"}, {"lastName", "LeCun"}});
        CHECK(item["date"] == "2015-05-28");
        CHECK(item["pages"] == "436-444");
        CHECK(item["DOI"] == "10.1038/nature14539");
        CHECK(item["PMID"] == "26017442");
        CHECK(startsWith(item.value("abstractNote", ""), "Deep learning allows"));
    }

    TEST_CASE("title lookup via Crossref")
    {
        auto item = lookupByTitle("Deep learning", "LeCun");
        REQUIRE(item.has_value());
        CAPTURE(item->dump(2));
        CHECK(normalize(item->value("title", "")) == "deep learning");
        CHECK(item->value("DOI", "") == "10.1038/nature14539");
        CHECK(item->value("itemType", "") == "journalArticle");

        auto longer = lookupByTitle("Deep Residual Learning for Image Recognition");
        REQUIRE(longer.has_value());
        CHECK(normalize(longer->value("title", "")) == "deep residual learning for image recognition");
        CHECK_FALSE(lookupByTitle("zq xv ournotes nonexistent title kplw").has_value());
    }

    TEST_CASE("unknown DOI and non-PDF downloads fail cleanly")
    {
        CHECK_THROWS_AS(lookup({"doi", "10.1038/this-does-not-exist-ournotes"}), MetadataError);
        auto dir = fs::temp_directory_path() / ("ournotes-test-" + newKey());
        CHECK_THROWS_AS(downloadFile("https://export.arxiv.org/api/query?id_list=2301.08243", dir / "x.pdf", true),
                        MetadataError);
        CHECK_FALSE(fs::exists(dir / "x.pdf"));
        CHECK_FALSE(fs::exists(dir / "x.pdf.part"));
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
}

TEST_CASE("titleSimilarity")
{
    CHECK(titleSimilarity("Deep learning", "Deep Learning.") == doctest::Approx(1.0));
    CHECK(titleSimilarity("Attention is all you need", "Attention Is All You Need!") == doctest::Approx(1.0));
    CHECK(titleSimilarity("Deep learning", "Deep reinforcement learning") < 0.9);
    CHECK(titleSimilarity("", "x") == 0.0);
    CHECK(arxivPdfUrl("arXiv:2301.08243") == "https://arxiv.org/pdf/2301.08243");
}
