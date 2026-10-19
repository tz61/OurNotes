#include <doctest.h>

#include "core/util.h"
#include "fixtures.h"
#include "formats/bibtex.h"

using namespace fp;
using namespace fp::formats;

TEST_CASE("BibTeX round trip keeps type, title, creators, date, DOI and pages")
{
    auto article = fixtures::journalArticle();
    article.erase("journalAbbreviation");  // not exported
    std::vector<Json> items = {article, fixtures::conferencePaper(), fixtures::book(), fixtures::preprint(),
                               fixtures::bookSection(), fixtures::thesis()};
    auto bib = exportBibtex(items);
    CAPTURE(bib);
    auto back = importBibtex(bib);
    REQUIRE(back.size() == items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        auto expected = items[i];
        expected.erase("extra");
        fixtures::checkSameItem(expected, back[i]);
    }
    CHECK(back[0]["abstractNote"] == article["abstractNote"]);
    CHECK(back[0]["tags"].size() == 2);
    CHECK(back[0]["tags"][1]["tag"] == "neural networks");
    CHECK(back[0]["citationKey"] == "lecun2015deep");
    CHECK(back[3]["extra"] == "arXiv:1706.03762 [cs.CL]");
    CHECK_FALSE(back[0].contains("key"));
}

TEST_CASE("BibTeX export: keys, types and escaping")
{
    auto article = fixtures::journalArticle();
    auto second = fixtures::journalArticle();
    second["title"] = "The deep learning revolution";
    auto withKey = fixtures::book();
    withKey["citationKey"] = "goodfellow_dl";
    auto viaExtra = fixtures::thesis();
    viaExtra["extra"] = "Some note\nCitation Key: smith2019toe";
    auto note = Json{{"itemType", "note"}, {"note", "<p>hi</p>"}};
    auto bib = exportBibtex({article, second, withKey, viaExtra, note});
    CAPTURE(bib);
    CHECK(bib.find("@article{lecun2015deep,") != std::string::npos);
    CHECK(bib.find("@article{lecun2015deepa,") != std::string::npos);
    CHECK(bib.find("@book{goodfellow_dl,") != std::string::npos);
    CHECK(bib.find("@phdthesis{smith2019toe,") != std::string::npos);
    CHECK(bib.find("note = {Some note}") != std::string::npos);
    CHECK(bib.find("school = {University of Cambridge}") != std::string::npos);
    CHECK(bib.find("author = {LeCun, Yann and Bengio, Yoshua and Hinton, Geoffrey}") != std::string::npos);
    CHECK(bib.find("pages = {436--444}") != std::string::npos);
    CHECK(bib.find("month = may,") != std::string::npos);
    CHECK(bib.find("keywords = {machine learning, neural networks}") != std::string::npos);

    Json special = fixtures::book();
    special["title"] = "R&D costs 100% of $5 for #1 a_b {x} \\ and BERT in iOS — ü";
    special["creators"] = Json::parse(R"([{"creatorType":"author","name":"Barnes and Noble"},
                                          {"creatorType":"author","firstName":"Jörg","lastName":"Müller"}])");
    special["_attachments"] = Json::parse(R"([{"title":"Full Text","path":"/home/me/a:b.pdf","contentType":"application/pdf","url":""},
                                              {"title":"Link","path":"","contentType":"text/html","url":"https://x"}])");
    special["_notes"] = Json::array({"<p>First note</p>"});
    auto out = exportBibtex({special});
    CAPTURE(out);
    CHECK(out.find("title = {{R\\&D} costs 100\\% of \\$5 for \\#1 a\\_b \\{x\\} \\textbackslash{} and {BERT} in {iOS} "
                   "— ü}") != std::string::npos);
    CHECK(out.find("author = {{Barnes and Noble} and Müller, Jörg}") != std::string::npos);
    CHECK(out.find("file = {Full Text:/home/me/a\\:b.pdf:application/pdf}") != std::string::npos);
    CHECK(out.find("annote = {First note}") != std::string::npos);
    CHECK(out.find("@book{barnesandnoble2016rd,") != std::string::npos);

    auto back = importBibtex(out);
    REQUIRE(back.size() == 1);
    CHECK(back[0]["title"] == special["title"]);
    CHECK(back[0]["creators"][0]["name"] == "Barnes and Noble");
    CHECK(back[0]["_files"] == Json::array({"/home/me/a:b.pdf"}));
    CHECK(back[0]["_notes"][0].get<std::string>().find("First note") != std::string::npos);
}

