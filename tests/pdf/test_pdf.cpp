#include <doctest.h>

#include <fstream>

#include "../core/test_helpers.h"
#include "pdf/document.h"

using namespace fp;
using namespace fp::pdf;

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
    CHECK(text.utf8() == "Hello World from freePen\nsecond line of text");

    // "Hello" baseline is at y=700 in PDF space → ~92 in page space
    auto hit = text.hitTest({80, 85});
    REQUIRE(hit >= 0);
    auto word = text.wordAt(hit);
    CHECK(text.textOf(word.first, word.second) == "Hello");
    auto rects = text.rectsOf(0, (int)text.chars.size() - 1);
    CHECK(rects.size() == 2);
    CHECK(text.textOf(0, (int)text.chars.size() - 1) == "Hello World from freePen second line of text");

    auto hits = doc->search(0, "line of");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0][0].y0 > 100);  // second line: baseline at 132, 24pt font

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
