#include <doctest.h>

#include "core/util.h"
#include "fixtures.h"
#include "formats/citation.h"

using namespace fp;
using namespace fp::formats;

namespace {

std::vector<Json> allItems()
{
    return {fixtures::journalArticle(), fixtures::conferencePaper(), fixtures::book(), fixtures::preprint(),
            fixtures::bookSection(),    fixtures::webpage(),         fixtures::thesis()};
}

std::vector<std::string> lines(const std::string& s) { return split(s, '\n'); }

Json authored(const std::vector<std::pair<const char*, const char*>>& names, const char* date)
{
    Json item = Json::parse(R"({"itemType":"journalArticle","title":"T","creators":[]})");
    for (auto& [first, last] : names)
        item["creators"].push_back(Json{{"creatorType", "author"}, {"firstName", first}, {"lastName", last}});
    item["date"] = date;
    return item;
}

} // namespace

TEST_CASE("style list")
{
    std::vector<std::string> ids;
    for (auto& s : builtinStyles())
        ids.push_back(s.id);
    CHECK(ids == std::vector<std::string>{"apa", "chicago-author-date", "ieee", "mla", "harvard", "nature", "vancouver"});
    CHECK_THROWS_AS(formatBibliography(allItems(), "nope", OutputFormat::Text), fp::Error);
}

TEST_CASE("APA 7 bibliography")
{
    auto bib = lines(formatBibliography(allItems(), "apa", OutputFormat::Text));
    REQUIRE(bib.size() == 7);
    CHECK(bib[0] == "Doe, J. (2020). Learning representations. In A. Lovelace & A. M. Turing (Eds.), Handbook of "
                    "machine learning (2nd ed., pp. 10–25). Springer.");
    CHECK(bib[1] == "Goodfellow, I., Bengio, Y., & Courville, A. (2016). Deep learning. MIT Press.");
    CHECK(bib[2] == "He, K., Zhang, X., Ren, S., & Sun, J. (2016). Deep residual learning for image recognition. In "
                    "Proceedings of the IEEE Conference on Computer Vision and Pattern Recognition (pp. 770–778). "
                    "IEEE. https://doi.org/10.1109/CVPR.2016.90");
    CHECK(bib[3] == "LeCun, Y., Bengio, Y., & Hinton, G. (2015). Deep learning. Nature, 521(7553), 436–444. "
                    "https://doi.org/10.1038/nature14539");
    CHECK(bib[4] == "Smith, J. R. (2019). On the theory of everything [Doctoral dissertation, University of "
                    "Cambridge].");
    CHECK(bib[5] == "Vaswani, A., Shazeer, N., Parmar, N., Uszkoreit, J., Jones, L., Gomez, A. N., Kaiser, L., & "
                    "Polosukhin, I. (2017). Attention is all you need (arXiv:1706.03762). arXiv. "
                    "https://doi.org/10.48550/arXiv.1706.03762");
    CHECK(bib[6] == "World Health Organization. (n.d.). Coronavirus disease (COVID-19) pandemic. WHO. Retrieved "
                    "October 18, 2026, from https://www.who.int/emergencies/diseases/novel-coronavirus-2019");

    auto html = formatBibliography({fixtures::journalArticle(), fixtures::book()}, "apa", OutputFormat::Html);
    CHECK(html == "<div class=\"csl-bib-body\">\n"
                  "  <div class=\"csl-entry\">Goodfellow, I., Bengio, Y., &amp; Courville, A. (2016). <i>Deep "
                  "learning</i>. MIT Press.</div>\n"
                  "  <div class=\"csl-entry\">LeCun, Y., Bengio, Y., &amp; Hinton, G. (2015). Deep learning. "
                  "<i>Nature</i>, <i>521</i>(7553), 436–444. https://doi.org/10.1038/nature14539</div>\n"
                  "</div>");

    // 21+ authors: first 19, ellipsis, last
    std::vector<std::pair<const char*, const char*>> many;
    static const char* const lastNames[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",
                                            "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V"};
    for (auto* l : lastNames)
        many.push_back({"Xavier", l});
    auto big = formatBibliography({authored(many, "")}, "apa", OutputFormat::Text);
    CHECK(startsWith(big, "A, X., B, X., C, X."));
    CHECK(big.find("R, X., S, X., … V, X. (n.d.). T.") != std::string::npos);
    CHECK(big.find("T, X.") == std::string::npos);
}