TEST_CASE("BibTeX export type mapping")
{
    auto master = fixtures::thesis();
    master["thesisType"] = "Master's thesis";
    auto web = fixtures::webpage();
    Json program = Json::parse(R"({"itemType":"computerProgram","title":"freePen","creators":[
        {"creatorType":"programmer","firstName":"A","lastName":"Dev"}],"versionNumber":"1.0","date":"2026"})");
    Json report = Json::parse(R"({"itemType":"report","title":"A report","reportNumber":"TR-7",
        "institution":"ACME Labs","date":"2001"})");
    auto bib = exportBibtex({master, web, program, report, fixtures::preprint(), fixtures::bookSection(),
                             fixtures::conferencePaper()});
    CAPTURE(bib);
    CHECK(bib.find("@mastersthesis{") != std::string::npos);
    CHECK(bib.find("@misc{worldhealthorganizationcoronavirus,") != std::string::npos);
    CHECK(bib.find("howpublished = {\\url{https://www.who.int/emergencies/diseases/novel-coronavirus-2019}}") !=
          std::string::npos);
    CHECK(bib.find("urldate = {2026-10-18}") != std::string::npos);
    CHECK(bib.find("@software{dev2026freepen,") != std::string::npos);
    CHECK(bib.find("@techreport{") != std::string::npos);
    CHECK(bib.find("institution = {ACME Labs}") != std::string::npos);
    CHECK(bib.find("number = {TR-7}") != std::string::npos);
    CHECK(bib.find("eprint = {1706.03762}") != std::string::npos);
    CHECK(bib.find("archivePrefix = {arXiv}") != std::string::npos);
    CHECK(bib.find("primaryClass = {cs.CL}") != std::string::npos);
    CHECK(bib.find("@incollection{doe2020learning,") != std::string::npos);
    CHECK(bib.find("booktitle = {Handbook of machine learning}") != std::string::npos);
    CHECK(bib.find("editor = {Lovelace, Ada and Turing, Alan M.}") != std::string::npos);
    CHECK(bib.find("@inproceedings{he2016deep,") != std::string::npos);
    CHECK(bib.find("booktitle = {Proceedings of the {IEEE} Conference") == std::string::npos);
    CHECK(bib.find("booktitle = {Proceedings of the IEEE Conference on Computer Vision and Pattern Recognition}") !=
          std::string::npos);

    auto back = importBibtex(bib);
    REQUIRE(back.size() == 7);
    CHECK(back[0]["itemType"] == "thesis");
    CHECK(back[0]["thesisType"] == "Master's thesis");
    CHECK(back[1]["itemType"] == "webpage");
    CHECK(back[1]["url"] == web["url"]);
    CHECK(back[1]["accessDate"] == "2026-10-18");
    CHECK(back[2]["itemType"] == "computerProgram");
    CHECK(back[2]["creators"][0]["creatorType"] == "programmer");
    CHECK(back[3]["reportNumber"] == "TR-7");
}

