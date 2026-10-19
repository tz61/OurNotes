#pragma once

#include <functional>
#include <optional>
#include <string>

#include "core/json.h"
#include "core/util.h"

namespace fp::formats {

struct PdfMetadataMatch {
    Json item;           // Zotero-form item data (may carry "_notes")
    std::string source;  // how it was found, e.g. "DOI 10.1038/x · Crossref"
};

// Find bibliographic metadata for a PDF. Text is extracted locally; only the
// identifier found in it (DOI, arXiv, ISBN, PMID), or else the guessed title, is
// sent to the matching public service (Crossref, arXiv, Open Library, PubMed).
// `stage` reports progress ("Reading PDF", "Looking up DOI …", ...).
// Returns nullopt when nothing reliable was found; throws MetadataError on network errors
// during the title search.
std::optional<PdfMetadataMatch> retrieveMetadataForPdf(const fs::path& path,
                                                       const std::function<void(const std::string&)>& stage = {});

// The title guess used above (exposed for tests).
std::string guessPdfTitle(const fs::path& path);

} // namespace fp::formats
