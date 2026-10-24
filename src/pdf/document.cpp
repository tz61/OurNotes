#include "pdf/document.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <mutex>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

// MuPDF reports errors with setjmp/longjmp (fz_try/fz_catch). Inside those blocks
// we only call C functions and assign to variables declared outside, then turn
// failures into C++ exceptions after the block.

namespace on::pdf {

namespace {

constexpr size_t kPageCache = 12;

Rect toRect(const fz_rect& r) { return {r.x0, r.y0, r.x1, r.y1}; }
fz_rect toFz(const Rect& r) { return fz_make_rect(r.x0, r.y0, r.x1, r.y1); }

bool isWordChar(uint32_t c)
{
    if (c < 0x80)
        return std::isalnum((int)c) || c == '_' || c == '\'' || c == '-';
    return c != 0xA0 && c != 0x2013 && c != 0x2014 && c != 0x201C && c != 0x201D;
}

void silence(void*, const char*) {}

// MuPDF keeps some state for the whole process, e.g. the allocator context HarfBuzz uses
// while shaping text for HTML and EPUB layout, which is set and cleared around each call.
// It guards that with these locks, which only work if every context shares them:
// documents are used on several threads at once (the window, page rendering, the
// full-text indexer). Without them, one thread cleared the context under another and
// MuPDF crashed in fz_free.
std::mutex g_mupdfLocks[FZ_LOCK_MAX];
void lockMupdf(void*, int lock) { g_mupdfLocks[lock].lock(); }
void unlockMupdf(void*, int lock) { g_mupdfLocks[lock].unlock(); }
fz_locks_context g_mupdfLockContext{nullptr, lockMupdf, unlockMupdf};

// Map Unicode mathematical alphanumerics (𝑧, 𝜙, ℰ, 𝟙 ...) to plain letters, as
// NFKC does in pdf.js, so selections and searches read like the visible text.
uint32_t foldMathChar(uint32_t c)
{
    if (c >= 0x1D400 && c <= 0x1D6A3) {
        uint32_t i = (c - 0x1D400) % 52;
        return i < 26 ? 'A' + i : 'a' + (i - 26);
    }
    if (c >= 0x1D6A8 && c <= 0x1D7C9) {
        static const uint32_t variants[] = {0x3B5, 0x3B8, 0x3BA, 0x3C6, 0x3C1, 0x3C0};
        uint32_t i = (c - 0x1D6A8) % 58;
        if (i < 25)
            return i == 17 ? 0x398 : 0x391 + i;
        if (i == 25)
            return 0x2207;
        if (i < 51)
            return 0x3B1 + (i - 26);
        if (i == 51)
            return 0x2202;
        return variants[i - 52];
    }
    if (c >= 0x1D7CE && c <= 0x1D7FF)
        return '0' + (c - 0x1D7CE) % 10;
    if (c >= 0x2100 && c <= 0x2134) {
        static const std::pair<uint32_t, char> letterlike[] = {
            {0x2102, 'C'}, {0x210A, 'g'}, {0x210B, 'H'}, {0x210C, 'H'}, {0x210D, 'H'}, {0x210E, 'h'},
            {0x2110, 'I'}, {0x2111, 'I'}, {0x2112, 'L'}, {0x2113, 'l'}, {0x2115, 'N'}, {0x2119, 'P'},
            {0x211A, 'Q'}, {0x211B, 'R'}, {0x211C, 'R'}, {0x211D, 'R'}, {0x2124, 'Z'}, {0x2128, 'Z'},
            {0x212C, 'B'}, {0x212D, 'C'}, {0x212F, 'e'}, {0x2130, 'E'}, {0x2131, 'F'}, {0x2133, 'M'},
            {0x2134, 'o'}};
        for (auto& [from, to] : letterlike)
            if (c == from)
                return (uint32_t)to;
    }
    return c;
}

std::string colorHex(int n, const float* c)
{
    float r = 1, g = 0.83f, b = 0;
    if (n == 1)
        r = g = b = c[0];
    else if (n == 3) {
        r = c[0];
        g = c[1];
        b = c[2];
    } else if (n == 4) {
        r = (1 - c[0]) * (1 - c[3]);
        g = (1 - c[1]) * (1 - c[3]);
        b = (1 - c[2]) * (1 - c[3]);
    }
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", (int)std::lround(r * 255), (int)std::lround(g * 255),
                  (int)std::lround(b * 255));
    return buf;
}

void parseColor(const std::string& hex, float rgb[3])
{
    unsigned v = 0xffd400;
    if (hex.size() == 7 && hex[0] == '#')
        v = (unsigned)std::strtoul(hex.c_str() + 1, nullptr, 16);
    rgb[0] = ((v >> 16) & 0xff) / 255.f;
    rgb[1] = ((v >> 8) & 0xff) / 255.f;
    rgb[2] = (v & 0xff) / 255.f;
}

} // namespace

// --- geometry ------------------------------------------------------------------------

