#include <doctest.h>

#include "core/zotero_importer.h"

using namespace on;

TEST_CASE("zotero: prefs.js values")
{
    auto prefs = parseZoteroPrefs(
        "// Mozilla User Preferences\n"
        "user_pref(\"extensions.zotero.dataDir\", \"C:\\\\Users\\\\me\\\\Zotero \\\"data\\\"\");\n"
        "user_pref(\"extensions.zotero.useDataDir\", true);\n"
        "user_pref(\"extensions.zotero.lastViewedFolder\", -12);\n"
        "user_pref(\"extensions.zotero.sync.server.username\",\"me\");\n"
        "user_pref(\"broken\", maybe);\n"
        "user_pref(\"unfinished\", \"no end");
    CHECK(prefs["extensions.zotero.dataDir"] == "C:\\Users\\me\\Zotero \"data\"");
    CHECK(prefs["extensions.zotero.useDataDir"] == "true");
    CHECK(prefs["extensions.zotero.lastViewedFolder"] == "-12");
    CHECK(prefs["extensions.zotero.sync.server.username"] == "me");
    CHECK(prefs.count("broken") == 0);
    CHECK(prefs.count("unfinished") == 0);
}

TEST_CASE("zotero: prefs.js with very long values")
{
    // Zotero keeps caches in prefs.js; MSVC's std::regex gave up on values like these
    std::string big(500000, 'x');
    for (size_t i = 0; i < big.size(); i += 1000)
        big[i] = '\\', big[i + 1] = '"';
    auto prefs = parseZoteroPrefs("user_pref(\"cache\", \"" + big + "\");\nuser_pref(\"after\", false);\n");
    CHECK(prefs["cache"].size() == big.size() - big.size() / 1000);
    CHECK(prefs["after"] == "false");
}
