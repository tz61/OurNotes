// Widget-level tests, run with the offscreen Qt platform.

#include <doctest.h>

#include <QApplication>
#include <QComboBox>
#include <QTreeWidget>

#include <atomic>
#include <QTest>
#include <QTextDocument>

#include "../core/test_helpers.h"
#include "../pdf/sample_pdf.h"
#include "core/config.h"
#include "core/library.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/fancy_progress.h"
#include "gui/items_model.h"
#include "gui/metadata_progress.h"
#include "gui/note_html.h"
#include "gui/preferences.h"
#include "gui/sync_controller.h"
#include "gui/ui_scale.h"
#include "gui/reader/pdf_view.h"
#include "gui/reader/reader_tab.h"

using namespace fp;
using namespace fp::gui;

namespace {

QApplication& app()
{
    static int argc = 1;
    static char name[] = "test_gui";
    static char* argv[] = {name, nullptr};
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QApplication instance(argc, argv);
    return instance;
}

// Library + configuration in a temporary directory, registered as the app context.
struct Env {
    TempDir dir;
    Config config{dir.path / "config.json"};
    Library library{dir.path / "data"};
    AppContext context{config, library};
    int64_t lib = library.defaultLibrary();
};

QPointF charCenter(PdfView& view, int index)
{
    auto& box = view.document()->text(0).chars[index].box;
    return view.pageToViewport(0, QPointF((box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2));
}

void drag(QWidget* w, QPointF from, QPointF to)
{
    QTest::mousePress(w, Qt::LeftButton, {}, from.toPoint());
    QTest::mouseMove(w, ((from + to) / 2).toPoint());
    QTest::mouseMove(w, to.toPoint());
    QTest::mouseRelease(w, Qt::LeftButton, {}, to.toPoint());
}

} // namespace

TEST_CASE("pdf view: highlight, note and ink tools create Zotero-style annotations")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView view;
    view.resize(800, 1000);
    view.open(file);
    view.show();
    QApplication::processEvents();

    std::vector<Json> created;
    QObject::connect(&view, &PdfView::annotationCreated, [&](const Json& d) { created.push_back(d); });

    view.setTool(ReaderTool::Highlight);
    drag(view.viewport(), charCenter(view, 0), charCenter(view, 10));  // "Hello World"
    REQUIRE(created.size() == 1);
    CHECK(created[0]["annotationType"] == "highlight");
    CHECK(created[0]["annotationText"] == "Hello World");
    CHECK(created[0]["annotationPageLabel"] == "1");
    auto rect = created[0]["annotationPosition"]["rects"][0];
    CHECK(rect[0].get<double>() == doctest::Approx(72).epsilon(0.02));  // PDF user space, origin bottom-left
    CHECK(rect[1].get<double>() > 680);
    CHECK(rect[3].get<double>() < 730);
    CHECK(jstr(created[0], "annotationSortIndex").substr(0, 6) == "00000|");

    view.setTool(ReaderTool::Note);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, view.pageToViewport(0, {300, 400}).toPoint());
    REQUIRE(created.size() == 2);
    CHECK(created[1]["annotationType"] == "note");
    auto nr = created[1]["annotationPosition"]["rects"][0];
    CHECK(nr[2].get<double>() - nr[0].get<double>() == doctest::Approx(22));

    view.setTool(ReaderTool::Ink);
    drag(view.viewport(), view.pageToViewport(0, {100, 500}), view.pageToViewport(0, {200, 560}));
    REQUIRE(created.size() == 3);
    CHECK(created[2]["annotationType"] == "ink");
    CHECK(created[2]["annotationPosition"]["paths"][0].size() >= 4);
}

TEST_CASE("pdf view: double-click selects a word")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView view;
    view.resize(800, 1000);
    view.open(file);
    view.show();
    QApplication::processEvents();
    QTest::mouseDClick(view.viewport(), Qt::LeftButton, {}, charCenter(view, 8).toPoint());
    CHECK(view.selectedText() == "World");
}

TEST_CASE("reader tab persists annotations in the library")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto att = env.library.importAttachment(env.lib, file, jstr(parent, "key"));
    auto key = jstr(att, "key");

    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 900);
    tab.show();
    QApplication::processEvents();
    auto* view = tab.view();
    view->setTool(ReaderTool::Underline);
    drag(view->viewport(), charCenter(*view, 12), charCenter(*view, 15));  // "from"
    auto anns = env.library.annotations(env.lib, key);
    REQUIRE(anns.size() == 1);
    CHECK(anns[0]["annotationType"] == "underline");
    CHECK(anns[0]["annotationText"] == "from");
    CHECK(anns[0]["parentItem"] == key);

    // select it and delete with the keyboard
    view->setTool(ReaderTool::Select);
    view->selectAnnotation(qs(jstr(anns[0], "key")), false);
    QTest::keyClick(view, Qt::Key_Delete);
    CHECK(env.library.annotations(env.lib, key).empty());
}

TEST_CASE("note HTML round trip")
{
    app();
    QTextDocument doc;
    doc.setHtml("<h1>Title</h1><p>Some <b>bold</b>, <i>italic</i> and a <a href=\"https://x.org\">link</a>.</p>"
                "<ul><li>one</li><li>two</li></ul><ol><li>first</li></ol><blockquote><p>quoted</p></blockquote>");
    auto html = documentToNoteHtml(&doc);
    CHECK(html.startsWith("<div data-schema-version=\"9\">"));
    CHECK(html.contains("<h1>Title</h1>"));
    CHECK(html.contains("<strong>bold</strong>"));
    CHECK(html.contains("<em>italic</em>"));
    CHECK(html.contains("<a href=\"https://x.org\">link</a>"));
    CHECK(html.contains("<ul><li>one</li><li>two</li></ul>"));
    CHECK(html.contains("<ol><li>first</li></ol>"));
    CHECK(html.contains("<blockquote><p>quoted</p></blockquote>"));
    QTextDocument again;
    again.setHtml(html);
    CHECK(documentToNoteHtml(&again) == html);
}