Rect Rect::united(const Rect& o) const
{
    if (isEmpty())
        return o;
    if (o.isEmpty())
        return *this;
    return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
}

Rect Rect::normalized() const { return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)}; }

std::string sortIndex(int pageIndex, int charOffset, float top)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%05d|%06d|%05d", std::max(0, pageIndex), std::max(0, charOffset),
                  std::max(0, (int)std::floor(top)));
    return buf;
}

// --- PageText ------------------------------------------------------------------------

int PageText::hitTest(Point p, bool nearest) const
{
    for (size_t i = 0; i < chars.size(); ++i)
        if (chars[i].box.contains(p))
            return (int)i;
    if (!nearest || lines.empty())
        return -1;
    // Closest line (vertical distance first, then horizontal), then closest char in it.
    int bestLine = -1;
    float best = 1e9f;
    for (size_t l = 0; l < lines.size(); ++l) {
        auto& b = lines[l].box;
        float dy = p.y < b.y0 ? b.y0 - p.y : p.y > b.y1 ? p.y - b.y1 : 0;
        float dx = p.x < b.x0 ? b.x0 - p.x : p.x > b.x1 ? p.x - b.x1 : 0;
        float d = dy * 4 + dx;
        if (d < best) {
            best = d;
            bestLine = (int)l;
        }
    }
    auto& line = lines[bestLine];
    int bestChar = line.first;
    best = 1e9f;
    for (int i = line.first; i < line.first + line.count; ++i) {
        auto& b = chars[i].box;
        float cx = (b.x0 + b.x1) / 2;
        float d = std::fabs(cx - p.x);
        if (d < best) {
            best = d;
            bestChar = i;
        }
    }
    return bestChar;
}

std::string PageText::textOf(int from, int to) const
{
    if (from > to)
        std::swap(from, to);
    from = std::max(from, 0);
    to = std::min(to, (int)chars.size() - 1);
    std::string out;
    for (int i = from; i <= to; ++i) {
        if (i > from && chars[i].line != chars[i - 1].line) {
            uint32_t prev = chars[i - 1].c;
            uint32_t next = chars[i].c;
            bool hyphen = (prev == '-' || prev == 0xAD) && next < 0x80 && std::islower((int)next);
            if (hyphen) {
                while (!out.empty() && (out.back() == '-'))
                    out.pop_back();
                if (prev == 0xAD && out.size() >= 2)
                    out.erase(out.size() - 2);  // U+00AD is two bytes in UTF-8
            } else if (!out.empty() && out.back() != ' ') {
                out += ' ';
            }
        }
        if (chars[i].c == 0xAD && i < to && chars[i + 1].line == chars[i].line)
            continue;
        appendUtf8(out, chars[i].c);
    }
    return trim(out);
}

std::vector<Rect> PageText::rectsOf(int from, int to) const
{
    if (from > to)
        std::swap(from, to);
    std::vector<Rect> out;
    int curLine = -1;
    for (int i = std::max(from, 0); i <= to && i < (int)chars.size(); ++i) {
        auto& ch = chars[i];
        if (ch.line != curLine) {
            out.push_back(ch.box);
            curLine = ch.line;
        } else {
            out.back() = out.back().united(ch.box);
        }
    }
    return out;
}

std::pair<int, int> PageText::wordAt(int index) const
{
    if (index < 0 || index >= (int)chars.size())
        return {-1, -1};
    int line = chars[index].line;
    int a = index, b = index;
    if (!isWordChar(chars[index].c) || chars[index].c == ' ')
        return {index, index};
    while (a > 0 && chars[a - 1].line == line && isWordChar(chars[a - 1].c) && chars[a - 1].c != ' ')
        --a;
    while (b + 1 < (int)chars.size() && chars[b + 1].line == line && isWordChar(chars[b + 1].c) &&
           chars[b + 1].c != ' ')
        ++b;
    return {a, b};
}

std::pair<int, int> PageText::lineAt(int index) const
{
    if (index < 0 || index >= (int)chars.size())
        return {-1, -1};
    auto& l = lines[chars[index].line];
    return {l.first, l.first + l.count - 1};
}

std::string PageText::utf8() const
{
    std::string out;
    for (size_t i = 0; i < chars.size(); ++i) {
        if (i > 0 && chars[i].line != chars[i - 1].line)
            out += '\n';
        appendUtf8(out, chars[i].c);
    }
    return out;
}

// --- Document ---------------------------------------------------------------------------

struct Document::Impl {
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
    pdf_document* pdf = nullptr;
    std::map<int, fz_page*> pages;
    std::list<int> lru;
    std::map<int, PageText> texts;
    std::map<int, fz_matrix> pdfToPage;

    ~Impl()
    {
        if (!ctx)
            return;
        for (auto& [n, p] : pages)
            fz_drop_page(ctx, p);
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
    }

    [[noreturn]] void fail(const std::string& what)
    {
        throw Error(what + ": " + fz_caught_message(ctx));
    }
};

