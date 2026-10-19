// Widget-level tests, run with the offscreen Qt platform.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QTimer>
#include <QMessageBox>
#include <QLayout>
#include <QToolButton>
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QTextBlock>
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
#include "gui/item_pane.h"
#include "gui/items_model.h"
#include "gui/metadata_progress.h"
#include "gui/math_render.h"
#include "gui/note_editor.h"
#include "gui/note_html.h"
#include "gui/preferences.h"
#include "gui/sync_controller.h"
#include "gui/theme.h"
#include "gui/ui_scale.h"
#include "gui/reader/pdf_view.h"
#include "gui/reader/reader_tab.h"

using namespace fp;
using namespace fp::gui;

namespace {

// The QApplication lives in main() below, so it is destroyed before static teardown.
QApplication& app() { return *qApp; }

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

TEST_CASE("reader: PDF pages follow dark mode")
{
    app();
    Env env;
    // paper becomes dark, ink light, and colours keep their hue
    QImage img(3, 1, QImage::Format_RGB888);
    img.setPixelColor(0, 0, Qt::white);
    img.setPixelColor(1, 0, Qt::black);
    img.setPixelColor(2, 0, QColor(220, 30, 30));
    darkenPage(img);
    CHECK(img.pixelColor(0, 0) == darkPageColor());
    CHECK(img.pixelColor(1, 0).lightness() > 200);
    CHECK(img.pixelColor(2, 0).red() > img.pixelColor(2, 0).green() + 80);

    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key");
    applyAppearance(Appearance::Light);
    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 900);
    tab.show();
    auto* view = tab.view();
    auto paper = [&] {  // a blank spot on the page
        auto img = view->viewport()->grab().toImage();
        return img.pixelColor((view->pageToViewport(0, {500, 500}) * img.devicePixelRatio()).toPoint());
    };
    auto inkIsLight = [&] {  // the darkest and lightest pixels along the first line of text
        auto img = view->viewport()->grab().toImage();
        int lightest = 0;
        auto a = charCenter(*view, 0) * img.devicePixelRatio(), b = charCenter(*view, 10) * img.devicePixelRatio();
        for (int x = int(a.x()); x < int(b.x()); ++x)
            lightest = std::max(lightest, img.pixelColor(x, int(a.y())).lightness());
        return lightest > 180;
    };
    auto* toggle = tab.findChild<QAction*>("darkPages");
    REQUIRE(toggle);
    CHECK(!view->darkPages());
    CHECK(!toggle->isVisible());
    CHECK(QTest::qWaitFor([&] { return paper().lightness() > 240; }));

