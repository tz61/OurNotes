#pragma once

#include <string>
#include <vector>

// Translation by Google Translate (the free endpoint its web page uses, no key), for text
// chosen in a PDF. The calls that go online are synchronous with 15 s timeouts; call
// them from a worker thread.
namespace on::formats {

struct Translation {
    std::string text;      // the whole translation
    std::string source;    // the language of what was translated, as Google found it ("en", "zh-CN")
    std::string target;    // the language translated into
    std::string phonetic;  // how the original is said ("əˈten(t)SHən"); may be empty
    struct Meanings {
        std::string partOfSpeech;  // in the target language ("名词" for zh-CN)
        std::vector<std::string> terms;
    };
    std::vector<Meanings> dictionary;  // a word's other meanings, by part of speech; may be empty
};

struct Language {
    const char* code;  // Google's ("zh-CN", "iw" for Hebrew)
    const char* name;  // in the language itself
};

// The languages offered to translate into, the most read first.
const std::vector<Language>& translationLanguages();

// The name of a language for its code ("en" → "English"), or the code if it is not listed.
std::string languageName(const std::string& code);

// One language under two codes: case aside, "zh" and "zh-CN", "en" and "en-GB"; but not
// Chinese in its simplified and traditional writing.
bool sameLanguage(const std::string& a, const std::string& b);

// The code to translate into for a locale ("zh_CN", "zh-Hant-TW", "en-US", "he_IL").
std::string translationLanguageFor(const std::string& locale);

// The page of translate.google.com with the text (shortened if very long) and its translation.
std::string googleTranslatePage(const std::string& text, const std::string& target);

// An answer of the endpoint (JSON, "dj=1"). Throws MetadataError if there is no translation in it.
Translation parseGoogleTranslation(const std::string& body);

// The text translated into `target`, from whatever language it is in. Throws MetadataError
// (no network, Google busy or refusing).
Translation translate(const std::string& text, const std::string& target);

} // namespace on::formats
