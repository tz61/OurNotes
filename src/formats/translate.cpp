#include "formats/translate.h"

#include "core/json.h"
#include "core/util.h"
#include "formats/http.h"
#include "formats/metadata.h"

namespace on::formats {

namespace {

constexpr time_t kTimeoutSeconds = 15;
constexpr const char* kService = "Google Translate";
constexpr size_t kMaxTerms = 8;
constexpr size_t kMaxPageText = 5000;  // (bytes, in the page's address)

std::string primary(const std::string& code) { return toLower(code.substr(0, code.find_first_of("-_"))); }

// At most `bytes` bytes of UTF-8 text, not cutting a character.
std::string utf8Prefix(const std::string& s, size_t bytes)
{
    if (s.size() <= bytes)
        return s;
    while (bytes > 0 && (static_cast<unsigned char>(s[bytes]) & 0xC0) == 0x80)
        --bytes;
    return s.substr(0, bytes);
}

} // namespace

const std::vector<Language>& translationLanguages()
{
    static const std::vector<Language> list = {
        {"zh-CN", "中文（简体）"},
        {"zh-TW", "中文（繁體）"},
        {"en", "English"},
        {"ja", "日本語"},
        {"ko", "한국어"},
        {"fr", "Français"},
        {"de", "Deutsch"},
        {"es", "Español"},
        {"it", "Italiano"},
        {"pt", "Português"},
        {"ru", "Русский"},
        {"uk", "Українська"},
        {"pl", "Polski"},
        {"nl", "Nederlands"},
        {"sv", "Svenska"},
        {"da", "Dansk"},
        {"no", "Norsk"},
        {"fi", "Suomi"},
        {"cs", "Čeština"},
        {"hu", "Magyar"},
        {"ro", "Română"},
        {"el", "Ελληνικά"},
        {"tr", "Türkçe"},
        {"ar", "العربية"},
        {"iw", "עברית"},
        {"fa", "فارسی"},
        {"hi", "हिन्दी"},
        {"bn", "বাংলা"},
        {"th", "ไทย"},
        {"vi", "Tiếng Việt"},
        {"id", "Bahasa Indonesia"},
        {"ms", "Bahasa Melayu"},
    };
    return list;
}

std::string languageName(const std::string& code)
{
    for (auto& l : translationLanguages())
        if (iequals(l.code, code))
            return l.name;
    if (primary(code) != "zh")
        for (auto& l : translationLanguages())
            if (primary(l.code) == primary(code))
                return l.name;
    return code;
}

bool sameLanguage(const std::string& a, const std::string& b)
{
    if (iequals(a, b))
        return true;
    if (a.empty() || b.empty() || primary(a) != primary(b))
        return false;
    if (primary(a) != "zh")
        return true;
    // Chinese: the same when one of them says no more ("zh")
    return a.find_first_of("-_") == std::string::npos || b.find_first_of("-_") == std::string::npos;
}

std::string translationLanguageFor(const std::string& locale)
{
    auto l = toLower(replaceAll(locale, "_", "-"));
    auto p = primary(l);
    if (p.empty() || p == "c" || p == "posix")
        return "en";
    if (p == "zh") {
        for (const char* traditional : {"hant", "-tw", "-hk", "-mo"})
            if (l.find(traditional) != std::string::npos)
                return "zh-TW";
        return "zh-CN";
    }
    if (p == "he")
        return "iw";
    if (p == "nb" || p == "nn")
        return "no";
    return p;
}

std::string googleTranslatePage(const std::string& text, const std::string& target)
{
    return "https://translate.google.com/?sl=auto&tl=" + urlEncode(target) +
           "&text=" + urlEncode(utf8Prefix(text, kMaxPageText)) + "&op=translate";
}

Translation parseGoogleTranslation(const std::string& body)
{
    Json j;
    try {
        j = Json::parse(body);
    } catch (const std::exception&) {
        throw MetadataError(std::string("Unexpected response from ") + kService + ".");
    }
    if (!j.is_object())
        throw MetadataError(std::string("Unexpected response from ") + kService + ".");
    Translation t;
    t.source = jstr(j, "src");
    if (j.contains("sentences") && j["sentences"].is_array())
        for (auto& s : j["sentences"]) {
            if (!s.is_object())
                continue;
            if (s.contains("trans") && s["trans"].is_string())
                t.text += s["trans"].get<std::string>();
            else if (s.contains("src_translit") && s["src_translit"].is_string())
                t.phonetic = s["src_translit"].get<std::string>();
        }
    t.text = trim(t.text);
    if (j.contains("dict") && j["dict"].is_array())
        for (auto& d : j["dict"]) {
            if (!d.is_object() || !d.contains("terms") || !d["terms"].is_array())
                continue;
            Translation::Meanings m;
            m.partOfSpeech = jstr(d, "pos");
            for (auto& term : d["terms"])
                if (term.is_string() && m.terms.size() < kMaxTerms)
                    m.terms.push_back(term.get<std::string>());
            if (!m.terms.empty())
                t.dictionary.push_back(std::move(m));
        }
    if (t.text.empty())
        throw MetadataError(std::string(kService) + " gave no translation.");
    return t;
}

Translation translate(const std::string& text, const std::string& target)
{
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    (void)text;
    (void)target;
    throw MetadataError("HTTPS is unavailable: this build of OurNotes was compiled without OpenSSL, so it cannot "
                        "translate.");
#else
    httplib::Client cli("https://translate.googleapis.com");
    cli.set_connection_timeout(kTimeoutSeconds);
    cli.set_read_timeout(kTimeoutSeconds);
    cli.set_write_timeout(kTimeoutSeconds);
    // dt: the translation (t), a word's dictionary entries (bd), how it is said (rm);
    // hl: the parts of speech named in the target language; dj: JSON with names
    auto path = "/translate_a/single?client=gtx&sl=auto&tl=" + urlEncode(target) + "&hl=" + urlEncode(target) +
                "&dt=t&dt=bd&dt=rm&dj=1&ie=UTF-8&oe=UTF-8";
    httplib::Headers headers = {{"User-Agent", userAgent()}, {"Accept", "application/json"}};
    // (the text in the body, not the address: it may be long)
    auto res = cli.Post(path, headers, "q=" + urlEncode(text), "application/x-www-form-urlencoded");
    if (!res)
        throwNetworkError(res.error(), "translate.googleapis.com");
    if (res->status == 429 || res->status == 503)
        throw MetadataError(std::string(kService) + " is refusing for now (too many requests). Please try again later.");
    if (res->status != 200)
        throw MetadataError(std::string(kService) + " returned an error (HTTP " + std::to_string(res->status) + ").");
    auto t = parseGoogleTranslation(res->body);
    t.target = target;
    return t;
#endif
}

} // namespace on::formats
