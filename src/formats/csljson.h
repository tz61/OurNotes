#pragma once

#include <string>
#include <vector>

#include "core/json.h"

namespace fp::formats {

// Zotero item data <-> CSL-JSON, using the type/field/creator mappings from the schema.
// toCsl: "id" is the item key (or citationKey, or a generated key); dates become
// {"date-parts":[[y,m,d]]} ({"raw": ...} when unparseable); tags become "keyword".
Json toCsl(const Json& item);
Json fromCsl(const Json& csl);

std::string exportCslJson(const std::vector<Json>& items);  // pretty-printed array
std::vector<Json> importCslJson(const std::string& text);   // array or single object; throws fp::Error

} // namespace fp::formats