std::unique_ptr<Document> Document::open(const fs::path& path)
{
    std::unique_ptr<Document> d(new Document);
    d->path_ = path;
    d->d_ = std::make_unique<Impl>();
    auto& im = *d->d_;
    im.ctx = fz_new_context(nullptr, &g_mupdfLockContext, 128 << 20);
    if (!im.ctx)
        throw Error("cannot initialise MuPDF");
    fz_set_warning_callback(im.ctx, silence, nullptr);
    fz_set_error_callback(im.ctx, silence, nullptr);
    std::string file = pathString(path);
    fz_document* doc = nullptr;
    int pages = 0;
    bool failed = false;
    fz_var(doc);
    fz_var(pages);
    fz_try(im.ctx)
    {
        fz_register_document_handlers(im.ctx);
        doc = fz_open_document(im.ctx, file.c_str());
        if (!fz_needs_password(im.ctx, doc))
            pages = fz_count_pages(im.ctx, doc);
    }
    fz_catch(im.ctx)
    {
        failed = true;
    }
    if (failed) {
        if (doc)
            fz_drop_document(im.ctx, doc);
        im.fail("cannot open " + file);
    }
    im.doc = doc;
    im.pdf = pdf_specifics(im.ctx, doc);
    d->pageCount_ = pages;
    return d;
}

Document::~Document() = default;

bool Document::isPdf() const { return d_->pdf != nullptr; }

bool Document::needsPassword() const { return fz_needs_password(d_->ctx, d_->doc) && pageCount_ == 0; }

bool Document::authenticate(const std::string& password)
{
    if (!fz_authenticate_password(d_->ctx, d_->doc, password.c_str()))
        return false;
    int pages = 0;
    fz_var(pages);
    fz_try(d_->ctx) { pages = fz_count_pages(d_->ctx, d_->doc); }
    fz_catch(d_->ctx) { pages = 0; }
    pageCount_ = pages;
    return true;
}

void* Document::loadPage(int n)
{
    auto& im = *d_;
    if (n < 0 || n >= pageCount_)
        throw Error("page out of range");
    auto it = im.pages.find(n);
    if (it != im.pages.end()) {
        im.lru.remove(n);
        im.lru.push_front(n);
        return it->second;
    }
    fz_page* page = nullptr;
    bool failed = false;
    fz_var(page);
    fz_try(im.ctx) { page = fz_load_page(im.ctx, im.doc, n); }
    fz_catch(im.ctx) { failed = true; }
    if (failed)
        im.fail("cannot load page " + std::to_string(n + 1));
    im.pages[n] = page;
    im.lru.push_front(n);
    while (im.lru.size() > kPageCache) {
        int old = im.lru.back();
        im.lru.pop_back();
        fz_drop_page(im.ctx, im.pages[old]);
        im.pages.erase(old);
    }
    return page;
}

void Document::releaseMemory()
{
    auto& im = *d_;
    for (auto& [n, p] : im.pages)
        fz_drop_page(im.ctx, p);
    im.pages.clear();
    im.lru.clear();
    im.texts.clear();
    fz_empty_store(im.ctx);
}

Size Document::pageSize(int n)
{
    auto* page = (fz_page*)loadPage(n);
    fz_rect r = fz_empty_rect;
    fz_try(d_->ctx) { r = fz_bound_page(d_->ctx, page); }
    fz_catch(d_->ctx) { r = fz_make_rect(0, 0, 612, 792); }
    return {r.x1 - r.x0, r.y1 - r.y0};
}

std::string Document::pageLabel(int n)
{
    auto* page = (fz_page*)loadPage(n);
    char buf[64] = {0};
    fz_try(d_->ctx) { fz_page_label(d_->ctx, page, buf, sizeof buf); }
    fz_catch(d_->ctx) { buf[0] = 0; }
    return buf[0] ? std::string(buf) : std::to_string(n + 1);
}

std::string Document::metadata(const std::string& key)
{
    char buf[1024] = {0};
    int n = -1;
    fz_var(n);
    fz_try(d_->ctx) { n = fz_lookup_metadata(d_->ctx, d_->doc, key.c_str(), buf, sizeof buf); }
    fz_catch(d_->ctx) { n = -1; }
    return n > 0 ? std::string(buf) : std::string();
}

static Image toImage(fz_context* ctx, fz_pixmap* pix)
{
    Image img;
    img.width = fz_pixmap_width(ctx, pix);
    img.height = fz_pixmap_height(ctx, pix);
    img.stride = (int)fz_pixmap_stride(ctx, pix);
    auto* samples = fz_pixmap_samples(ctx, pix);
    img.pixels.assign(samples, samples + (size_t)img.stride * img.height);
    return img;
}