TEST_CASE("APA in-text citations")
{
    auto smithJones = authored({{"Anna", "Smith"}, {"Bob", "Jones"}}, "2020");
    auto lee = authored({{"C", "Lee"}, {"D", "Park"}, {"E", "Kim"}}, "2021-03-04");
    CHECK(formatCitation({smithJones, lee}, "apa") == "(Smith & Jones, 2020; Lee et al., 2021)");
    CHECK(formatCitation({authored({{"Anna", "Smith"}}, "")}, "apa") == "(Smith, n.d.)");
    CHECK(formatCitation({fixtures::webpage()}, "apa") == "(World Health Organization, n.d.)");
    CHECK(formatCitation({smithJones, lee}, "chicago-author-date") == "(Smith and Jones 2020; Lee, Park, and Kim 2021)");
    CHECK(formatCitation({smithJones, lee}, "mla") == "(Smith and Jones; Lee et al.)");
    CHECK(formatCitation({smithJones, lee}, "harvard") == "(Smith and Jones, 2020; Lee, Park and Kim, 2021)");
    CHECK(formatCitation({}, "apa").empty());
}

TEST_CASE("IEEE bibliography and citations")
{
    auto bib = lines(formatBibliography(allItems(), "ieee", OutputFormat::Text));
    REQUIRE(bib.size() == 7);
    CHECK(bib[0] == "[1] Y. LeCun, Y. Bengio, and G. Hinton, “Deep learning,” Nature, vol. 521, no. 7553, pp. "
                    "436–444, May 2015, doi: 10.1038/nature14539.");
    CHECK(bib[1] == "[2] K. He, X. Zhang, S. Ren, and J. Sun, “Deep residual learning for image recognition,” in "
                    "Proceedings of the IEEE Conference on Computer Vision and Pattern Recognition, Las Vegas, NV, "
                    "USA: IEEE, Jun. 2016, pp. 770–778. doi: 10.1109/CVPR.2016.90.");
    CHECK(bib[2] == "[3] I. Goodfellow, Y. Bengio, and A. Courville, Deep learning. Cambridge, MA: MIT Press, 2016.");
    CHECK(bib[3] == "[4] A. Vaswani et al., “Attention is all you need,” Jun. 12, 2017, arXiv: arXiv:1706.03762. doi: "
                    "10.48550/arXiv.1706.03762.");
    CHECK(bib[4] == "[5] J. Doe, “Learning representations,” in Handbook of machine learning, A. Lovelace and A. M. "
                    "Turing, Eds., 2nd ed., Berlin: Springer, 2020, pp. 10–25.");
    CHECK(bib[5] == "[6] World Health Organization, “Coronavirus disease (COVID-19) pandemic.” WHO. Accessed: Oct. "
                    "18, 2026. [Online]. Available: https://www.who.int/emergencies/diseases/novel-coronavirus-2019");
    CHECK(bib[6] == "[7] J. R. Smith, “On the theory of everything,” Ph.D. dissertation, University of Cambridge, "
                    "Cambridge, 2019.");

    auto two = std::vector<Json>{fixtures::journalArticle(), fixtures::book()};
    CHECK(formatCitation(two, "ieee") == "[1], [2]");
    CHECK(formatCitation(allItems(), "ieee") == "[1]–[7]");
    auto html = formatBibliography({fixtures::book()}, "ieee", OutputFormat::Html);
    CHECK(html.find("<div class=\"csl-entry\">[1] I. Goodfellow, Y. Bengio, and A. Courville, <i>Deep learning</i>. "
                    "Cambridge, MA: MIT Press, 2016.</div>") != std::string::npos);
}

