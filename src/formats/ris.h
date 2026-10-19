#pragma once

#include <string>
#include <vector>

#include "core/json.h"

namespace fp::formats {

std::string exportRis(const std::vector<Json>& items);

// Returns items in Zotero data form plus optional "_notes" (N1, HTML) and "_files"
// (L1/L4 paths).
std::vector<Json> importRis(const std::string& text);

} // namespace fp::formats
