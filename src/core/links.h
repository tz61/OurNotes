#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/json.h"

namespace on {

class Library;

// A link to something in a library, as notes have them (a click on it opens it):
//
//   ournotes://item/<library uid>/<key>[?page=N][&rect=X0,Y0,X1,Y1][&annotation=KEY]
//
// The key is a regular item's (shown in the library), a note's (opened), an attachment's (a
// PDF opened, at page N from 1, at the rect there: points from the page's top left) or an
// annotation's (its PDF opened at it). The library is named by its uid, the same on every
// computer it syncs to.
struct ItemLink {
    std::string library;  // uid
    std::string key;
    int page = 0;  // 0: none
    std::optional<std::array<double, 4>> rect;
    std::string annotation;  // an annotation's key, in the PDF `key` names
};

constexpr std::string_view kItemLinkPrefix = "ournotes://item/";

std::string itemLinkUrl(const ItemLink& link);
std::optional<ItemLink> parseItemLink(std::string_view url);  // nullopt: not one

// Where a link leads in `lib`: the library (by its uid; for one not on this computer, any that
// has the key) and the item there; nullopt when there is none.
struct LinkTarget {
    int64_t libraryId = 0;
    Json item;
};
std::optional<LinkTarget> linkTarget(Library& lib, const ItemLink& link);

// A link and the words a note shows for it: "(Smith and Jones, 2020, p. 3)" for an item or a
// page of its PDF, a note's title.
struct Reference {
    std::string url;
    std::string text;
};

// To `key` in a library: a regular item, a note, an attachment (at `page` from 1, called
// `pageLabel` when its PDF names its pages, else by its number; at `rect` on it) or an
// annotation (in its PDF, at its page). Throws on Error when there is no such item.
Reference referenceTo(Library& lib, int64_t libraryId, const std::string& key, int page = 0,
                      const std::string& pageLabel = {}, const std::optional<std::array<double, 4>>& rect = {});

// “quote” (Smith, 2020, p. 3), the reference a link; without a quote, the link alone.
std::string referenceHtml(const Reference& ref, const std::string& quote = {});
std::string referenceMarkdown(const Reference& ref, const std::string& quote = {});
std::string referenceText(const Reference& ref, const std::string& quote = {});

} // namespace on
