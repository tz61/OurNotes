#include "formats/formats.h"

#include <cctype>

#include "core/util.h"
#include "formats/bibtex.h"
#include "formats/csljson.h"
#include "formats/ris.h"

namespace fp::formats {

const std::vector<Format>& allFormats()
{
    static const std::vector<Format> formats = {
        {"bibtex", "BibTeX", ".bib", exportBibtex, importBibtex},
        {"ris", "RIS", ".ris", exportRis, importRis},
        {"csljson", "CSL JSON", ".json", exportCslJson, importCslJson},
    };
    return formats;
}

const Format* findFormat(const std::string& id)
{
    for (auto& f : allFormats())
        if (f.id == id)
            return &f;
    return nullptr;
}

std::string detectFormat(const std::string& text)
{
    std::string_view s = text;
    if (startsWith(s, "\xEF\xBB\xBF"))
        s.remove_prefix(3);
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start]))
        ++start;
    if (start == s.size())
        return {};

    if (s[start] == '[' || s[start] == '{') {
        auto doc = Json::parse(s.begin() + (long)start, s.end(), nullptr, false);
        if (doc.is_object() && doc.contains("items") && doc["items"].is_array())
            doc = doc["items"];
        if (!doc.is_discarded()) {
            const Json& first = doc.is_array() && !doc.empty() ? doc[0] : doc;
            if (first.is_object() && (first.contains("type") || first.contains("title") || first.contains("id")))
                return "csljson";
            if (doc.is_array() && doc.empty())
                return "csljson";
        }
    }

    // RIS: a "TY  - " line among the first lines
    size_t pos = start;
    for (int line = 0; line < 50 && pos < s.size(); ++line) {
        size_t end = s.find('\n', pos);
        auto l = s.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        if (startsWith(l, "TY  -") || startsWith(l, "TY -"))
            return "ris";
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }

    // BibTeX: "@type{" or "@type(" anywhere
    for (size_t at = s.find('@'); at != std::string_view::npos; at = s.find('@', at + 1)) {
        size_t i = at + 1;
        while (i < s.size() && std::isalpha((unsigned char)s[i]))
            ++i;
        if (i == at + 1)
            continue;
        while (i < s.size() && std::isspace((unsigned char)s[i]))
            ++i;
        if (i < s.size() && (s[i] == '{' || s[i] == '('))
            return "bibtex";
    }
    return {};
}

} // namespace fp::formats
