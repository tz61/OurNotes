#include <doctest.h>

#include "core/util.h"
#include "formats/metadata.h"
#include "formats/translate.h"

using namespace on;
using namespace on::formats;

TEST_CASE("translation: Google's answer for a word, with how it is said and its other meanings")
{
    // (shortened from a real answer, hl=zh-CN)
    auto t = parseGoogleTranslation(
        R"({"sentences":[{"trans":"注意力","orig":"attention","backend":10},)"
        R"({"translit":"Zhùyì lì","src_translit":"əˈten(t)SHən"}],)"
        R"("dict":[{"pos":"名词","terms":["注意","关注","注意力","关心","意思","意兴","留心","照顾","关照"],)"
        R"("entry":[{"word":"注意","reverse_translation":["attention","mind"],"score":0.6}],"base_form":"attention"},)"
        R"({"pos":"动词","terms":[]}],)"
        R"("src":"en","confidence":1.0,"spell":{},"ld_result":{"srclangs":["en"]}})");
    CHECK(t.text == "注意力");
    CHECK(t.source == "en");
    CHECK(t.phonetic == "əˈten(t)SHən");
    REQUIRE(t.dictionary.size() == 1);  // (none without terms)
    CHECK(t.dictionary[0].partOfSpeech == "名词");
    CHECK(t.dictionary[0].terms.size() == 8);  // (the first few)
    CHECK(t.dictionary[0].terms[0] == "注意");
}

TEST_CASE("translation: Google's answer for sentences, one after the other")
{
    auto t = parseGoogleTranslation(R"({"sentences":[{"trans":"我们提出一个新的网络。","orig":"We propose a new network. "},)"
                                    R"({"trans":"效果很好。","orig":"It works well."}],"src":"en"})");
    CHECK(t.text == "我们提出一个新的网络。效果很好。");
    CHECK(t.phonetic.empty());
    CHECK(t.dictionary.empty());
    CHECK_THROWS_AS(parseGoogleTranslation(R"({"sentences":[],"src":"en"})"), MetadataError);
    CHECK_THROWS_AS(parseGoogleTranslation("<html>Too many requests</html>"), MetadataError);
    CHECK_THROWS_AS(parseGoogleTranslation("[1,2]"), MetadataError);
}

TEST_CASE("translation: languages, their names, and the one of the system")
{
    CHECK(languageName("en") == "English");
    CHECK(languageName("en-GB") == "English");
    CHECK(languageName("zh-CN") == "中文（简体）");
    CHECK(languageName("zh-tw") == "中文（繁體）");
    CHECK(languageName("xx") == "xx");

    CHECK(sameLanguage("zh-CN", "zh-cn"));
    CHECK(sameLanguage("zh", "zh-TW"));
    CHECK_FALSE(sameLanguage("zh-CN", "zh-TW"));
    CHECK(sameLanguage("en", "en-GB"));
    CHECK_FALSE(sameLanguage("en", "de"));
    CHECK_FALSE(sameLanguage("", "en"));

    CHECK(translationLanguageFor("zh_CN") == "zh-CN");
    CHECK(translationLanguageFor("zh-Hans-CN") == "zh-CN");
    CHECK(translationLanguageFor("zh-Hant-TW") == "zh-TW");
    CHECK(translationLanguageFor("zh_HK") == "zh-TW");
    CHECK(translationLanguageFor("en_US") == "en");
    CHECK(translationLanguageFor("pt-BR") == "pt");
    CHECK(translationLanguageFor("he_IL") == "iw");
    CHECK(translationLanguageFor("nb_NO") == "no");
    CHECK(translationLanguageFor("C") == "en");
    CHECK(translationLanguageFor("") == "en");
    for (auto& l : translationLanguages())
        CHECK(translationLanguageFor(l.code) == l.code);

    auto page = googleTranslatePage("deep learning", "zh-CN");
    CHECK(startsWith(page, "https://translate.google.com/?sl=auto&tl=zh-CN&text=deep"));
    CHECK(page.find(' ') == std::string::npos);
    CHECK(googleTranslatePage(std::string(6000, 'a'), "en").size() < 5100);
}