TEST_CASE("other styles format a journal article")
{
    std::vector<Json> one = {fixtures::journalArticle()};
    CHECK(formatBibliography(one, "chicago-author-date") ==
          "LeCun, Yann, Yoshua Bengio, and Geoffrey Hinton. 2015. “Deep learning.” Nature 521 (7553): 436–44. "
          "https://doi.org/10.1038/nature14539.");
    CHECK(formatBibliography(one, "mla") ==
          "LeCun, Yann, et al. “Deep learning.” Nature, vol. 521, no. 7553, 28 May 2015, pp. 436–44, "
          "https://doi.org/10.1038/nature14539.");
    CHECK(formatBibliography(one, "harvard") ==
          "LeCun, Y., Bengio, Y. and Hinton, G. (2015) ‘Deep learning’, Nature, 521(7553), pp. 436–444. Available at: "
          "https://doi.org/10.1038/nature14539.");
    CHECK(formatBibliography(one, "nature") ==
          "1. LeCun, Y., Bengio, Y. & Hinton, G. Deep learning. Nature 521, 436–444 (2015).");
    CHECK(formatBibliography(one, "vancouver") ==
          "1. LeCun Y, Bengio Y, Hinton G. Deep learning. Nature. 2015 May 28;521(7553):436–44. "
          "doi:10.1038/nature14539");
    CHECK(formatBibliography(one, "nature", OutputFormat::Html).find("<i>Nature</i> <b>521</b>, 436–444 (2015).") !=
          std::string::npos);
    CHECK(formatCitation({one[0], one[0]}, "nature") == "¹,²");
    CHECK(formatCitation({one[0], one[0]}, "nature", OutputFormat::Html) == "<sup>1,2</sup>");
    CHECK(formatCitation({one[0], one[0], one[0]}, "vancouver") == "(1–3)");
}

TEST_CASE("every style handles every item type")
{
    auto items = allItems();
    items.push_back(Json{{"itemType", "note"}, {"note", "<p>skip me</p>"}});
    items.push_back(Json::parse(R"({"itemType":"report","title":"Annual report","reportNumber":"42",
        "institution":"ACME","place":"Springfield","date":"2001-02","creators":[
        {"creatorType":"editor","firstName":"Eve","lastName":"Editor"}]})"));
    items.push_back(Json::parse(R"({"itemType":"document","title":"Untitled draft","creators":[]})"));
    for (auto& style : builtinStyles()) {
        CAPTURE(style.id);
        auto text = formatBibliography(items, style.id, OutputFormat::Text);
        CAPTURE(text);
        auto entries = lines(text);
        CHECK(entries.size() == 9);
        for (auto& e : entries) {
            CHECK(e.find("..") == std::string::npos);
            CHECK(e.find(" ,") == std::string::npos);
            CHECK(e.find("  ") == std::string::npos);
            CHECK(e.find("()") == std::string::npos);
        }
        CHECK(text.find("Untitled draft") != std::string::npos);
        CHECK(text.find("Editor") != std::string::npos);
        auto html = formatBibliography(items, style.id, OutputFormat::Html);
        size_t divs = 0;
        for (size_t pos = html.find("csl-entry"); pos != std::string::npos; pos = html.find("csl-entry", pos + 1))
            ++divs;
        CHECK(divs == 9);
        CHECK(!formatCitation(items, style.id).empty());
    }
    auto chicago = formatBibliography(items, "chicago-author-date");
    CHECK(chicago.find("Editor, Eve, ed. 2001. Annual report. 42. Springfield: ACME.") != std::string::npos);
    CHECK(chicago.find("Doe, Jane. 2020. “Learning representations.” In Handbook of machine learning, edited by Ada "
                       "Lovelace and Alan M. Turing, 10–25. Berlin: Springer.") != std::string::npos);
    auto harvard = formatBibliography(items, "harvard");
    CHECK(harvard.find("World Health Organization (no date) Coronavirus disease (COVID-19) pandemic. Available at: "
                       "https://www.who.int/emergencies/diseases/novel-coronavirus-2019 (Accessed: 18 October 2026).") !=
          std::string::npos);
    CHECK(harvard.find("Vaswani, A. et al. (2017) ‘Attention is all you need’. arXiv. Available at: "
                       "https://doi.org/10.48550/arXiv.1706.03762.") != std::string::npos);
    auto nature = formatBibliography(items, "nature");
    CHECK(nature.find("4. Vaswani, A. et al. Attention is all you need. Preprint at "
                      "https://doi.org/10.48550/arXiv.1706.03762 (2017).") != std::string::npos);
    CHECK(nature.find("3. Goodfellow, I., Bengio, Y. & Courville, A. Deep learning. (MIT Press, 2016).") !=
          std::string::npos);
    auto mla = formatBibliography(items, "mla");
    CHECK(mla.find("Smith, John Ronald. On the theory of everything. 2019. University of Cambridge, PhD "
                   "dissertation.") != std::string::npos);
    auto vancouver = formatBibliography(items, "vancouver");
    CHECK(vancouver.find("6. World Health Organization. Coronavirus disease (COVID-19) pandemic [Internet]. WHO; "
                         "[cited 2026 Oct 18]. Available from: "
                         "https://www.who.int/emergencies/diseases/novel-coronavirus-2019") != std::string::npos);
}