Image Document::renderClip(int n, float scale, const Rect& clip)
{
    auto* page = (fz_page*)loadPage(n);
    auto ctx = d_->ctx;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    bool failed = false;
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx)
    {
        fz_matrix ctm = fz_scale(scale, scale);
        fz_rect area = clip.isEmpty() ? fz_bound_page(ctx, page) : toFz(clip);
        fz_irect bbox = fz_round_rect(fz_transform_rect(area, ctm));
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pix, 0xff);
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_run_page_contents(ctx, page, dev, ctm, nullptr);
        fz_run_page_widgets(ctx, page, dev, ctm, nullptr);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) { fz_drop_device(ctx, dev); }
    fz_catch(ctx) { failed = true; }
    if (failed) {
        fz_drop_pixmap(ctx, pix);
        d_->fail("cannot render page " + std::to_string(n + 1));
    }
    auto img = toImage(ctx, pix);
    fz_drop_pixmap(ctx, pix);
    return img;
}

Image Document::render(int n, float scale) { return renderClip(n, scale, Rect{}); }

namespace {

// A device that records where images are drawn, cut to the clipping in effect.
// fz_new_derived_device() hands out raw zeroed memory, so the containers live outside.
struct ImageBoxDevice {
    fz_device super;
    std::vector<fz_rect>* boxes;
    std::vector<fz_rect>* clips;  // each entry: the intersection of all clips so far
    int tiles;                    // inside a pattern
    int masking;                  // drawing a soft mask, not the page
};

ImageBoxDevice* ib(fz_device* dev) { return reinterpret_cast<ImageBoxDevice*>(dev); }
fz_rect currentClip(ImageBoxDevice* d) { return d->clips->empty() ? fz_infinite_rect : d->clips->back(); }
void pushClip(fz_device* dev, fz_rect r) { ib(dev)->clips->push_back(fz_intersect_rect(r, currentClip(ib(dev)))); }

void ibClipPath(fz_context* ctx, fz_device* dev, const fz_path* path, int, fz_matrix ctm, fz_rect)
{
    pushClip(dev, fz_bound_path(ctx, path, nullptr, ctm));
}
void ibClipStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                      fz_matrix ctm, fz_rect)
{
    pushClip(dev, fz_bound_path(ctx, path, stroke, ctm));
}
void ibClipText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_rect)
{
    pushClip(dev, fz_bound_text(ctx, text, nullptr, ctm));
}
void ibClipStrokeText(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                      fz_matrix ctm, fz_rect)
{
    pushClip(dev, fz_bound_text(ctx, text, stroke, ctm));
}
void ibClipImageMask(fz_context*, fz_device* dev, fz_image*, fz_matrix ctm, fz_rect)
{
    pushClip(dev, fz_transform_rect(fz_unit_rect, ctm));
}
void ibPopClip(fz_context*, fz_device* dev)
{
    if (!ib(dev)->clips->empty())
        ib(dev)->clips->pop_back();
}
void ibBeginMask(fz_context*, fz_device* dev, fz_rect area, int, fz_colorspace*, const float*, fz_color_params)
{
    pushClip(dev, area);  // popped by the pop_clip that ends the masked content
    ++ib(dev)->masking;
}
void ibEndMask(fz_context*, fz_device* dev, fz_function*) { --ib(dev)->masking; }
int ibBeginTile(fz_context*, fz_device* dev, fz_rect, fz_rect, float, float, fz_matrix, int, int)
{
    ++ib(dev)->tiles;
    return 0;
}
void ibEndTile(fz_context*, fz_device* dev) { --ib(dev)->tiles; }
void ibFillImage(fz_context*, fz_device* dev, fz_image*, fz_matrix ctm, float alpha, fz_color_params)
{
    auto* d = ib(dev);
    if (d->tiles > 0 || d->masking > 0 || alpha < 0.5f)
        return;
    fz_rect r = fz_intersect_rect(fz_transform_rect(fz_unit_rect, ctm), currentClip(d));
    if (!fz_is_empty_rect(r))
        d->boxes->push_back(r);
}

} // namespace

std::vector<Rect> Document::imageRects(int n)
{
    auto* page = (fz_page*)loadPage(n);
    auto ctx = d_->ctx;
    std::vector<fz_rect> boxes, clips;
    fz_device* dev = nullptr;
    bool failed = false;
    fz_rect bound = fz_empty_rect;
    fz_var(dev);
    fz_try(ctx)
    {
        bound = fz_bound_page(ctx, page);
        auto* d = fz_new_derived_device(ctx, ImageBoxDevice);
        dev = &d->super;
        d->boxes = &boxes;
        d->clips = &clips;
        dev->clip_path = ibClipPath;
        dev->clip_stroke_path = ibClipStrokePath;
        dev->clip_text = ibClipText;
        dev->clip_stroke_text = ibClipStrokeText;
        dev->clip_image_mask = ibClipImageMask;
        dev->pop_clip = ibPopClip;
        dev->begin_mask = ibBeginMask;
        dev->end_mask = ibEndMask;
        dev->begin_tile = ibBeginTile;
        dev->end_tile = ibEndTile;
        dev->fill_image = ibFillImage;
        fz_run_page_contents(ctx, page, dev, fz_identity, nullptr);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) { fz_drop_device(ctx, dev); }
    fz_catch(ctx) { failed = true; }
    std::vector<Rect> out;
    if (failed)
        return out;
    for (auto& b : boxes) {
        fz_rect r = fz_intersect_rect(b, bound);
        if (!fz_is_empty_rect(r))
            out.push_back({r.x0 - bound.x0, r.y0 - bound.y0, r.x1 - bound.x0, r.y1 - bound.y0});
    }
    return out;
}