    applyAppearance(Appearance::Dark);
    QApplication::processEvents();
    CHECK(view->darkPages());
    CHECK(toggle->isVisible());
    CHECK(toggle->isChecked());
    CHECK(QTest::qWaitFor([&] { return paper() == darkPageColor() && inkIsLight(); }));
    // highlights stay visible on dark paper (multiplying would leave it dark)
    view->setTool(ReaderTool::Highlight);
    drag(view->viewport(), charCenter(*view, 0), charCenter(*view, 10));
    view->setTool(ReaderTool::Select);
    view->clearTextSelection();
    QApplication::processEvents();
    {
        auto img = view->viewport()->grab().toImage();
        auto top = charCenter(*view, 2) * img.devicePixelRatio();
        // just above the glyphs, inside the highlight box
        QColor c = img.pixelColor(int(top.x()), int(top.y() - 9 * img.devicePixelRatio()));
        CHECK(c.lightness() > darkPageColor().lightness() + 25);
        CHECK(c.red() > c.blue() + 30);
        CHECK(inkIsLight());
    }
    if (auto dir = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        tab.grab().save(dir + "/reader-dark.png");

    // the toolbar button shows the PDF's own colours, and is remembered
    toggle->trigger();
    CHECK(!view->darkPages());
    CHECK(env.config.get("/reader/darkPages").get<bool>() == false);
    CHECK(QTest::qWaitFor([&] { return paper().lightness() > 240; }));
    toggle->trigger();
    CHECK(view->darkPages());

    applyAppearance(Appearance::System);
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

namespace {

int countMath(QTextDocument* doc, QStringList* latex = nullptr)
{
    int n = 0;
    for (auto b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            auto f = it.fragment().charFormat();
            QString tex;
            if (f.isImageFormat() && parseMathResourceName(f.toImageFormat().name(), &tex, nullptr)) {
                ++n;
                if (latex)
                    *latex << tex;
            }
        }
    return n;
}

} // namespace

TEST_CASE("notes: Zotero math markup survives load and save")
{
    app();
    Env env;
    QTextDocument doc;
    QString note = "<div data-schema-version=\"9\"><p>Energy <span class=\"math\">$E=mc^2$</span> and "
                   "<span class=\"math\">$a &lt; b$</span> hold.</p>"
                   "<pre class=\"math\">$$\\int_0^1 x\\,dx = \\frac{1}{2}$$</pre><p>After.</p></div>";
    doc.setHtml(noteHtmlForEditor(note, env.lib, &doc));
    QStringList latex;
    CHECK(countMath(&doc, &latex) == 3);
    CHECK(latex.contains("a < b"));
    auto out = documentToNoteHtml(&doc);
    CHECK(out.contains("<span class=\"math\">$E=mc^2$</span>"));
    CHECK(out.contains("<span class=\"math\">$a &lt; b$</span>"));
    CHECK(out.contains("<pre class=\"math\">$$\\int_0^1 x\\,dx = \\frac{1}{2}$$</pre>"));
    CHECK(out.contains("<p>After.</p>"));

    auto m = renderMath("\\frac{a}{b}", false, 16, Qt::black, 1);
    CHECK(m.ok);
    CHECK(m.descent > 0);  // fractions reach below the baseline
    auto bad = renderMath("\\frac{a}{", false, 16, Qt::black, 1);
    CHECK(!bad.ok);
}

TEST_CASE("notes: typing $...$ makes a formula, prices stay text")
{
    app();
    NoteTextEdit edit;
    edit.resize(500, 300);
    edit.show();
    QTest::keyClicks(&edit, "Let $x^2$ be");
    QStringList latex;
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"x^2"});
    CHECK(edit.toPlainText().startsWith("Let "));
    CHECK(edit.toPlainText().endsWith(" be"));

    edit.clear();
    QTest::keyClicks(&edit, "It costs $5 and $10 today");
    CHECK(countMath(edit.document()) == 0);

    edit.clear();
    QTest::keyClicks(&edit, "$$\\sum_i x_i$$");
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK((edit.document()->begin().blockFormat().alignment() & Qt::AlignHCenter) != 0);
}

TEST_CASE("notes: wide images are scaled to fit without horizontal scrolling")
{
    app();
    NoteTextEdit edit;
    edit.resize(320, 400);
    edit.show();
    QImage big(1000, 400, QImage::Format_RGB32);
    big.fill(Qt::darkCyan);
    edit.document()->addResource(QTextDocument::ImageResource, QUrl("test:big"), big);
    edit.setHtml("<p>Figure:</p><p><img src=\"test:big\" width=\"1000\" height=\"400\"></p>");
    QApplication::processEvents();
    CHECK(edit.horizontalScrollBar()->maximum() == 0);
    CHECK(edit.document()->documentLayout()->documentSize().width() <= edit.viewport()->width() + 1);
    auto snapshots = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR");
    if (!snapshots.isEmpty())
        edit.grab().save(snapshots + "/note-image-fit.png");
}

TEST_CASE("item pane: a paper's notes appear in the Notes tab next to Info")
{
    app();
    Env env;
    auto& lib = env.library;
    auto paper = lib.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = jstr(paper, "key");
    lib.createNote(env.lib, "<p>First thoughts</p>", key);
    lib.createNote(env.lib, "<p>Second: <span class=\"math\">$x^2$</span></p>", key);
    auto bare = lib.createItem(env.lib, Json{{"itemType", "book"}, {"title", "No notes"}});

    ItemPane pane;
    pane.resize(420, 700);
    pane.show();
    pane.showItems(env.lib, {qs(key)});
    auto* tabs = pane.findChild<QTabWidget*>();
    REQUIRE(tabs);
    CHECK(tabs->currentIndex() == 1);  // notes shown directly
    CHECK(tabs->tabText(1) == "Notes (2)");
    auto* panel = pane.findChild<ItemNotesPanel*>();
    REQUIRE(panel);
    CHECK(panel->count() == 2);
    CHECK(!panel->editor()->key().isEmpty());

    pane.showItems(env.lib, {qs(jstr(bare, "key"))});
    CHECK(tabs->currentIndex() == 0);  // nothing to show: Info
    CHECK(tabs->tabText(1) == "Notes");
    auto snapshots = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR");
    pane.showItems(env.lib, {qs(key)});
    QApplication::processEvents();
    if (!snapshots.isEmpty())
        pane.grab().save(snapshots + "/item-pane-notes.png");
}

