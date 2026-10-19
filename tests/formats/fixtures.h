#pragma once

#include <doctest.h>

#include "core/json.h"

namespace fixtures {

using fp::Json;

inline Json journalArticle()
{
    return Json::parse(R"({
        "key": "ABCD2345",
        "itemType": "journalArticle",
        "title": "Deep learning",
        "creators": [
            {"creatorType": "author", "firstName": "Yann", "lastName": "LeCun"},
            {"creatorType": "author", "firstName": "Yoshua", "lastName": "Bengio"},
            {"creatorType": "author", "firstName": "Geoffrey", "lastName": "Hinton"}],
        "abstractNote": "Deep learning allows computational models to learn.\nSecond paragraph & more.",
        "publicationTitle": "Nature",
        "volume": "521",
        "issue": "7553",
        "pages": "436-444",
        "date": "2015-05-28",
        "journalAbbreviation": "Nature",
        "DOI": "10.1038/nature14539",
        "url": "https://www.nature.com/articles/nature14539",
        "ISSN": "1476-4687",
        "language": "en",
        "tags": [{"tag": "machine learning", "type": 0}, {"tag": "neural networks", "type": 0}],
        "collections": [],
        "relations": {}
    })");
}

inline Json conferencePaper()
{
    return Json::parse(R"({
        "itemType": "conferencePaper",
        "title": "Deep residual learning for image recognition",
        "creators": [
            {"creatorType": "author", "firstName": "Kaiming", "lastName": "He"},
            {"creatorType": "author", "firstName": "Xiangyu", "lastName": "Zhang"},
            {"creatorType": "author", "firstName": "Shaoqing", "lastName": "Ren"},
            {"creatorType": "author", "firstName": "Jian", "lastName": "Sun"}],
        "proceedingsTitle": "Proceedings of the IEEE Conference on Computer Vision and Pattern Recognition",
        "conferenceName": "CVPR 2016",
        "publisher": "IEEE",
        "place": "Las Vegas, NV, USA",
        "date": "2016-06",
        "pages": "770-778",
        "DOI": "10.1109/CVPR.2016.90",
        "tags": [],
        "collections": [],
        "relations": {}
    })");
}

inline Json book()
{
    return Json::parse(R"({
        "itemType": "book",
        "title": "Deep learning",
        "creators": [
            {"creatorType": "author", "firstName": "Ian", "lastName": "Goodfellow"},
            {"creatorType": "author", "firstName": "Yoshua", "lastName": "Bengio"},
            {"creatorType": "author", "firstName": "Aaron", "lastName": "Courville"}],
        "publisher": "MIT Press",
        "place": "Cambridge, MA",
        "date": "2016",
        "ISBN": "9780262035613",
        "tags": [{"tag": "AI", "type": 0}],
        "collections": [],
        "relations": {}
    })");
}

inline Json preprint()
{
    return Json::parse(R"({
        "itemType": "preprint",
        "title": "Attention is all you need",
        "creators": [
            {"creatorType": "author", "firstName": "Ashish", "lastName": "Vaswani"},
            {"creatorType": "author", "firstName": "Noam", "lastName": "Shazeer"},
            {"creatorType": "author", "firstName": "Niki", "lastName": "Parmar"},
            {"creatorType": "author", "firstName": "Jakob", "lastName": "Uszkoreit"},
            {"creatorType": "author", "firstName": "Llion", "lastName": "Jones"},
            {"creatorType": "author", "firstName": "Aidan N.", "lastName": "Gomez"},
            {"creatorType": "author", "firstName": "Lukasz", "lastName": "Kaiser"},
            {"creatorType": "author", "firstName": "Illia", "lastName": "Polosukhin"}],
        "repository": "arXiv",
        "archiveID": "arXiv:1706.03762",
        "date": "2017-06-12",
        "DOI": "10.48550/arXiv.1706.03762",
        "url": "https://arxiv.org/abs/1706.03762",
        "extra": "arXiv:1706.03762 [cs.CL]",
        "tags": [],
        "collections": [],
        "relations": {}
    })");
}

inline Json bookSection()
{
    return Json::parse(R"({
        "itemType": "bookSection",
        "title": "Learning representations",
        "creators": [
            {"creatorType": "author", "firstName": "Jane", "lastName": "Doe"},
            {"creatorType": "editor", "firstName": "Ada", "lastName": "Lovelace"},
            {"creatorType": "editor", "firstName": "Alan M.", "lastName": "Turing"}],
        "bookTitle": "Handbook of machine learning",
        "edition": "2",
        "publisher": "Springer",
        "place": "Berlin",
        "date": "2020",
        "pages": "10-25",
        "tags": [],
        "collections": [],
        "relations": {}
    })");
}

inline Json webpage()
{
    return Json::parse(R"({
        "itemType": "webpage",
        "title": "Coronavirus disease (COVID-19) pandemic",
        "creators": [{"creatorType": "author", "name": "World Health Organization"}],
        "websiteTitle": "WHO",
        "url": "https://www.who.int/emergencies/diseases/novel-coronavirus-2019",
        "accessDate": "2026-10-18T09:00:00Z",
        "tags": [],
        "collections": [],
        "relations": {}
    })");
}

inline Json thesis()
{
    return Json::parse(R"({
        "itemType": "thesis",
        "title": "On the theory of everything",
        "creators": [{"creatorType": "author", "firstName": "John Ronald", "lastName": "Smith"}],
        "thesisType": "PhD thesis",
        "university": "University of Cambridge",
        "place": "Cambridge",
        "date": "2019",
        "tags": [],
        "collections": [],
        "relations": {}
    })");
}

// Compares the bibliographic core of two items (round-trip checks).
inline void checkSameItem(const Json& expected, const Json& actual)
{
    CAPTURE(actual.dump(2));
    CHECK(actual.value("itemType", "") == expected.value("itemType", ""));
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        const std::string field = it.key();
        if (field == "creators" || field == "tags" || field == "collections" || field == "relations" ||
            field == "key" || field == "extra" || field == "accessDate" || field == "abstractNote" ||
            !it->is_string())
            continue;
        CAPTURE(field);
        CHECK(actual.value(field, "") == it->get<std::string>());
    }
    const auto& ec = expected["creators"];
    const auto& ac = actual.value("creators", Json::array());
    REQUIRE(ac.size() == ec.size());
    for (size_t i = 0; i < ec.size(); ++i) {
        CHECK(ac[i].value("creatorType", "") == ec[i].value("creatorType", ""));
        CHECK(ac[i].value("firstName", "") == ec[i].value("firstName", ""));
        CHECK(ac[i].value("lastName", "") == ec[i].value("lastName", ""));
        CHECK(ac[i].value("name", "") == ec[i].value("name", ""));
    }
}

} // namespace fixtures
