#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/json.h"

namespace fp::formats {

struct Format {
    std::string id;         // "bibtex", "ris", "csljson"
    std::string label;      // "BibTeX"
    std::string extension;  // ".bib"
    std::function<std::string(const std::vector<Json>&)> exporter;
    std::function<std::vector<Json>(const std::string&)> importer;
};

const std::vector<Format>& allFormats();
const Format* findFormat(const std::string& id);

// Guesses the format of import data: "bibtex", "ris", "csljson" or "".
std::string detectFormat(const std::string& text);

} // namespace fp::formats