TEST_CASE("items model: scopes, search and trash")
{
    app();
    Env env;
    auto& lib = env.library;
    auto a = lib.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Alpha"}});
    auto b = lib.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Beta"}});
    lib.createNote(env.lib, "<p>about gradients</p>", jstr(a, "key"));
    auto coll = lib.createCollection(env.lib, "C");
    lib.addToCollection(env.lib, jstr(coll, "key"), {jstr(b, "key")});

    ItemsModel model;
    model.setScope({env.lib, ScopeKind::Library, {}});
    CHECK(model.rowCount() == 2);
    auto ia = model.indexOfKey(qs(jstr(a, "key")));
    CHECK(model.rowCount(ia) == 1);  // the note

    model.setScope({env.lib, ScopeKind::Collection, jstr(coll, "key")});
    CHECK(model.rowCount() == 1);
    model.setScope({env.lib, ScopeKind::Unfiled, {}});
    CHECK(model.rowCount() == 1);

    model.setScope({env.lib, ScopeKind::Library, {}});
    model.setSearch("gradients", SearchMode::AllFields);
    REQUIRE(model.rowCount() == 1);
    CHECK(model.keyAt(model.index(0, 0)) == qs(jstr(a, "key")));
    model.setSearch({}, SearchMode::AllFields);

    lib.trashItems(env.lib, {jstr(b, "key")});
    model.reload();
    CHECK(model.rowCount() == 1);
    model.setScope({env.lib, ScopeKind::Trash, {}});
    CHECK(model.rowCount() == 1);
}

TEST_CASE("interface scale: defaults to 150% and is editable in preferences")
{
    app();
    Env env;
    CHECK(interfaceScale(env.config) == 150);
    env.config.set("/ui/scale", 1000);
    CHECK(interfaceScale(env.config) == 300);  // clamped
    env.config.set("/ui/scale", 150);

    SyncController sync;
    PreferencesDialog dlg(&sync);
    auto* combo = dlg.findChild<QComboBox*>("interfaceScale");
    REQUIRE(combo);
    REQUIRE(combo->currentData().toInt() == 150);
    combo->setCurrentIndex(combo->findData(125));
    CHECK(env.config.get("/ui/scale").get<int>() == 125);
    CHECK(interfaceScale(env.config) == 125);
    if (qEnvironmentVariableIsSet("FREEPEN_TEST_SNAPSHOT_DIR"))
        dlg.grab().save(qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR") + "/preferences.png");
}

TEST_CASE("retrieve metadata window: progress, results and parent items")
{
    app();
    Env env;
    TempDir tmp;
    std::vector<MetadataProgressDialog::Job> jobs;
    for (auto name : {"found.pdf", "unknown.pdf", "slow.pdf"}) {
        auto file = tmp.path / name;
        writeSamplePdf(file);
        auto att = env.library.importAttachment(env.lib, file);
        jobs.push_back({qs(jstr(att, "key")), name, env.library.attachmentFile(env.lib, att)});
    }
    std::atomic<bool> release{false};
    auto retriever = [&](const fs::path& p, const std::function<void(const std::string&)>& stage)
        -> std::optional<formats::PdfMetadataMatch> {
        stage("Looking up DOI 10.1234/test");
        auto name = pathString(p.filename());
        if (name == "unknown.pdf")
            return std::nullopt;
        if (name == "slow.pdf")
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return formats::PdfMetadataMatch{
            Json{{"itemType", "journalArticle"}, {"title", "Paper for " + name}, {"DOI", "10.1234/test"}},
            "DOI 10.1234/test \u00b7 Crossref"};
    };
    MetadataProgressDialog dlg(env.lib, jobs, nullptr, retriever);
    dlg.show();
    dlg.start();
    REQUIRE(QTest::qWaitFor([&] { return dlg.foundCount() + dlg.notFoundCount() == 2; }, 5000));
    QTest::qWait(500);  // let the bar animation settle
    auto snapshots = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR");
    if (!snapshots.isEmpty())
        dlg.grab().save(snapshots + "/metadata-progress.png");
    CHECK(!dlg.isFinished());
    release = true;
    REQUIRE(QTest::qWaitFor([&] { return dlg.isFinished(); }, 5000));
    QTest::qWait(500);
    if (!snapshots.isEmpty())
        dlg.grab().save(snapshots + "/metadata-done.png");
    CHECK(dlg.foundCount() == 2);
    CHECK(dlg.notFoundCount() == 1);
    for (auto& job : jobs) {
        auto att = *env.library.item(env.lib, ss(job.key));
        bool shouldHaveParent = job.name != "unknown.pdf";
        CHECK(jstr(att, "parentItem").empty() != shouldHaveParent);
        if (shouldHaveParent) {
            auto parent = env.library.item(env.lib, jstr(att, "parentItem"));
            REQUIRE(parent);
            CHECK(jstr(*parent, "title") == "Paper for " + ss(job.name));
            CHECK(jstr(att, "title") == "PDF");
        }
    }
}
