#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/util.h"

// Online metadata lookup by identifier (Crossref, arXiv, Open Library, PubMed).
// All calls are synchronous with 15 s network timeouts; call them from a worker thread.
namespace fp::formats {

class MetadataError : public fp::Error {
public:
    using fp::Error::Error;
};

struct Identifier {
    std::string kind;   // "doi", "arxiv", "isbn", "pmid"
    std::string value;  // DOI as written, arXiv id without version, ISBN digits, PMID digits

    bool operator==(const Identifier& o) const { return kind == o.kind && value == o.value; }
};

// Identifiers typed or pasted by the user, separated by whitespace, commas or newlines:
// DOIs (bare, "doi:" prefix or doi.org URL), arXiv ids (2301.12345v2, hep-th/9901001,
// "arXiv:" prefix, arxiv.org/abs|pdf URLs), ISBN-10/13 (checksum validated) and PMIDs
// (only with an explicit "PMID:" prefix). Deduplicated, in input order.
std::vector<Identifier> parseIdentifiers(const std::string& text);

// Identifiers found in running text such as a PDF's first page: DOIs, arXiv stamps
// ("arXiv:2301.08243v3 [cs.CV] 25 Jan 2023"), "ISBN ..." and "PMID: ...".
std::vector<Identifier> findIdentifiersInText(const std::string& text);

// Fetches an item (Zotero data form; "_notes" may hold HTML child notes).
// Throws MetadataError when nothing is found or the network fails.
Json lookup(const Identifier& id);

// Crossref bibliographic search (top 3 hits); returns the best match only if its normalised
// title is at least 90% similar to `title`. An author name (e.g. the first author's last
// name) helps a lot for short, generic titles. Throws MetadataError on network failure.
std::optional<Json> lookupByTitle(const std::string& title, const std::string& authorHint = {});

// Levenshtein ratio (0..1) of the normalised (lower-case, accent-folded, punctuation-free) strings.
double titleSimilarity(const std::string& a, const std::string& b);

std::string arxivPdfUrl(const std::string& arxivId);

// Streams `url` to `dest` (via dest + ".part", renamed on success). With requirePdf the
// body must start with the %PDF magic. Throws MetadataError.
void downloadFile(const std::string& url, const fs::path& dest, bool requirePdf = false);

} // namespace fp::formats