namespace {

// Answers the "Remove Tag" confirmation once it shows up; *text gets its question.
void answerTagConfirmation(bool remove, QString* text)
{
    auto* timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, [timer, remove, text] {
        for (auto* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (!box || !box->isVisible() || box->objectName() != "confirmRemoveTag")
                continue;
            *text = box->text();
            if (auto dir = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
                box->grab().save(dir + "/tag-remove-confirm.png");
            for (auto* b : box->buttons())
                if ((box->buttonRole(b) == QMessageBox::DestructiveRole) == remove) {
                    b->click();
                    break;
                }
            timer->stop();
            timer->deleteLater();
            return;
        }
    });
    timer->start();
}

bool confirmationVisible()
{
    for (auto* w : QApplication::topLevelWidgets())
        if (qobject_cast<QMessageBox*>(w) && w->isVisible())
            return true;
    return false;
}

} // namespace

TEST_CASE("item pane: tags are shown as visible chips")
{
    const std::string kLongTag = "Computer Science - Computer Vision and Pattern Recognition and Robotics";
    app();
    Env env;
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"},
                                                      {"title", "Tagged"},
                                                      {"tags", Json::array({Json{{"tag", "robotics"}},
                                                                            Json{{"tag", "humanoid"}, {"type", 1}},
                                                                            Json{{"tag", kLongTag}, {"type", 1}}})}});
    auto key = jstr(paper, "key");
    auto tagsOf = [&] {
        QStringList out;
        for (auto& t : env.library.item(env.lib, key)->value("tags", Json::array()))
            out << qs(jstr(t, "tag"));
        return out;
    };
    ItemPane pane;
    pane.resize(420, 900);
    pane.show();
    pane.showItems(env.lib, {qs(key)});
    pane.findChild<QTabWidget*>()->setCurrentIndex(0);
    QApplication::processEvents();
    QApplication::processEvents();
    TagsBox* tags = nullptr;
    for (auto* t : pane.findChildren<TagsBox*>())
        if (t->isVisible())
            tags = t;
    REQUIRE(tags);
    auto chips = [&] {
        QList<QWidget*> out;
        for (auto* chip : tags->findChildren<QWidget*>("tagChip"))
            if (chip->isVisible())
                out << chip;
        return out;
    };
    CHECK(chips().size() == 3);
    for (auto* chip : chips()) {
        CHECK(chip->height() > 10);
        // chips never stick out of the pane, however long the tag
        CHECK(chip->mapTo(&pane, QPoint(chip->width(), 0)).x() <= pane.width());
        // each has its own remove button inside it
        auto* remove = chip->findChild<QAbstractButton*>("removeTag");
        REQUIRE(remove);
        CHECK(remove->isVisible());
        CHECK(chip->rect().contains(remove->geometry()));
    }
    auto snapshots = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR");
    if (!snapshots.isEmpty())
        tags->grab().save(snapshots + "/tags.png");

    QWidget* robotics = nullptr;
    for (auto* chip : chips())
        if (chip->toolTip() == "robotics")
            robotics = chip;
    REQUIRE(robotics);

    // clicking the tag itself removes nothing and asks nothing
    QTest::mouseClick(robotics, Qt::LeftButton, {}, QPoint(12, robotics->height() / 2));
    QTest::qWait(50);
    CHECK(!confirmationVisible());
    CHECK(tagsOf().contains("robotics"));

    // the remove button asks first; Cancel keeps the tag
    QString question;
    answerTagConfirmation(false, &question);
    QTest::mouseClick(robotics->findChild<QAbstractButton*>("removeTag"), Qt::LeftButton);
    REQUIRE(QTest::qWaitFor([&] { return !question.isEmpty(); }));
    CHECK(question.contains("robotics"));
    QApplication::processEvents();
    CHECK(tagsOf().contains("robotics"));
    CHECK(chips().size() == 3);

    // confirming removes it
    question.clear();
    answerTagConfirmation(true, &question);
    QTest::mouseClick(robotics->findChild<QAbstractButton*>("removeTag"), Qt::LeftButton);
    REQUIRE(QTest::qWaitFor([&] { return !question.isEmpty(); }));
    CHECK(QTest::qWaitFor([&] { return chips().size() == 2; }));
    CHECK(!tagsOf().contains("robotics"));
    CHECK(tagsOf().size() == 2);
    CHECK(chips().size() == 2);
}