const PageText& Document::text(int n)
{
    auto it = d_->texts.find(n);
    if (it != d_->texts.end())
        return it->second;
    auto* page = (fz_page*)loadPage(n);
    auto ctx = d_->ctx;
    fz_stext_page* st = nullptr;
    fz_var(st);
    fz_try(ctx)
    {
        fz_stext_options opts;
        fz_init_stext_options(ctx, &opts);
        opts.flags = FZ_STEXT_CLIP;
        st = fz_new_stext_page_from_page(ctx, page, &opts);
    }
    fz_catch(ctx) { st = nullptr; }
    PageText pt;
    if (st) {
        for (fz_stext_block* b = st->first_block; b; b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT)
                continue;
            for (fz_stext_line* l = b->u.t.first_line; l; l = l->next) {
                TextLine line;
                line.box = toRect(l->bbox);
                line.first = (int)pt.chars.size();
                line.vertical = l->wmode != 0;
                int lineIndex = (int)pt.lines.size();
                for (fz_stext_char* c = l->first_char; c; c = c->next)
                    pt.chars.push_back({foldMathChar((uint32_t)c->c), toRect(fz_rect_from_quad(c->quad)), lineIndex});
                line.count = (int)pt.chars.size() - line.first;
                if (line.count > 0)
                    pt.lines.push_back(line);
            }
        }
        fz_drop_stext_page(ctx, st);
    }
    return d_->texts.emplace(n, std::move(pt)).first->second;
}

std::string Document::pageText(int n) { return text(n).utf8(); }

std::string Document::fullText(int maxPages)
{
    std::string out;
    int count = maxPages > 0 ? std::min(maxPages, pageCount_) : pageCount_;
    for (int i = 0; i < count; ++i) {
        try {
            out += pageText(i);
            out += "\n\f\n";
        } catch (const std::exception&) {
        }
        d_->texts.erase(i);  // indexing reads every page once; don't keep it all
    }
    return out;
}

std::vector<std::vector<Rect>> Document::search(int n, const std::string& needle, int maxHits, bool caseSensitive)
{
    std::vector<std::vector<Rect>> hits;
    if (needle.empty())
        return hits;
    auto* page = (fz_page*)loadPage(n);
    std::vector<fz_quad> quads((size_t)maxHits);
    std::vector<int> marks((size_t)maxHits);
    int count = 0;
    fz_var(count);
    fz_try(d_->ctx)
    {
        count = caseSensitive ? fz_match_page(d_->ctx, page, needle.c_str(), marks.data(), quads.data(), maxHits,
                                              FZ_SEARCH_EXACT)
                              : fz_search_page(d_->ctx, page, needle.c_str(), marks.data(), quads.data(), maxHits);
    }
    fz_catch(d_->ctx) { count = 0; }
    for (int i = 0; i < count; ++i) {
        if (marks[i] || hits.empty())
            hits.emplace_back();
        hits.back().push_back(toRect(fz_rect_from_quad(quads[i])));
    }
    return hits;
}

static int pageNumber(fz_context* ctx, fz_document* doc, fz_location loc)
{
    int page = -1;
    fz_var(page);
    fz_try(ctx) { page = loc.page >= 0 ? fz_page_number_from_location(ctx, doc, loc) : -1; }
    fz_catch(ctx) { page = -1; }
    return page;
}

static void convertOutline(fz_context* ctx, fz_document* doc, fz_outline* o, std::vector<OutlineEntry>& out)
{
    for (; o; o = o->next) {
        OutlineEntry e;
        e.title = o->title ? o->title : "";
        e.uri = o->uri ? o->uri : "";
        e.y = o->y;
        e.page = pageNumber(ctx, doc, o->page);
        convertOutline(ctx, doc, o->down, e.children);
        out.push_back(std::move(e));
    }
}

std::vector<OutlineEntry> Document::outline()
{
    std::vector<OutlineEntry> out;
    fz_outline* o = nullptr;
    fz_var(o);
    fz_try(d_->ctx) { o = fz_load_outline(d_->ctx, d_->doc); }
    fz_catch(d_->ctx) { o = nullptr; }
    if (o) {
        convertOutline(d_->ctx, d_->doc, o, out);
        fz_drop_outline(d_->ctx, o);
    }
    return out;
}

static int resolveLink(fz_context* ctx, fz_document* doc, const char* uri, float* y)
{
    float x = 0;
    fz_location loc = fz_make_location(-1, -1);
    fz_var(loc);
    fz_try(ctx) { loc = fz_resolve_link(ctx, doc, uri, &x, y); }
    fz_catch(ctx) { return -1; }
    return pageNumber(ctx, doc, loc);
}

