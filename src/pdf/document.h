#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/util.h"

// Qt-free wrapper around MuPDF. A Document owns its own MuPDF context and must be
// used from one thread at a time; open one Document per thread for parallel work.
//
// Coordinates: "page space" is MuPDF's space for the displayed (rotated) page, in
// points with the origin at the top-left. Annotation positions are stored, like
// Zotero, in PDF user space (origin bottom-left, unrotated); use pdfToPage/pageToPdf.
namespace fp::pdf {

struct Point {
    float x = 0, y = 0;
};

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float width() const { return x1 - x0; }
    float height() const { return y1 - y0; }
    bool isEmpty() const { return x1 <= x0 || y1 <= y0; }
    bool contains(Point p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
    Rect united(const Rect& o) const;
    Rect normalized() const;
};

struct Size {
    float width = 0, height = 0;
};

// 8-bit RGB image, rows of `stride` bytes.
struct Image {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> pixels;
};

struct TextChar {
    uint32_t c = 0;
    Rect box;
    int line = 0;  // index into PageText::lines
};

struct TextLine {
    Rect box;
    int first = 0;  // index of first char
    int count = 0;
    bool vertical = false;
};

struct PageText {
    std::vector<TextChar> chars;  // reading order
    std::vector<TextLine> lines;

    // Index of the char nearest to p (within a line if possible); -1 if no text.
    int hitTest(Point p, bool nearest = true) const;
    // Text of chars [from, to] (inclusive), lines joined with spaces, hyphenation removed.
    std::string textOf(int from, int to) const;
    // One rect per line covering chars [from, to].
    std::vector<Rect> rectsOf(int from, int to) const;
    // Expand an index to the surrounding word / line.
    std::pair<int, int> wordAt(int index) const;
    std::pair<int, int> lineAt(int index) const;
    std::string utf8() const;
};

struct OutlineEntry {
    std::string title;
    int page = -1;
    float y = 0;
    std::string uri;
    std::vector<OutlineEntry> children;
};

struct Link {
    Rect rect;
    int page = -1;  // internal target, -1 for external
    float y = 0;
    std::string uri;
};

class Document {
public:
    // Throws fp::Error on failure.
    static std::unique_ptr<Document> open(const fs::path& path);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    const fs::path& path() const { return path_; }
    bool isPdf() const;
    bool needsPassword() const;
    bool authenticate(const std::string& password);

    int pageCount() const { return pageCount_; }
    Size pageSize(int page);
    std::string pageLabel(int page);
    std::string metadata(const std::string& key);  // "info:Title", "info:Author", "format", ...

    // Page contents without the file's own annotations (those are shown from the
    // library database, see embeddedAnnotations()).
    Image render(int page, float scale);
    Image renderClip(int page, float scale, const Rect& clip);  // clip in page space

    const PageText& text(int page);
    std::string pageText(int page);
    std::string fullText(int maxPages = 0);  // for indexing
    // One entry per hit; a hit spanning several lines has several rects.
    std::vector<std::vector<Rect>> search(int page, const std::string& needle, int maxHits = 512);
    std::vector<OutlineEntry> outline();
    std::vector<Link> links(int page);

    Point pdfToPage(int page, Point p);
    Point pageToPdf(int page, Point p);
    Rect pdfToPage(int page, const Rect& r);
    Rect pageToPdf(int page, const Rect& r);

    // Annotations stored inside the PDF file, as Zotero-style annotation data.
    std::vector<Json> embeddedAnnotations();
    // Write a copy of the document with `annotations` (Zotero-style data) embedded.
    void exportWithAnnotations(const fs::path& out, const std::vector<Json>& annotations);

    struct Impl;  // MuPDF state (document.cpp)

private:
    Document() = default;
    void* loadPage(int page);  // fz_page*, cached

    fs::path path_;
    int pageCount_ = 0;
    std::unique_ptr<Impl> d_;
};

// Zotero's annotationSortIndex: "PPPPP|OOOOOO|TTTTT" (page, char offset, top).
std::string sortIndex(int pageIndex, int charOffset, float top);

} // namespace fp::pdf