namespace {

// The formula source being edited in place (text marked with kMathSourceProperty).
QString mathSource(QTextDocument* doc)
{
    QString out;
    for (auto b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().property(kMathSourceProperty).toInt())
                out += it.fragment().text();
    return out;
}

// Centre of the n-th formula image in viewport coordinates.
QPoint formulaCenter(NoteTextEdit& edit, int n = 0)
{
    auto* doc = edit.document();
    for (auto b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            auto f = it.fragment().charFormat();
            if (!f.isImageFormat() || !parseMathResourceName(f.toImageFormat().name(), nullptr, nullptr) || n--)
                continue;
            QTextCursor before(doc), after(doc);
            before.setPosition(it.fragment().position());
            after.setPosition(it.fragment().position() + 1);
            auto r1 = edit.cursorRect(before), r2 = edit.cursorRect(after);
            return QPoint((r1.left() + r2.left()) / 2, r1.center().y());
        }
    return {};
}

} // namespace

TEST_CASE("notes: $$ starts a formula that is typed in place")
{
    app();
    NoteTextEdit edit;
    edit.resize(500, 300);
    edit.show();
    QTest::keyClicks(&edit, "Area $$");
    CHECK(countMath(edit.document()) == 0);
    CHECK(mathSource(edit.document()) == "$$");  // an empty inline formula, cursor inside
    QTest::keyClicks(&edit, "\\pi r^2");
    CHECK(mathSource(edit.document()) == "$\\pi r^2$");
    CHECK(documentToNoteHtml(edit.document()).contains("<span class=\"math\">$\\pi r^2$</span>"));
    QTest::keyClicks(&edit, "$ here");  // typing the closing $ finishes the formula
    QStringList latex;
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"\\pi r^2"});
    CHECK(mathSource(edit.document()).isEmpty());
    CHECK(edit.toPlainText().endsWith(" here"));
    // the text after the formula is plain text, not formula-formatted
    auto last = edit.document()->lastBlock();
    auto f = (--last.end()).fragment().charFormat();
    CHECK(!f.hasProperty(QTextFormat::ImageName));
    CHECK(f.verticalAlignment() == QTextCharFormat::AlignNormal);

    // undo brings back the source without rendering it again straight away
    edit.clear();
    QTest::keyClicks(&edit, "a $$x$");
    REQUIRE(countMath(edit.document()) == 1);
    edit.undo();
    CHECK(countMath(edit.document()) == 0);
    CHECK(edit.toPlainText().contains("$x$"));

    // on an empty line, $$ starts a centred display formula; $$ ends it and moves on
    edit.clear();
    QTest::keyClicks(&edit, "$$");
    CHECK(mathSource(edit.document()) == "$$$$");
    QTest::keyClicks(&edit, "E=mc^2$$next");
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"E=mc^2"});
    CHECK((edit.document()->begin().blockFormat().alignment() & Qt::AlignHCenter) != 0);
    CHECK(edit.document()->blockCount() == 2);
    CHECK(edit.document()->lastBlock().text() == "next");
    CHECK((edit.document()->lastBlock().blockFormat().alignment() & Qt::AlignHCenter) == 0);
    CHECK(documentToNoteHtml(edit.document()).contains("<pre class=\"math\">$$E=mc^2$$</pre>"));
}