std::vector<Link> Document::links(int n)
{
    std::vector<Link> out;
    auto* page = (fz_page*)loadPage(n);
    auto ctx = d_->ctx;
    fz_link* links = nullptr;
    fz_var(links);
    fz_try(ctx) { links = fz_load_links(ctx, page); }
    fz_catch(ctx) { links = nullptr; }
    for (fz_link* l = links; l; l = l->next) {
        Link link;
        link.rect = toRect(l->rect);
        link.uri = l->uri ? l->uri : "";
        if (l->uri && !fz_is_external_link(ctx, l->uri))
            link.page = resolveLink(ctx, d_->doc, l->uri, &link.y);
        out.push_back(std::move(link));
    }
    fz_drop_link(ctx, links);
    return out;
}

// --- coordinates ---------------------------------------------------------------------------

static fz_matrix pageMatrix(Document::Impl& im, fz_page* page, int n)
{
    auto it = im.pdfToPage.find(n);
    if (it != im.pdfToPage.end())
        return it->second;
    fz_matrix ctm = fz_identity;
    if (im.pdf) {
        fz_try(im.ctx)
        {
            pdf_page* pp = pdf_page_from_fz_page(im.ctx, page);
            if (pp)
                pdf_page_transform(im.ctx, pp, nullptr, &ctm);
        }
        fz_catch(im.ctx) { ctm = fz_identity; }
    }
    im.pdfToPage[n] = ctm;
    return ctm;
}

Point Document::pdfToPage(int n, Point p)
{
    auto m = pageMatrix(*d_, (fz_page*)loadPage(n), n);
    auto r = fz_transform_point(fz_make_point(p.x, p.y), m);
    return {r.x, r.y};
}

Point Document::pageToPdf(int n, Point p)
{
    auto m = fz_invert_matrix(pageMatrix(*d_, (fz_page*)loadPage(n), n));
    auto r = fz_transform_point(fz_make_point(p.x, p.y), m);
    return {r.x, r.y};
}

Rect Document::pdfToPage(int n, const Rect& r)
{
    auto m = pageMatrix(*d_, (fz_page*)loadPage(n), n);
    return toRect(fz_transform_rect(toFz(r.normalized()), m));
}

Rect Document::pageToPdf(int n, const Rect& r)
{
    auto m = fz_invert_matrix(pageMatrix(*d_, (fz_page*)loadPage(n), n));
    return toRect(fz_transform_rect(toFz(r.normalized()), m));
}

// --- annotations in the file ------------------------------------------------------------------

namespace {

struct AnnotRead {
    enum pdf_annot_type type = PDF_ANNOT_UNKNOWN;
    std::string comment;
    std::string color = "#ffd400";
    std::vector<Rect> rects;                     // page space
    std::vector<std::vector<fz_point>> strokes;  // page space
};

bool readAnnot(fz_context* ctx, pdf_annot* a, AnnotRead* r)
{
    bool ok = true;
    fz_var(ok);
    fz_try(ctx)
    {
        r->type = pdf_annot_type(ctx, a);
        const char* contents = pdf_annot_contents(ctx, a);
        if (contents)
            r->comment = contents;
        int cn = 0;
        float c[4] = {0, 0, 0, 0};
        pdf_annot_color(ctx, a, &cn, c);
        if (cn)
            r->color = colorHex(cn, c);
        if (r->type == PDF_ANNOT_HIGHLIGHT || r->type == PDF_ANNOT_UNDERLINE) {
            int qn = pdf_annot_quad_point_count(ctx, a);
            for (int i = 0; i < qn; ++i)
                r->rects.push_back(toRect(fz_rect_from_quad(pdf_annot_quad_point(ctx, a, i))));
        }
        if (r->rects.empty())
            r->rects.push_back(toRect(pdf_annot_has_rect(ctx, a) ? pdf_annot_rect(ctx, a) : pdf_bound_annot(ctx, a)));
        if (r->type == PDF_ANNOT_INK) {
            int strokes = pdf_annot_ink_list_count(ctx, a);
            for (int i = 0; i < strokes; ++i) {
                r->strokes.emplace_back();
                int vn = pdf_annot_ink_list_stroke_count(ctx, a, i);
                for (int v = 0; v < vn; ++v)
                    r->strokes.back().push_back(pdf_annot_ink_list_stroke_vertex(ctx, a, i, v));
            }
        }
    }
    fz_catch(ctx) { ok = false; }
    return ok;
}

struct AnnotWrite {
    std::string type;
    std::vector<fz_rect> rects;                  // page space
    std::vector<std::vector<fz_point>> strokes;  // page space
    float rgb[3] = {1, 0.83f, 0};
    std::string comment;
    std::string author;
    float width = 2;
    float fontSize = 12;
};

// Returns an error message, empty on success.
std::string writeAnnot(fz_context* ctx, pdf_page* pp, const AnnotWrite* w)
{
    pdf_annot* a = nullptr;
    std::string error;
    fz_var(a);
    fz_try(ctx)
    {
        if (w->type == "highlight" || w->type == "underline") {
            a = pdf_create_annot(ctx, pp, w->type == "highlight" ? PDF_ANNOT_HIGHLIGHT : PDF_ANNOT_UNDERLINE);
            for (auto& r : w->rects)
                pdf_add_annot_quad_point(ctx, a, fz_quad_from_rect(r));
        } else if (w->type == "note") {
            a = pdf_create_annot(ctx, pp, PDF_ANNOT_TEXT);
        } else if (w->type == "text") {
            a = pdf_create_annot(ctx, pp, PDF_ANNOT_FREE_TEXT);
            pdf_set_annot_default_appearance(ctx, a, "Helv", w->fontSize, 3, w->rgb);
        } else if (w->type == "image") {
            a = pdf_create_annot(ctx, pp, PDF_ANNOT_SQUARE);
            pdf_set_annot_border_width(ctx, a, 2);
        } else if (w->type == "ink") {
            a = pdf_create_annot(ctx, pp, PDF_ANNOT_INK);
            for (auto& pts : w->strokes)
                pdf_add_annot_ink_list(ctx, a, (int)pts.size(), const_cast<fz_point*>(pts.data()));
            pdf_set_annot_border_width(ctx, a, w->width);
        }
        if (a) {
            if ((w->type == "note" || w->type == "text" || w->type == "image") && !w->rects.empty())
                pdf_set_annot_rect(ctx, a, w->rects.front());
            if (w->type != "text")
                pdf_set_annot_color(ctx, a, 3, w->rgb);
            if (!w->comment.empty() || w->type == "text")
                pdf_set_annot_contents(ctx, a, w->comment.c_str());
            if (!w->author.empty())
                pdf_set_annot_author(ctx, a, w->author.c_str());
            pdf_update_annot(ctx, a);
        }
    }
    fz_always(ctx) { pdf_drop_annot(ctx, a); }
    fz_catch(ctx) { error = fz_caught_message(ctx); }
    return error;
}

} // namespace

