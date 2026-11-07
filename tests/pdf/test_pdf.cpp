#include <doctest.h>

#include <cmath>
#include <fstream>
#include <vector>
#include <thread>
#include <atomic>

#include "../core/test_helpers.h"
#include "pdf/document.h"

using namespace on;
using namespace on::pdf;

#include "sample_pdf.h"

TEST_CASE("open, text, hit testing, search")
{
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    auto doc = Document::open(file);
    REQUIRE(doc->pageCount() == 1);
    CHECK(doc->isPdf());
    auto size = doc->pageSize(0);
    CHECK(size.width == doctest::Approx(612));
    CHECK(size.height == doctest::Approx(792));
    CHECK(doc->pageLabel(0) == "1");

    auto& text = doc->text(0);
    CHECK(text.lines.size() == 2);
    CHECK(text.utf8() == "Hello World from OurNotes\nsecond line of text");

    // "Hello" baseline is at y=700 in PDF space → ~92 in page space
    auto hit = text.hitTest({80, 85});
    REQUIRE(hit >= 0);
    auto word = text.wordAt(hit);
    CHECK(text.textOf(word.first, word.second) == "Hello");
    auto rects = text.rectsOf(0, (int)text.chars.size() - 1);
    CHECK(rects.size() == 2);
    CHECK(text.textOf(0, (int)text.chars.size() - 1) == "Hello World from OurNotes second line of text");

    auto hits = doc->search(0, "line of");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0][0].y0 > 100);  // second line: baseline at 132, 24pt font
    // in any case, or as written
    CHECK(doc->search(0, "hello world").size() == 1);
    CHECK(doc->search(0, "hello world", 512, true).empty());
    CHECK(doc->search(0, "Hello World", 512, true).size() == 1);

    auto img = doc->render(0, 0.5f);
    CHECK(img.width == 306);
    CHECK(img.height == 396);
    CHECK(img.pixels.size() == (size_t)img.stride * img.height);
}

TEST_CASE("coordinate conversion handles rotation")
{
    TempDir tmp;
    auto file = tmp.path / "rot.pdf";
    writeSamplePdf(file, 90);
    auto doc = Document::open(file);
    auto size = doc->pageSize(0);
    CHECK(size.width == doctest::Approx(792));
    Point pdfPt{72, 700};
    auto pagePt = doc->pdfToPage(0, pdfPt);
    auto back = doc->pageToPdf(0, pagePt);
    CHECK(back.x == doctest::Approx(72));
    CHECK(back.y == doctest::Approx(700));
}

TEST_CASE("export annotations and read them back")
{
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    auto doc = Document::open(file);
    auto& text = doc->text(0);
    auto rect = text.rectsOf(0, 4)[0];  // "Hello"
    auto pdfRect = doc->pageToPdf(0, rect);
    Json highlight{{"annotationType", "highlight"},
                   {"annotationColor", "#ff6666"},
                   {"annotationComment", "greeting"},
                   {"annotationPosition",
                    {{"pageIndex", 0}, {"rects", Json::array({{pdfRect.x0, pdfRect.y0, pdfRect.x1, pdfRect.y1}})}}}};
    Json ink{{"annotationType", "ink"},
             {"annotationColor", "#2ea8e5"},
             {"annotationPosition",
              {{"pageIndex", 0}, {"width", 3}, {"paths", Json::array({Json::array({100, 100, 150, 150, 200, 120})})}}}};
    auto out = tmp.path / "annotated.pdf";
    doc->exportWithAnnotations(out, {highlight, ink});

    auto reread = Document::open(out);
    auto anns = reread->embeddedAnnotations();
    REQUIRE(anns.size() == 2);
    CHECK(anns[0]["annotationType"] == "highlight");
    CHECK(anns[0]["annotationColor"] == "#ff6666");
    CHECK(anns[0]["annotationComment"] == "greeting");
    CHECK(anns[0]["annotationText"] == "Hello");
    auto r = anns[0]["annotationPosition"]["rects"][0];
    CHECK(r[0].get<float>() == doctest::Approx(pdfRect.x0).epsilon(0.01));
    CHECK(anns[1]["annotationType"] == "ink");
    CHECK(anns[1]["annotationPosition"]["paths"][0].size() == 6);
    // annotations are not part of render() output: the page stays identical
    auto a = doc->render(0, 0.25f), b = reread->render(0, 0.25f);
    REQUIRE(a.pixels.size() == b.pixels.size());
    size_t differing = 0;
    int maxDiff = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i)
        if (a.pixels[i] != b.pixels[i]) {
            ++differing;
            maxDiff = std::max(maxDiff, std::abs(a.pixels[i] - b.pixels[i]));
        }
    MESSAGE("differing bytes: " << differing << " max diff " << maxDiff);
    CHECK(maxDiff <= 8);
}

