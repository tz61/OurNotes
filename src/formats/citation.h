#pragma once

#include <string>
#include <vector>

#include "core/json.h"

// Built-in citation styles implemented directly in C++ (no CSL engine).
namespace fp::formats {

struct StyleInfo {
    std::string id;
    std::string title;
    bool numeric = false;
};

enum class OutputFormat { Text, Html };

// "apa", "chicago-author-date", "ieee", "mla", "harvard", "nature", "vancouver"
const std::vector<StyleInfo>& builtinStyles();

// Author-date styles are sorted by first author, year and title; numeric styles keep the
// given order and are numbered. Html wraps each entry in <div class="csl-entry"> inside
// <div class="csl-bib-body">; Text puts one entry per line. Throws fp::Error for unknown
// styles. Notes and attachments are skipped.
std::string formatBibliography(const std::vector<Json>& items, const std::string& styleId,
                               OutputFormat fmt = OutputFormat::Text);

// In-text citation for the given items, e.g. "(Smith & Jones, 2020; Lee et al., 2021)" in
// APA or "[1], [2]" in IEEE (numbers follow the order of `items`).
std::string formatCitation(const std::vector<Json>& items, const std::string& styleId,
                           OutputFormat fmt = OutputFormat::Text);

} // namespace fp::formats
