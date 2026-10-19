#include "formats/pdf_metadata.h"

#include <cmath>

#include "formats/metadata.h"
#include "pdf/document.h"

namespace fp::formats {

namespace {

std::string titleFromDocument(pdf::Document& doc)
{
    auto meta = trim(doc.metadata("info:Title"));
    bool plausible = meta.size() > 12 && meta.find(".dvi") == std::string::npos &&
                     meta.find(".doc") == std::string::npos && meta.find("Microsoft Word") == std::string::npos;
    if (plausible)
        return meta;
    if (doc.pageCount() == 0)
        return {};
    // the tallest lines in the top half of the first page
    auto& text = doc.text(0);
    auto size = doc.pageSize(0);
    float best = 0;
    for (auto& l : text.lines)
        if (l.box.y0 < size.height * 0.5f && !l.vertical && l.count > 3)
            best = std::max(best, l.box.height());
    std::string title;
    for (auto& l : text.lines) {
        if (l.vertical || l.box.y0 >= size.height * 0.5f || std::abs(l.box.height() - best) > 0.6f)
            continue;
        if (!title.empty())
            title += ' ';
        title += text.textOf(l.first, l.first + l.count - 1);
    }
    return trim(title);
}

} // namespace

std::string guessPdfTitle(const fs::path& path)
{
    auto doc = pdf::Document::open(path);
    return titleFromDocument(*doc);
}

std::optional<Json> retrieveMetadataForPdf(const fs::path& path)
{
    auto doc = pdf::Document::open(path);
    std::string text;
    for (int i = 0; i < std::min(2, doc->pageCount()); ++i)
        text += doc->pageText(i) + "\n";
    text += doc->metadata("info:Subject") + "\n" + doc->metadata("info:Keywords");
    for (auto& id : findIdentifiersInText(text)) {
        try {
            return lookup(id);
        } catch (const MetadataError&) {
        }
    }
    auto title = titleFromDocument(*doc);
    if (title.size() > 10)
        return lookupByTitle(title);
    return std::nullopt;
}

} // namespace fp::formats