TEST_CASE("sort index format")
{
    CHECK(sortIndex(3, 904, 257.8f) == "00003|000904|00257");
}

TEST_CASE("image areas of a page, within their clipping")
{
    TempDir tmp;
    auto file = tmp.path / "image.pdf";
    writeImagePdf(file);
    auto doc = Document::open(file);
    auto rects = doc->imageRects(0);
    REQUIRE(rects.size() == 1);
    // page space: top-left origin, so PDF y 500..600 is 192..292
    CHECK(rects[0].x0 == doctest::Approx(72).epsilon(0.01));
    CHECK(rects[0].x1 == doctest::Approx(172).epsilon(0.01));
    CHECK(rects[0].y0 == doctest::Approx(192).epsilon(0.01));
    CHECK(rects[0].y1 == doctest::Approx(292).epsilon(0.01));

    writeImagePdf(file, true);
    auto clipped = Document::open(file);
    rects = clipped->imageRects(0);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].x1 == doctest::Approx(122).epsilon(0.01));

    TempDir tmp2;
    writeSamplePdf(tmp2.path / "text.pdf");
    CHECK(Document::open(tmp2.path / "text.pdf")->imageRects(0).empty());
}

TEST_CASE("the word near a point: on it, close to it, beside a space or a comma; none far away")
{
    TempDir tmp;
    auto file = tmp.path / "words.pdf";
    writeTextPdf(file, {{{72, 100, "One two, three"}, {72, 114, "four five six"}}});
    auto doc = Document::open(file);
    auto& text = doc->text(0);
    auto wordAtPoint = [&](float x, float y) {
        auto [a, b] = text.wordNear({x, y});
        return a < 0 ? std::string("(none)") : text.textOf(a, b);
    };
    auto boxOf = [&](int i) { return text.chars[i].box; };
    auto middle = [&](const Rect& r) { return Point{(r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2}; };
    auto one = middle(boxOf(1));
    CHECK(wordAtPoint(one.x, one.y) == "One");
    // just above and under the letters, and by the end of a line
    CHECK(wordAtPoint(one.x, boxOf(1).y0 - 3) == "One");
    auto six = boxOf((int)text.chars.size() - 1);
    CHECK(wordAtPoint(six.x1 + 3, (six.y0 + six.y1) / 2) == "six");
    // a space: the word on that side
    auto space = boxOf(3);
    CHECK(wordAtPoint(space.x0 + 0.2f, (space.y0 + space.y1) / 2) == "One");
    CHECK(wordAtPoint(space.x1 - 0.2f, (space.y0 + space.y1) / 2) == "two");
    // a comma too
    auto comma = boxOf(7);
    CHECK(text.chars[7].c == ',');
    CHECK(wordAtPoint(comma.x0 + 0.2f, (comma.y0 + comma.y1) / 2) == "two");
    // far off: none
    CHECK(wordAtPoint(300, 100) == "(none)");
    CHECK(wordAtPoint(one.x, boxOf(1).y0 - 30) == "(none)");
    CHECK(wordAtPoint(one.x, text.lines[1].box.y1 + 30) == "(none)");
}

namespace {

// A page's links as text: what each one's box holds, and where it leads
std::vector<std::string> linksOf(Document& doc, int page)
{
    std::vector<std::string> out;
    auto& text = doc.text(page);
    for (auto& link : doc.links(page)) {
        std::string letters;
        for (auto& c : text.chars) {
            Point middle{(c.box.x0 + c.box.x1) / 2, (c.box.y0 + c.box.y1) / 2};
            if (link.rect.contains(middle) && c.c < 128)
                letters += (char)c.c;
        }
        out.push_back(letters + " p" + std::to_string(link.page + 1) + " y" + std::to_string((int)std::lround(link.y)));
    }
    return out;
}

using Lines = std::vector<std::string>;

} // namespace

TEST_CASE("citations a PDF does not link are links to their entries in its list of references")
{
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    std::vector<PdfTextLine> references = {{72, 100, "R EFERENCES"},
                                           {72, 120, "[1] A. Author. The first paper."},
                                           {72, 134, "Journal of Tests, 2020."},
                                           {72, 160, "[2] B. Author. The second paper."},
                                           {330, 100, "[3] C. Author. The third paper."}};  // (the next column)
    SUBCASE("each number, with its brackets, leads to its entry; in any order, by ranges; not values")
    {
        writeTextPdf(file, {{{72, 100, "As shown in [2], [3], and in [3, 1] and [1\xB1" "3] and [2;3] too."},
                             {72, 120, "HOVER[2] and search[3, 2, 1]."}},
                            references,
                            {{72, 100, "As in [2], with the appendix."}}});
        auto doc = Document::open(file);
        REQUIRE(doc->links(0).size() == 12);
        auto links = linksOf(*doc, 0);
        auto entry1 = doc->text(1).lines[1].box.y0, entry3 = doc->text(1).lines[4].box.y0;
        CHECK(links[0].rfind("[2] p2 y", 0) == 0);
        CHECK(links[1].rfind("[3] p2 y", 0) == 0);
        CHECK(links[2].rfind("[3 p2 y", 0) == 0);
        CHECK(links[3].rfind("1] p2 y" + std::to_string((int)std::lround(entry1)), 0) == 0);
        CHECK(links[4].rfind("[1 p2", 0) == 0);  // (a range: its two ends)
        CHECK(links[5].rfind("3] p2", 0) == 0);
        CHECK(links[6].rfind("[2 p2", 0) == 0);
        CHECK(links[7].rfind("3] p2", 0) == 0);
        CHECK(links[8].rfind("[2] p2", 0) == 0);  // (right after a word)
        CHECK(links[9].rfind("[3 p2", 0) == 0);
        // where it leads: the entry's line, there in its column
        auto link = doc->links(0)[1];
        CHECK(link.page == 1);
        CHECK(link.x == doctest::Approx(330).epsilon(0.02));
        CHECK(link.y == doctest::Approx(entry3));
        CHECK(link.uri == "#citation=3");
        CHECK(doc->links(1).empty());  // the entries themselves: no links
        CHECK(linksOf(*doc, 2) == Lines{"[2] p2 y" + std::to_string((int)std::lround(doc->text(1).lines[3].box.y0))});
    }
    SUBCASE("written like citations: no links")
    {
        writeTextPdf(file, {{{72, 100, "Not x[1], ab[2], Table[1, 3], 2[1], [1]x, [0, 1], [0.5, 1], [1, a]."},
                             {72, 120, "Nor [2, 2], [9], [1, 9], [3\xB1" "1], = [1, 2], [1, 2][1, 3], [1, 2],[2, 3]."},
                             {72, 140, "[1, 3]"}},  // (a table's cell)
                            references});
        auto doc = Document::open(file);
        CHECK(linksOf(*doc, 0).empty());
    }
    SUBCASE("what the file links stays as it is, only the rest is added")
    {
        std::string line = "See [1] and [2] here.";
        writeTextPdf(file, {{{72, 100, line}}, references}, {{0, 90, 112, 100, 1, 300}});
        auto doc = Document::open(file);
        auto links = doc->links(0);
        REQUIRE(links.size() == 2);
        CHECK(links[0].y == doctest::Approx(300));  // (the file's, where the file says)
        CHECK(links[0].uri.rfind("#citation=", 0) != 0);
        CHECK(links[1].uri == "#citation=2");
    }
    SUBCASE("no list of references: no links")
    {
        writeTextPdf(file, {{{72, 100, "See [1] and [2], in the range [1, 2]."}}, {{72, 100, "The end."}}});
        auto doc = Document::open(file);
        CHECK(doc->links(0).empty());
        // nor a line or two beginning so
        writeTextPdf(file, {{{72, 100, "See [1] and [2]."}, {72, 120, "[1] one."}, {72, 140, "[2] two."}}});
        CHECK(Document::open(file)->links(0).empty());
    }
    SUBCASE("an entry that did not come out of the text: the others are found, it has no link")
    {
        writeTextPdf(file, {{{72, 100, "See [2] and [3], [2, 3]."}},
                            {{72, 100, "[1] one."}, {72, 120, "[3] three."}, {72, 140, "[4] four."}}});
        auto doc = Document::open(file);
        CHECK(linksOf(*doc, 0).size() == 2);
        CHECK(linksOf(*doc, 0)[0].rfind("[3] p2", 0) == 0);
        CHECK(linksOf(*doc, 0)[1].rfind("3] p2", 0) == 0);
    }
    SUBCASE("references written \"12.\": only those under the heading, not numbered steps or sections")
    {
        writeTextPdf(file, {{{72, 100, "1. Introduction"},
                             {72, 120, "2. Our method"},
                             {72, 140, "3. Results"},
                             {72, 160, "As in [2]."}},
                            {{72, 100, "References"},
                             {72, 120, "1. A. Author."},
                             {72, 140, "2. B. Author."},
                             {72, 160, "3. C. Author."}},
                            {{72, 100, "1. One."},
                             {72, 120, "2. Two."},
                             {72, 140, "3. Three."},
                             {72, 160, "As in [3]."}}});
        auto doc = Document::open(file);
        auto y2 = doc->text(1).lines[2].box.y0, y3 = doc->text(1).lines[3].box.y0;
        CHECK(linksOf(*doc, 0) == Lines{"[2] p2 y" + std::to_string((int)std::lround(y2))});
        CHECK(doc->links(1).empty());
        CHECK(linksOf(*doc, 2) == Lines{"[3] p2 y" + std::to_string((int)std::lround(y3))});
        // the same, without the heading: steps
        writeTextPdf(file, {{{72, 100, "1. One."}, {72, 120, "2. Two."}, {72, 140, "3. Three."}, {72, 160, "As in [2]."}}});
        CHECK(Document::open(file)->links(0).empty());
    }
    SUBCASE("several lists: the one after the citation (a chapter's); a few numbered lines are none")
    {
        std::vector<PdfTextLine> list;
        for (int i = 1; i <= 6; ++i)
            list.push_back({72, 100.0 + 20 * i, "[" + std::to_string(i) + "] An author, a paper."});
        writeTextPdf(file, {{{72, 100, "As in [2]."}}, list, {{72, 100, "As in [2]."}}, list, {{72, 100, "As in [2]."}}});
        auto doc = Document::open(file);
        CHECK(doc->links(0)[0].page == 1);
        CHECK(doc->links(2)[0].page == 3);
        CHECK(doc->links(4)[0].page == 3);  // (after them all: the last one)

        std::vector<PdfTextLine> many;
        for (int i = 1; i <= 16; ++i)
            many.push_back({72, 40.0 + 20 * i, "[" + std::to_string(i) + "] An author, a paper."});
        writeTextPdf(file, {{{72, 100, "As in [2]."}, {72, 120, "[1] a step"}, {72, 140, "[2] a step"}, {72, 160, "[3] a step"}},
                            many});
        CHECK(Document::open(file)->links(0)[0].page == 1);
    }
    SUBCASE("a citation on the page of the list: before it, and after it")
    {
        writeTextPdf(file, {{{72, 100, "Before it, as in [2]."},
                             {72, 120, "[1] one."},
                             {72, 140, "[2] two."},
                             {72, 160, "[3] three."},
                             {72, 180, "After it, as in [1]."}}});
        auto doc = Document::open(file);
        REQUIRE(linksOf(*doc, 0).size() == 2);
        CHECK(linksOf(*doc, 0)[0].rfind("[2] p1", 0) == 0);
        CHECK(linksOf(*doc, 0)[1].rfind("[1] p1", 0) == 0);
    }
}

TEST_CASE("documents can be used on several threads at once")
{
    // HTML layout shapes text with HarfBuzz, whose allocator context MuPDF keeps for the
    // whole process; without shared locks, threads clear it under each other and crash
    TempDir tmp;
    auto file = tmp.path / "page.xhtml";
    std::string body;
    for (int i = 0; i < 200; ++i)
        body += "<p>Paragraph " + std::to_string(i) + " with some text to shape and lay out, "
                "including ligatures ffi fl and numbers 12345.</p>\n";
    std::ofstream(file) << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                           "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
                        << body << "</body></html>\n";
    std::atomic<int> finished{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 15; ++i) {
                auto doc = Document::open(file);
                doc->fullText(2);
                doc->render(0, 0.5f);
            }
            ++finished;
        });
    for (auto& t : threads)
        t.join();
    CHECK(finished == 4);
}
