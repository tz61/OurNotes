#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace fp::formats {

// BibTeX export with Better-BibTeX-like citation keys (lastnameYearFirstword, or the
// item's citationKey field / "Citation Key:" line in Extra), deduplicated with a/b/c.
std::string exportBibtex(const std::vector<Json>& items);

// Tolerant BibTeX/BibLaTeX parser. Returns items in Zotero data form plus optional
// "_notes" (HTML) and "_files" (paths from JabRef/Zotero `file` fields).
std::vector<Json> importBibtex(const std::string& text);

// LaTeX markup (accents, escapes, dashes, formatting commands) to plain UTF-8.
std::string latexToUnicode(std::string_view latex);

} // namespace fp::formats
