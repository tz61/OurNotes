#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/json.h"

// Item type / field / creator-type schema, generated from Zotero (tools/gen_schema.py)
// and compiled into the binary.
namespace fp::schema {

struct Color {
    const char* name;
    const char* hex;
};
extern const std::vector<Color> kAnnotationColors;

extern const char* const kLinkModes[];  // imported_file, imported_url, linked_file, linked_url, embedded_image

bool isRegularType(const std::string& itemType);
bool isStoredLinkMode(const std::string& linkMode);  // file lives in storage/<key>/

std::vector<std::string> itemTypes(bool regularOnly = true);  // sorted by label
bool isValidType(const std::string& itemType);
std::vector<std::string> fieldsFor(const std::string& itemType);
bool hasField(const std::string& itemType, const std::string& field);
std::map<std::string, std::string> baseFieldMap(const std::string& itemType);  // specific -> base
std::string getField(const Json& data, const std::string& baseField);           // honours mapped fields
std::vector<std::string> creatorTypesFor(const std::string& itemType);
std::string primaryCreatorType(const std::string& itemType);

std::string typeLabel(const std::string& itemType);
std::string fieldLabel(const std::string& field);
std::string creatorLabel(const std::string& creatorType);

const Json& cslMappings();  // {"types":{csl:[zotero...]}, "fields":{...}, "names":{...}}

// Convert item data to another type, mapping base fields; returns fields that were dropped.
Json convertType(const Json& data, const std::string& newType, Json* dropped = nullptr);

} // namespace fp::schema