TEST_CASE("BibTeX parser edge cases")
{
    const char* bib = R"(
% A comment line with an @ sign: someone@example.org
@string{jgr = "J. Geophys. Res."}
@STRING{ pub = {Springer} }
@comment{ ignored {nested} block @article{fake, title={no}} }
@preamble{ "\newcommand{\noopsort}[1]{}" }

@Article{muller2020,
  author = {M{\"u}ller, J{\"o}rg and Fran{\c c}ois Dupont and {\v{C}}apek, Karel and {World Health Organization}
            and D'Angelo, Ana and Ludwig van Beethoven and de la Fontaine, Jean and King, Jr., Martin},
  title = {{Nested {Braces} in} the {T}itle: An {\'e}tude of \LaTeX{} -- and \emph{more}},
  journal = jgr # { Letters},
  publisher = pub,
  year = 2020,
  month = mar,
  volume = "12",
  number = {3},
  pages = {100--110},
  note = "Quoted {value} with {\ss}",
  keywords = {alpha; beta, gamma},
  doi = {https://doi.org/10.1000/xyz\_123},
  file = {Full Text:/home/user/papers/muller.pdf:application/pdf;Snapshot:/tmp/snap.html:text/html},
}

@book(knuth84, author = "Donald E. Knuth", title = "The {\TeX}book", publisher = "Addison-Wesley", year = 1984)

@article{broken, title = {Unclosed value
  year = 2000

@misc{vaswani2017, author = {Vaswani, Ashish and others}, title = {Attention Is All You Need},
  eprint = {1706.03762v5}, archivePrefix = {arXiv}, primaryClass = {cs.CL}, year = {2017}}

@article{scholar, title={Attention is all you need}, author={Vaswani, Ashish},
  journal={arXiv preprint arXiv:1706.03762}, year={2017}}

@article{dblp, author = {A. Author}, title = {T}, journal = {CoRR}, volume = {abs/2301.08243}, year = {2023},
  eprinttype = {arXiv}, eprint = {2301.08243}}

@inproceedings{conf, author={Smith, A.}, title={Paper}, booktitle={Proc. of Things}, year={2019},
  month={June}, day={5}, address={Paris}, organization={ACM}}
)";
    auto items = importBibtex(bib);
    std::string keys;
    for (auto& it : items)
        keys += it.value("citationKey", "?") + " ";
    CAPTURE(keys);
    REQUIRE(items.size() == 6);

    auto& m = items[0];
    CAPTURE(m.dump(2));
    CHECK(m["itemType"] == "journalArticle");
    CHECK(m["citationKey"] == "muller2020");
    CHECK(m["title"] == "Nested Braces in the Title: An étude of LaTeX – and more");
    CHECK(m["publicationTitle"] == "J. Geophys. Res. Letters");
    CHECK(m["date"] == "2020-03");
    CHECK(m["volume"] == "12");
    CHECK(m["issue"] == "3");
    CHECK(m["pages"] == "100-110");
    CHECK(m["DOI"] == "10.1000/xyz_123");
    CHECK(m["publisher"] == "Springer");
    CHECK(m["extra"] == "Quoted value with ß");
    CHECK(m["_files"] == Json::array({"/home/user/papers/muller.pdf", "/tmp/snap.html"}));
    REQUIRE(m["tags"].size() == 3);
    CHECK(m["tags"][0] == Json{{"tag", "alpha"}, {"type", 0}});
    const auto& c = m["creators"];
    REQUIRE(c.size() == 8);
    CHECK(c[0] == Json{{"creatorType", "author"}, {"firstName", "Jörg"}, {"lastName", "Müller"}});
    CHECK(c[1]["firstName"] == "François");
    CHECK(c[1]["lastName"] == "Dupont");
    CHECK(c[2]["lastName"] == "Čapek");
    CHECK(c[3] == Json{{"creatorType", "author"}, {"name", "World Health Organization"}});
    CHECK(c[4]["lastName"] == "D'Angelo");
    CHECK(c[5]["firstName"] == "Ludwig");
    CHECK(c[5]["lastName"] == "van Beethoven");
    CHECK(c[6]["lastName"] == "de la Fontaine");
    CHECK(c[7]["firstName"] == "Martin, Jr.");

    CHECK(items[1]["itemType"] == "book");
    CHECK(items[1]["title"] == "The TeXbook");
    CHECK(items[1]["creators"][0]["firstName"] == "Donald E.");
    CHECK(items[1]["date"] == "1984");

    // the unterminated entry is dropped, parsing resumes at the next entry
    auto& v = items[2];
    CHECK(v["itemType"] == "preprint");
    CHECK(v["creators"].size() == 1);
    CHECK(v["archiveID"] == "arXiv:1706.03762");
    CHECK(v["DOI"] == "10.48550/arXiv.1706.03762");
    CHECK(v["repository"] == "arXiv");
    CHECK(v["url"] == "https://arxiv.org/abs/1706.03762");

    CHECK(items[3]["itemType"] == "preprint");
    CHECK(items[3]["archiveID"] == "arXiv:1706.03762");
    CHECK(items[4]["itemType"] == "preprint");
    CHECK(items[4]["archiveID"] == "arXiv:2301.08243");
    CHECK_FALSE(items[4].contains("volume"));

    CHECK(items[5]["itemType"] == "conferencePaper");
    CHECK(items[5]["proceedingsTitle"] == "Proc. of Things");
    CHECK(items[5]["date"] == "2019-06-05");
    CHECK(items[5]["place"] == "Paris");
    CHECK(items[5]["publisher"] == "ACM");
}

TEST_CASE("LaTeX decoding")
{
    CHECK(latexToUnicode("Caf{\\'e} \\'{e}t\\'e \\v{c} \\v c {\\ss} R\\&D 1--2 a---b A~B") ==
          "Café été č č ß R&D 1–2 a—b A B");
    CHECK(latexToUnicode("{\\\"o} \\\"{o} \\c{c} {\\o} \\'{\\i} \\~n \\u{a} \\H{o} \\k{a} \\r{a} \\={a} \\.{z} \\^{o} "
                         "\\`{a} {\\aa} \\\"{\\i} \\v{Z}") == "ö ö ç ø í ñ ă ő ą å ā ż ô à å ï Ž");
    CHECK(latexToUnicode("$\\alpha$-helix and \\textit{in vivo} ``quoted'' \\url{http://x.org/a_b}") ==
          "α-helix and in vivo “quoted” http://x.org/a_b");
    CHECK(latexToUnicode("Stra\\ss e \\textbackslash{} 50\\% \\$ \\#1 \\_ \\{\\}") == "Straße \\ 50% $ #1 _ {}");
    CHECK(latexToUnicode("\\noopsort{a}Zeta \\unknown{kept}") == "Zeta kept");
}

TEST_CASE("BibTeX parser is linear on large and broken input")
{
    std::string big;
    for (int i = 0; i < 3000; ++i)
        big += "@article{k" + std::to_string(i) + ", title = {Title " + std::to_string(i) +
               "}, author = {A, B and C, D}, year = 2000}\n";
    big += "@article{bad, title = {" + std::string(200000, '{');
    auto items = importBibtex(big);
    CHECK(items.size() == 3000);

    std::string deep;
    for (int i = 0; i < 100000; ++i)
        deep += "\\textit{";
    deep += "x" + std::string(100000, '}');
    CHECK(latexToUnicode(deep) == "x");
    std::string accents;
    for (int i = 0; i < 100000; ++i)
        accents += "\\\"";
    CHECK(latexToUnicode(accents + "o") == "o");
}