TEST_CASE("notes: clicking a formula edits it in place with a live preview")
{
    app();
    Env env;
    NoteTextEdit edit;
    edit.resize(500, 300);
    edit.show();
    edit.activateWindow();
    edit.setFocus();
    QString note = "<div data-schema-version=\"9\"><p>Let <span class=\"math\">$x^2$</span> be.</p></div>";
    edit.setHtml(noteHtmlForEditor(note, env.lib, edit.document()));
    edit.prepareDocument();
    QApplication::processEvents();
    auto center = formulaCenter(edit);
    REQUIRE(!center.isNull());

    QTest::mouseClick(edit.viewport(), Qt::LeftButton, {}, center);
    CHECK(countMath(edit.document()) == 0);
    CHECK(mathSource(edit.document()) == "$x^2$");
    auto* preview = edit.viewport()->findChild<QLabel*>();
    REQUIRE(preview);
    CHECK(preview->isVisible());
    CHECK(!preview->pixmap().isNull());

    QTest::keyClicks(&edit, "+1");
    CHECK(mathSource(edit.document()) == "$x^2+1$");
    QTest::keyClick(&edit, Qt::Key_Escape);
    QStringList latex;
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"x^2+1"});
    CHECK(!preview->isVisible());
    CHECK(edit.toPlainText().endsWith(" be."));

    // moving the cursor out of the formula also renders it
    QTest::mouseClick(edit.viewport(), Qt::LeftButton, {}, formulaCenter(edit));
    CHECK(countMath(edit.document()) == 0);
    QTest::keyClick(&edit, Qt::Key_End);
    CHECK(countMath(edit.document()) == 1);

    // a formula left empty is removed
    QTest::mouseClick(edit.viewport(), Qt::LeftButton, {}, formulaCenter(edit));
    for (int i = 0; i < 5; ++i)
        QTest::keyClick(&edit, Qt::Key_Backspace);
    CHECK(mathSource(edit.document()) == "$$");
    QTest::keyClick(&edit, Qt::Key_Return);
    CHECK(countMath(edit.document()) == 0);
    CHECK(edit.toPlainText() == "Let  be.");
}

TEST_CASE("appearance: dark mode, formula colour and readable note text")
{
    app();
    Env env;
    CHECK(appearanceFromConfig(env.config) == Appearance::System);

    applyAppearance(Appearance::Dark);
    CHECK(isDarkTheme());
    NoteTextEdit edit;
    edit.resize(400, 200);
    edit.show();
    QString note = "<div data-schema-version=\"9\"><p><span style=\"color: #000000\">Black text</span> "
                   "<span class=\"math\">$\\frac{a}{b}$</span></p></div>";
    edit.setHtml(noteHtmlForEditor(note, env.lib, edit.document()));
    edit.prepareDocument();
    QApplication::processEvents();

    // black text from a light theme is shown in the text colour, but saved unchanged
    auto first = edit.document()->begin().begin().fragment().charFormat();
    CHECK(!first.hasProperty(QTextFormat::ForegroundBrush));
    CHECK(documentToNoteHtml(edit.document()).contains("color: #000000"));
    CHECK(!edit.document()->isModified());

    // formulas are drawn in the text colour (light on dark) unless a colour is set
    auto brightest = [&](auto pred) {
        auto img = edit.viewport()->grab().toImage();
        auto c = formulaCenter(edit) * img.devicePixelRatio();
        int found = 0;
        for (int y = c.y() - 12; y < c.y() + 12; ++y)
            for (int x = c.x() - 12; x < c.x() + 12; ++x)
                if (img.rect().contains(x, y) && pred(img.pixelColor(x, y)))
                    ++found;
        return found;
    };
    CHECK(brightest([](QColor c) { return c.lightness() > 200; }) > 3);
    env.config.set("/notes/formulaColor", "#ff0000");
    emit env.context.appearanceChanged();
    QApplication::processEvents();
    CHECK(brightest([](QColor c) { return c.red() > 180 && c.green() < 90; }) > 3);
    env.config.set("/notes/formulaColor", "");

    if (auto dir = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        edit.grab().save(dir + "/note-dark.png");

    // back to light: the original black is shown again
    applyAppearance(Appearance::Light);
    QApplication::processEvents();
    CHECK(!isDarkTheme());
    first = edit.document()->begin().begin().fragment().charFormat();
    CHECK(first.foreground().color() == QColor(Qt::black));

    // the preference switches live and is stored
    SyncController sync;
    PreferencesDialog dlg(&sync);
    auto* combo = dlg.findChild<QComboBox*>("appearance");
    REQUIRE(combo);
    combo->setCurrentIndex(combo->findData("dark"));
    CHECK(env.config.get("/ui/appearance").get<std::string>() == "dark");
    CHECK(isDarkTheme());
    // also windows that were never shown (Qt only updates those with a native window)
    CHECK(dlg.palette().color(QPalette::Window) == QApplication::palette().color(QPalette::Window));
    if (auto dir = qEnvironmentVariable("FREEPEN_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        dlg.grab().save(dir + "/preferences-dark.png");
    combo->setCurrentIndex(combo->findData("system"));
    CHECK(appearanceFromConfig(env.config) == Appearance::System);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    doctest::Context context(argc, argv);
    return context.run();
}