std::vector<Json> Document::embeddedAnnotations()
{
    std::vector<Json> out;
    if (!d_->pdf)
        return out;
    auto ctx = d_->ctx;
    auto r3 = [](float v) { return std::round(v * 1000) / 1000; };
    for (int n = 0; n < pageCount_; ++n) {
        fz_page* page = nullptr;
        try {
            page = (fz_page*)loadPage(n);
        } catch (const std::exception&) {
            continue;
        }
        pdf_page* pp = pdf_page_from_fz_page(ctx, page);
        if (!pp)
            continue;
        std::vector<AnnotRead> found;
        for (pdf_annot* a = pdf_first_annot(ctx, pp); a; a = pdf_next_annot(ctx, a)) {
            AnnotRead r;
            if (readAnnot(ctx, a, &r))
                found.push_back(std::move(r));
        }
        for (auto& r : found) {
            const char* kind = r.type == PDF_ANNOT_HIGHLIGHT   ? "highlight"
                               : r.type == PDF_ANNOT_UNDERLINE ? "underline"
                               : r.type == PDF_ANNOT_TEXT      ? "note"
                               : r.type == PDF_ANNOT_FREE_TEXT ? "text"
                               : r.type == PDF_ANNOT_INK       ? "ink"
                               : r.type == PDF_ANNOT_SQUARE    ? "image"
                                                               : nullptr;
            if (!kind || r.rects.empty())
                continue;
            Json pos{{"pageIndex", n}};
            std::string text;
            if (r.type == PDF_ANNOT_HIGHLIGHT || r.type == PDF_ANNOT_UNDERLINE) {
                auto& pt = this->text(n);
                for (auto& rect : r.rects) {
                    int first = -1, last = -1;
                    for (int i = 0; i < (int)pt.chars.size(); ++i) {
                        auto& b = pt.chars[i].box;
                        if (rect.contains({(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2})) {
                            if (first < 0)
                                first = i;
                            last = i;
                        }
                    }
                    if (first >= 0)
                        text += (text.empty() ? "" : " ") + pt.textOf(first, last);
                }
            }
            Json rects = Json::array();
            for (auto& rect : r.rects) {
                auto pr = pageToPdf(n, rect);
                rects.push_back({r3(pr.x0), r3(pr.y0), r3(pr.x1), r3(pr.y1)});
            }
            pos["rects"] = rects;
            if (!r.strokes.empty()) {
                Json paths = Json::array();
                for (auto& stroke : r.strokes) {
                    Json path = Json::array();
                    for (auto& p : stroke) {
                        auto q = pageToPdf(n, Point{p.x, p.y});
                        path.push_back(r3(q.x));
                        path.push_back(r3(q.y));
                    }
                    paths.push_back(path);
                }
                pos["paths"] = paths;
                pos["width"] = 2;
            }
            out.push_back(Json{{"itemType", "annotation"},
                               {"annotationType", kind},
                               {"annotationText", text},
                               {"annotationComment", r.comment},
                               {"annotationColor", r.color},
                               {"annotationPageLabel", pageLabel(n)},
                               {"annotationSortIndex", sortIndex(n, 0, r.rects.front().y0)},
                               {"annotationPosition", pos},
                               {"annotationIsExternal", true}});
        }
    }
    return out;
}

void Document::writeAnnotations(const std::vector<Json>& annotations, bool external)
{
    auto ctx = d_->ctx;
    for (auto& ann : annotations) {
        if (jbool(ann, "annotationIsExternal") && !external)
            continue;
        auto pos = ann.value("annotationPosition", Json::object());
        int n = (int)jint(pos, "pageIndex", -1);
        if (n < 0 || n >= pageCount_)
            continue;
        pdf_page* pp = pdf_page_from_fz_page(ctx, (fz_page*)loadPage(n));
        if (!pp)
            continue;
        AnnotWrite w;
        w.type = jstr(ann, "annotationType");
        for (auto& r : pos.value("rects", Json::array()))
            if (r.is_array() && r.size() == 4 && r[0].is_number() && r[1].is_number() && r[2].is_number() &&
                r[3].is_number())
                w.rects.push_back(
                    toFz(pdfToPage(n, Rect{r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()})));
        for (auto& path : pos.value("paths", Json::array())) {
            std::vector<fz_point> pts;
            for (size_t i = 0; i + 1 < path.size(); i += 2) {
                if (!path[i].is_number() || !path[i + 1].is_number())
                    continue;
                auto p = pdfToPage(n, Point{path[i].get<float>(), path[i + 1].get<float>()});
                pts.push_back(fz_make_point(p.x, p.y));
            }
            if (!pts.empty())
                w.strokes.push_back(std::move(pts));
        }
        parseColor(jstr(ann, "annotationColor", "#ffd400"), w.rgb);
        w.comment = jstr(ann, "annotationComment");
        w.author = jstr(ann, "annotationAuthorName");
        w.width = (float)pos.value("width", 2.0);
        w.fontSize = (float)pos.value("fontSize", 12.0);
        writeAnnot(ctx, pp, &w);
    }
}

void Document::exportWithAnnotations(const fs::path& out, const std::vector<Json>& annotations)
{
    if (!d_->pdf)
        throw Error("only PDF files can carry annotations");
    // Work on a separate instance so the open document stays untouched.
    auto copy = Document::open(path_);
    auto& im = *copy->d_;
    auto ctx = im.ctx;
    copy->writeAnnotations(annotations, false);
    std::string file = pathString(out);
    bool failed = false;
    fz_var(failed);
    fz_try(ctx)
    {
        pdf_write_options opts = pdf_default_write_options;
        opts.do_garbage = 1;
        opts.do_compress = 1;
        pdf_save_document(ctx, im.pdf, file.c_str(), &opts);
    }
    fz_catch(ctx) { failed = true; }
    if (failed)
        im.fail("cannot save " + file);
}

void Document::renderPng(int n, float scale, const Rect& clip, const std::vector<Json>& annotations, bool withPage,
                         const fs::path& out)
{
    if (n < 0 || n >= pageCount_)
        throw Error("no page " + std::to_string(n + 1));
    // A separate instance gets the annotations, so the open document stays untouched.
    auto copy = Document::open(path_);
    auto& im = *copy->d_;
    auto ctx = im.ctx;
    if (im.pdf) {
        if (!withPage) {  // these annotations alone: none of the file's own
            pdf_page* pp = pdf_page_from_fz_page(ctx, (fz_page*)copy->loadPage(n));
            fz_try(ctx)
            {
                while (pdf_annot* a = pp ? pdf_first_annot(ctx, pp) : nullptr)
                    pdf_delete_annot(ctx, pp, a);
            }
            fz_catch(ctx) {}
        }
        copy->writeAnnotations(annotations, !withPage);
    }
    auto* page = (fz_page*)copy->loadPage(n);
    std::string file = pathString(out);
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    bool failed = false;
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx)
    {
        fz_matrix ctm = fz_scale(scale, scale);
        fz_rect area = clip.isEmpty() ? fz_bound_page(ctx, page) : toFz(clip);
        fz_irect bbox = fz_round_rect(fz_transform_rect(area, ctm));
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, nullptr, withPage ? 0 : 1);
        if (withPage)
            fz_clear_pixmap_with_value(ctx, pix, 0xff);
        else
            fz_clear_pixmap(ctx, pix);  // clear: the annotations on nothing
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        if (withPage) {
            fz_run_page_contents(ctx, page, dev, ctm, nullptr);
            fz_run_page_widgets(ctx, page, dev, ctm, nullptr);
        }
        fz_run_page_annots(ctx, page, dev, ctm, nullptr);
        fz_close_device(ctx, dev);
        fz_save_pixmap_as_png(ctx, pix, file.c_str());
    }
    fz_always(ctx)
    {
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) { failed = true; }
    if (failed)
        im.fail("cannot draw page " + std::to_string(n + 1) + " into " + file);
}

} // namespace on::pdf
