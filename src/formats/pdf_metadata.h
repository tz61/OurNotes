#pragma once

#include <optional>

#include "core/json.h"
#include "core/util.h"

namespace fp::formats {

// Find bibliographic metadata for a PDF: identifiers (DOI, arXiv, ISBN, PMID) on the
// first pages or in the document info are looked up; otherwise the title (document
// info, or the largest text at the top of page 1) is searched on Crossref.
// Returns item data (Zotero form), or nullopt when nothing reliable was found.
std::optional<Json> retrieveMetadataForPdf(const fs::path& path);

// The title guess used above (exposed for tests).
std::string guessPdfTitle(const fs::path& path);

} // namespace fp::formats
