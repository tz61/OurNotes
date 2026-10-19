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
#include <QHeaderView>
#include <QSplitter>
#include <QTabWidget>
#include <QTabBar>
#include <QToolBar>
#include <QListWidget>
#include <QTreeView>
#include <QUndoStack>
#include <QTreeWidget>

#include <array>
#include <atomic>
#include <QNativeGestureEvent>
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
#include "gui/library_view.h"
#include "gui/main_window.h"
#include "gui/items_model.h"
#include "gui/metadata_progress.h"
#include "gui/math_render.h"
#include "gui/note_editor.h"
#include "gui/note_html.h"
#include "gui/preferences.h"
#include "gui/sync_controller.h"
#include "gui/theme.h"
#include "gui/ui_scale.h"
#include "gui/wrap_toolbar.h"
#include "gui/reader/pdf_view.h"
#include "gui/reader/reader_tab.h"

using namespace on;
using namespace on::gui;

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

TEST_CASE("pdf view: an annotation with a comment shows a badge, and clicking it selects the annotation")
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

    auto annotation = [](const char* key, const char* type, const char* color, const char* comment, Json rect) {
        return Json{{"key", key},
                    {"annotationType", type},
                    {"annotationColor", color},
                    {"annotationComment", comment},
                    {"annotationPosition", Json{{"pageIndex", 0}, {"rects", Json::array({rect})}}}};
    };
    auto withComments = [&](bool comments) {
        auto c = [&](const char* text) { return comments ? text : ""; };
        return std::vector<Json>{annotation("HIGHLIT1", "highlight", "#ffd400", c("Why?"), {72, 690, 160, 710}),
                                 annotation("PLAINHL1", "highlight", "#ffd400", "", {72, 600, 160, 620}),
                                 annotation("AREABOX1", "image", "#2ea8e5", c("A figure"), {300, 300, 400, 400}),
                                 annotation("STICKYN1", "note", "#ff6666", c("A note"), {300, 500, 322, 522})};
    };
    view.setAnnotations(withComments(true));
    QApplication::processEvents();
    QRectF badge = view.commentBadgeRect("HIGHLIT1");
    REQUIRE(!badge.isEmpty());
    CHECK(!view.commentBadgeRect("AREABOX1").isEmpty());
    CHECK(view.commentBadgeRect("PLAINHL1").isEmpty());  // no comment
    CHECK(view.commentBadgeRect("STICKYN1").isEmpty());  // a sticky note shows its comment already
    CHECK(view.viewport()->rect().contains(badge.toRect()));
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        view.viewport()->grab().save(dir + "/comment-badges.png");

    // the badge is drawn: the spot looks different without the comment
    QImage shown = view.viewport()->grab(badge.toAlignedRect()).toImage();
    view.setAnnotations(withComments(false));
    QApplication::processEvents();
    CHECK(view.commentBadgeRect("HIGHLIT1").isEmpty());
    QImage hidden = view.viewport()->grab(badge.toAlignedRect()).toImage();
    CHECK(shown != hidden);

    view.setAnnotations(withComments(true));
    QStringList selected;
    QObject::connect(&view, &PdfView::annotationSelected, [&](const QString& key) { selected << key; });
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, view.commentBadgeRect("AREABOX1").center().toPoint());
    CHECK(selected == QStringList{"AREABOX1"});
    CHECK(view.selectedAnnotation() == "AREABOX1");
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, badge.center().toPoint());
    CHECK(view.selectedAnnotation() == "HIGHLIT1");
}

TEST_CASE("pdf view: area annotations resize from any edge or corner")
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
    std::vector<Json> created, changed;
    QObject::connect(&view, &PdfView::annotationCreated, [&](const Json& d) { created.push_back(d); });
    QObject::connect(&view, &PdfView::annotationChanged, [&](const Json& d) {
        changed.push_back(d);
        view.setAnnotations({d});  // as the library does after saving
    });
    view.setTool(ReaderTool::Area);
    drag(view.viewport(), view.pageToViewport(0, {200, 300}), view.pageToViewport(0, {400, 450}));
    REQUIRE(created.size() == 1);
    REQUIRE(created[0]["annotationType"] == "image");
    view.setAnnotations(created);
    view.setTool(ReaderTool::Select);
    view.selectAnnotation({}, false);  // nothing selected: the frame alone can be grabbed

    // [x1, y1, x2, y2] in PDF coordinates (y up: y2 is the top edge)
    auto box = [](const Json& d) {
        auto r = d["annotationPosition"]["rects"][0];
        return std::array<double, 4>{r[0].get<double>(), r[1].get<double>(), r[2].get<double>(), r[3].get<double>()};
    };
    auto at = [&](double x, double y) { return view.pageToViewport(0, {x, y}); };
    auto cursorAt = [&](double x, double y) {
        QTest::mouseMove(view.viewport(), at(x, y).toPoint());
        return view.viewport()->cursor().shape();
    };
    auto start = box(created[0]);

    // the top edge: vertical resize cursor, and only the top moves
    CHECK(cursorAt(300, 300) == Qt::SizeVerCursor);
    drag(view.viewport(), at(300, 300), at(300, 250));
    REQUIRE(changed.size() == 1);
    auto r = box(changed.back());
    CHECK(r[3] == doctest::Approx(start[3] + 50).epsilon(0.005));
    CHECK(r[1] == doctest::Approx(start[1]).epsilon(0.005));
    CHECK(r[0] == doctest::Approx(start[0]).epsilon(0.005));
    CHECK(r[2] == doctest::Approx(start[2]).epsilon(0.005));

    // the left edge
    CHECK(cursorAt(200, 375) == Qt::SizeHorCursor);
    drag(view.viewport(), at(200, 375), at(150, 375));
    REQUIRE(changed.size() == 2);
    r = box(changed.back());
    CHECK(r[0] == doctest::Approx(start[0] - 50).epsilon(0.005));
    CHECK(r[2] == doctest::Approx(start[2]).epsilon(0.005));

    // the bottom-right corner moves two edges
    CHECK(cursorAt(400, 450) == Qt::SizeFDiagCursor);
    CHECK(cursorAt(150, 450) == Qt::SizeBDiagCursor);
    drag(view.viewport(), at(400, 450), at(440, 480));
    REQUIRE(changed.size() == 3);
    r = box(changed.back());
    CHECK(r[2] == doctest::Approx(start[2] + 40).epsilon(0.005));
    CHECK(r[1] == doctest::Approx(start[1] - 30).epsilon(0.005));
    CHECK(r[3] == doctest::Approx(start[3] + 50).epsilon(0.005));

    // inside the frame, dragging moves the whole box
    auto before = box(changed.back());
    double cx = (before[0] + before[2]) / 2, cy = 792 - (before[1] + before[3]) / 2;
    CHECK(cursorAt(cx, cy) == Qt::SizeAllCursor);
    drag(view.viewport(), at(cx, cy), at(cx + 20, cy + 10));
    r = box(changed.back());
    CHECK(r[0] - before[0] == doctest::Approx(20).epsilon(0.05));
    CHECK(r[2] - before[2] == doctest::Approx(20).epsilon(0.05));
    CHECK(r[3] - before[3] == doctest::Approx(-10).epsilon(0.05));

    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty()) {
        view.selectAnnotation(qs(jstr(changed.back(), "key")), false);
        view.viewport()->grab().save(dir + "/area-handles.png");
    }

    // an edge cannot be dragged past the opposite one
    double left = r[0], right = r[2], mid = 792 - (r[1] + r[3]) / 2;
    drag(view.viewport(), at(left, mid), at(right + 200, mid));
    r = box(changed.back());
    CHECK(r[0] < r[2]);
    CHECK(r[2] == doctest::Approx(right).epsilon(0.005));
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

    // pictures keep their colours on a dark page
    {
        TempDir pics;
        writeImagePdf(pics.path / "image.pdf");
        auto doc = pdf::Document::open(pics.path / "image.pdf");
        auto rendered = doc->render(0, 1);
        QImage page = QImage(rendered.pixels.data(), rendered.width, rendered.height, rendered.stride,
                             QImage::Format_RGB888)
                          .copy();
        darkenPage(page, doc->imageRects(0), 1, doc->pageSize(0));
        CHECK(page.pixelColor(122, 242) == QColor(255, 0, 0));  // the middle of the picture
        CHECK(page.pixelColor(400, 400) == darkPageColor());    // paper
        // an image as large as the page (a scan) is recoloured like text
        QImage scan(100, 100, QImage::Format_RGB888);
        scan.fill(Qt::white);
        darkenPage(scan, {pdf::Rect{0, 0, 100, 100}}, 1, pdf::Size{100, 100});
        CHECK(scan.pixelColor(50, 50) == darkPageColor());
    }

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
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
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

TEST_CASE("reader: annotation cards stay idle and follow the theme")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key");
    {
        ReaderTab tab(env.lib, qs(key));
        tab.resize(1200, 900);
        tab.show();
        QApplication::processEvents();
        auto* view = tab.view();
        view->setTool(ReaderTool::Highlight);
        for (int i = 0; i < 40; ++i)  // many annotations, like a well-read paper
            drag(view->viewport(), charCenter(*view, (i % 4) * 5), charCenter(*view, (i % 4) * 5 + 3));
        view->setTool(ReaderTool::Select);
        QTest::qWait(100);
    }
    REQUIRE(env.library.annotations(env.lib, key).size() == 40);

    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 900);
    tab.show();
    applyAppearance(Appearance::Dark);

    // nothing keeps restyling itself once the theme has changed (this used to loop
    // forever, freezing the reader and eventually running out of memory)
    struct Counter : QObject {
        int changes = 0;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() == QEvent::StyleChange || e->type() == QEvent::PaletteChange)
                ++changes;
            return false;
        }
    } counter;
    QTest::qWait(100);
    qApp->installEventFilter(&counter);
    QTest::qWait(300);
    qApp->removeEventFilter(&counter);
    CHECK(counter.changes == 0);

    // cards are painted in the current theme's colours
    QWidget* card = nullptr;
    for (auto* c : tab.findChildren<QWidget*>("annotationCard"))
        if (c->isVisible())
            card = c;
    REQUIRE(card);
    auto img = card->grab().toImage();
    QColor inside = img.pixelColor(img.width() - 4 * img.devicePixelRatio(), img.height() / 2);
    CHECK(inside == QApplication::palette().color(QPalette::Base));
    QColor edge = img.pixelColor(int(1.5 * img.devicePixelRatio()), img.height() / 2);
    CHECK(edge == QColor("#ffd400"));  // the annotation's colour

    applyAppearance(Appearance::System);
}

TEST_CASE("reader: the view state round-trips in Zotero's format")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView a;
    a.resize(800, 600);
    a.open(file);
    a.show();
    QApplication::processEvents();
    a.setZoom(2.5);
    a.horizontalScrollBar()->setValue(140);
    a.verticalScrollBar()->setValue(900);
    auto state = a.viewState();
    CHECK(state["pageIndex"] == 0);
    CHECK(state["scale"].get<double>() == doctest::Approx(250));
    // PDF coordinates: the viewport's top is 900 px down the page, i.e. this far up from the bottom
    CHECK(state["top"].get<double>() == doctest::Approx(792 - (900 - 24) / (2.5 * 96 / 72)).epsilon(0.02));

    PdfView b;
    b.resize(800, 600);
    b.open(file);
    b.show();
    QApplication::processEvents();
    b.restoreViewState(state);
    CHECK(b.zoom() == doctest::Approx(2.5));
    CHECK(std::abs(b.verticalScrollBar()->value() - 900) <= 2);
    CHECK(std::abs(b.horizontalScrollBar()->value() - 140) <= 2);

    // fitted zoom is kept as a mode, as Zotero does
    a.fitPage();
    CHECK(a.viewState()["scale"] == "page-fit");
    b.restoreViewState(Json{{"pageIndex", 0}, {"scale", "page-width"}});
    CHECK(b.isFitWidth());
}

TEST_CASE("reader: pinching on a touchpad zooms, keeping the point under the fingers")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView view, expected;
    for (auto* v : {&view, &expected}) {
        v->resize(800, 600);
        v->open(file);
        v->show();
        QApplication::processEvents();
        v->setZoom(1);
    }
    // macOS (and Wayland) deliver a touchpad pinch as a native zoom gesture, not as Ctrl+wheel
    QPointF fingers(300, 200);
    auto pinch = [&](double value) {
        QNativeGestureEvent e(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, fingers, fingers,
                              view.viewport()->mapToGlobal(fingers), value, {});
        QApplication::sendEvent(view.viewport(), &e);
        return e.isAccepted();
    };
    CHECK(pinch(0.5));
    CHECK(view.zoom() == doctest::Approx(1.5));
    expected.setZoom(1.5, fingers);
    CHECK(view.horizontalScrollBar()->value() == expected.horizontalScrollBar()->value());
    CHECK(view.verticalScrollBar()->value() == expected.verticalScrollBar()->value());
    pinch(-0.2);
    CHECK(view.zoom() == doctest::Approx(1.2));
}

TEST_CASE("reader: a document opens where it was left")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key");
    int scrolled = 0;
    {
        ReaderTab tab(env.lib, qs(key));
        tab.resize(1200, 900);
        tab.show();
        QApplication::processEvents();
        tab.view()->setZoom(3);
        tab.view()->verticalScrollBar()->setValue(1500);
        scrolled = tab.view()->verticalScrollBar()->value();
        REQUIRE(scrolled > 1000);
    }  // closing the tab saves where it was
    auto state = env.library.readerState(key);
    CHECK(state["scale"].get<double>() == doctest::Approx(300));
    CHECK(fs::exists(env.library.storageDir() / key / ".zotero-reader-state"));

    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 900);
    tab.show();
    QApplication::processEvents();
    QApplication::processEvents();
    CHECK(tab.view()->zoom() == doctest::Approx(3));
    CHECK(std::abs(tab.view()->verticalScrollBar()->value() - scrolled) <= 2);
}

TEST_CASE("main window: open tabs and the library view come back after a restart")
{
    app();
    Env env;
    TempDir tmp;
    QStringList keys;
    QString paper;
    for (auto name : {"first", "second"}) {
        auto file = tmp.path / (std::string(name) + ".pdf");
        writeSamplePdf(file);
        auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", name}});
        keys << qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key"));
        paper = qs(jstr(parent, "key"));
    }
    {
        MainWindow window;
        window.resize(1400, 900);
        window.show();
        QApplication::processEvents();
        window.selectItem(paper);  // "second" in the library tab
        window.openAttachment(env.lib, keys[0]);
        window.openAttachment(env.lib, keys[1]);
        auto* tabs = window.findChild<QTabWidget*>("mainTabs");
        REQUIRE(tabs->count() == 3);
        tabs->setCurrentIndex(1);  // "first" is the tab being read
        window.close();
    }
    CHECK(env.config.get("/ui/session")["tabs"].size() == 2);

    MainWindow window;
    window.resize(1400, 900);
    window.show();
    QApplication::processEvents();
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    REQUIRE(tabs->count() == 3);
    CHECK(tabs->currentIndex() == 1);
    auto* first = qobject_cast<ReaderTab*>(tabs->widget(1));
    REQUIRE(first);
    CHECK(first->attachmentKey() == keys[0]);
    CHECK(tabs->tabText(2) == "second");
    // the other tab loads only when it is shown
    CHECK(qobject_cast<ReaderTab*>(tabs->widget(2)) == nullptr);
    tabs->setCurrentIndex(2);
    auto* second = qobject_cast<ReaderTab*>(tabs->widget(2));
    REQUIRE(second);
    CHECK(second->attachmentKey() == keys[1]);
    CHECK(tabs->currentIndex() == 2);
    // and the library tab has the same item selected
    auto* library = window.findChild<LibraryView*>();
    REQUIRE(library);
    CHECK(library->selectedKeys() == QStringList{paper});
}

TEST_CASE("reader: Ctrl+Z / Ctrl+Y undo and redo annotation changes, per tab")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key");
    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 900);
    tab.show();
    tab.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&tab));
    auto* view = tab.view();
    view->setFocus();
    QApplication::processEvents();
    auto anns = [&] { return env.library.annotations(env.lib, key); };
    auto undo = [&] { QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier); };
    auto redo = [&] { QTest::keyClick(view, Qt::Key_Y, Qt::ControlModifier); };

    view->setTool(ReaderTool::Highlight);
    drag(view->viewport(), charCenter(*view, 0), charCenter(*view, 4));
    view->setTool(ReaderTool::Select);
    REQUIRE(anns().size() == 1);
    auto annKey = jstr(anns()[0], "key");
    undo();
    CHECK(anns().empty());
    redo();
    REQUIRE(anns().size() == 1);
    CHECK(jstr(anns()[0], "key") == annKey);  // the same annotation comes back

    // a change, then undo puts the old value back
    Json changed = anns()[0];
    changed["annotationColor"] = "#ff6666";
    emit view->annotationChanged(changed);
    CHECK(jstr(anns()[0], "annotationColor") == "#ff6666");
    undo();
    CHECK(jstr(anns()[0], "annotationColor") == "#ffd400");
    redo();
    CHECK(jstr(anns()[0], "annotationColor") == "#ff6666");

    // typing a comment is one step
    for (auto text : {"n", "ni", "nic", "nice"}) {
        Json c = anns()[0];
        c["annotationComment"] = text;
        emit view->annotationChanged(c);
    }
    CHECK(jstr(anns()[0], "annotationComment") == "nice");
    undo();
    CHECK(jstr(anns()[0], "annotationComment").empty());
    CHECK(jstr(anns()[0], "annotationColor") == "#ff6666");
    redo();

    // deleting, and getting it back
    view->selectAnnotation(qs(annKey), false);
    QTest::keyClick(view, Qt::Key_Delete);
    CHECK(anns().empty());
    undo();
    REQUIRE(anns().size() == 1);
    CHECK(jstr(anns()[0], "key") == annKey);
    CHECK(jstr(anns()[0], "annotationComment") == "nice");

    // at most 50 steps are kept
    for (int i = 0; i < 60; ++i) {
        Json c = anns()[0];
        c["annotationColor"] = i % 2 ? "#5fb236" : "#2ea8e5";
        emit view->annotationChanged(c);
    }
    auto* stack = tab.findChild<QUndoStack*>();
    REQUIRE(stack);
    CHECK(stack->count() == 50);

    // in a text field, Ctrl+Z is the field's own
    auto* note = tab.findChild<NoteTextEdit*>();
    REQUIRE(note);
    note->setFocus();
    int before = stack->index();
    QTest::keyClick(note, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(note, Qt::Key_Y, Qt::ControlModifier);
    CHECK(stack->index() == before);
}

TEST_CASE("reader: a hidden tab gives back its memory and comes back when shown")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    QTabWidget tabs;
    tabs.resize(900, 900);
    auto* view = new PdfView;
    view->open(file);
    tabs.addTab(view, "pdf");
    tabs.addTab(new QWidget, "other");
    tabs.show();
    view->setReleaseDelays(100, 300);
    CHECK(QTest::qWaitFor([&] { return view->renderedPages() > 0; }));
    view->setZoom(2);
    view->verticalScrollBar()->setValue(300);
    int scrolled = view->verticalScrollBar()->value();

    tabs.setCurrentIndex(1);  // hidden: first the rendered pages go, then the render thread
    CHECK(QTest::qWaitFor([&] { return view->renderedPages() == 0; }, 2000));
    CHECK(view->isRendering());
    CHECK(QTest::qWaitFor([&] { return !view->isRendering(); }, 2000));

    tabs.setCurrentIndex(0);  // shown again: rendering resumes where it was
    CHECK(QTest::qWaitFor([&] { return view->isRendering() && view->renderedPages() > 0; }, 3000));
    CHECK(view->verticalScrollBar()->value() == scrolled);
    CHECK(view->zoom() == doctest::Approx(2));
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
    if (qEnvironmentVariableIsSet("OURNOTES_TEST_SNAPSHOT_DIR"))
        dlg.grab().save(qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR") + "/preferences.png");
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
    auto snapshots = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR");
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
    auto snapshots = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR");
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
    auto snapshots = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR");
    pane.showItems(env.lib, {qs(key)});
    QApplication::processEvents();
    if (!snapshots.isEmpty())
        pane.grab().save(snapshots + "/item-pane-notes.png");
}

namespace {

// Answers a confirmation dialog (by object name) once it shows up: with its
// destructive button, or Cancel. *text gets its question.
void answerConfirmation(const QString& name, bool confirm, QString* text)
{
    auto* timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, [timer, name, confirm, text] {
        for (auto* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (!box || !box->isVisible() || box->objectName() != name)
                continue;
            *text = box->text();
            if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
                box->grab().save(dir + "/" + name + ".png");
            for (auto* b : box->buttons())
                if ((box->buttonRole(b) == QMessageBox::DestructiveRole) == confirm) {
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

void answerTagConfirmation(bool remove, QString* text) { answerConfirmation("confirmRemoveTag", remove, text); }

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
    auto snapshots = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR");
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

    // a formula typed in place is one undo step: Ctrl+Z gives back the "$$" typed,
    // Ctrl+Y the formula
    edit.clear();
    QTest::keyClicks(&edit, "a $$x$");
    REQUIRE(countMath(edit.document()) == 1);
    QTest::keyClick(&edit, Qt::Key_Z, Qt::ControlModifier);
    CHECK(countMath(edit.document()) == 0);
    CHECK(edit.toPlainText() == "a $$");
    QTest::keyClick(&edit, Qt::Key_Y, Qt::ControlModifier);
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"x"});

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
    // the display formula and the new line after it go back together
    QTest::keyClick(&edit, Qt::Key_Z, Qt::ControlModifier);  // "next"
    QTest::keyClick(&edit, Qt::Key_Z, Qt::ControlModifier);
    CHECK(countMath(edit.document()) == 0);
    CHECK(edit.document()->blockCount() == 1);
    CHECK(edit.toPlainText() == "$$");
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

    // changing a formula is one undo step; opening one without changing it is none
    QTest::keyClick(&edit, Qt::Key_Z, Qt::ControlModifier);
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"x^2"});
    QTest::keyClick(&edit, Qt::Key_Y, Qt::ControlModifier);
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"x^2+1"});

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

    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
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
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        dlg.grab().save(dir + "/preferences-dark.png");
    combo->setCurrentIndex(combo->findData("system"));
    CHECK(appearanceFromConfig(env.config) == Appearance::System);
}

TEST_CASE("notes: the toolbar wraps instead of hiding buttons")
{
    app();
    Env env;
    NoteEditor editor;
    editor.resize(230, 400);
    editor.show();
    QApplication::processEvents();
    auto* bar = editor.findChild<WrapToolBar*>();
    REQUIRE(bar);
    QSet<int> rows;
    int buttons = 0;
    for (auto* b : bar->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!b->isVisible())
            continue;
        ++buttons;
        rows.insert(b->y());
        CHECK(b->geometry().right() < bar->width());  // nothing sticks out or is cut off
    }
    CHECK(buttons >= 12);  // style, B I U S, lists, quote, link, formula, colour
    CHECK(rows.size() >= 2);
    CHECK(editor.findChild<QToolBar*>() == nullptr);
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        editor.grab().save(dir + "/note-toolbar-wrapped.png");
}

TEST_CASE("notes: a note can be moved to the trash from the list, after asking")
{
    app();
    Env env;
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto note = env.library.createNote(env.lib, "<p>Idea: preparation</p>", jstr(paper, "key"));
    ItemNotesPanel panel;
    panel.resize(400, 600);
    panel.show();
    panel.setItem(env.lib, qs(jstr(paper, "key")));
    QApplication::processEvents();
    auto* list = panel.findChild<QListWidget*>();
    REQUIRE(list);
    REQUIRE(list->count() == 1);
    list->setCurrentRow(0);
    list->setFocus();
    auto trashed = [&] { return jbool(*env.library.item(env.lib, jstr(note, "key")), "deleted"); };

    QString question;
    answerConfirmation("confirmTrashNote", false, &question);
    QTest::keyClick(list, Qt::Key_Delete);
    REQUIRE(QTest::qWaitFor([&] { return !question.isEmpty(); }));
    CHECK(question.contains("Idea: preparation"));
    CHECK(!trashed());

    question.clear();
    answerConfirmation("confirmTrashNote", true, &question);
    QTest::keyClick(list, Qt::Key_Delete);
    REQUIRE(QTest::qWaitFor([&] { return !question.isEmpty(); }));
    CHECK(trashed());
    CHECK(QTest::qWaitFor([&] { return list->count() == 0; }));
}

TEST_CASE("main window: many tabs wrap onto more rows")
{
    app();
    Env env;
    TempDir tmp;
    MainWindow window;
    window.resize(900, 700);
    window.show();
    QApplication::processEvents();
    for (int i = 0; i < 8; ++i) {
        auto file = tmp.path / ("paper" + std::to_string(i) + ".pdf");
        writeSamplePdf(file);
        auto parent = env.library.createItem(
            env.lib, Json{{"itemType", "journalArticle"}, {"title", "A rather long paper title number " + std::to_string(i)}});
        window.openAttachment(env.lib, qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key")));
    }
    QApplication::processEvents();
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    REQUIRE(tabs->count() == 9);
    CHECK(!tabs->tabBar()->isVisible());
    auto* strip = window.findChild<QWidget*>("tabStrip");
    REQUIRE(strip);
    auto buttons = strip->findChildren<QWidget*>("tabButton");
    REQUIRE(buttons.size() == 9);
    QSet<int> rows;
    for (auto* b : buttons) {
        CHECK(b->isVisible());
        rows.insert(b->y());
        CHECK(b->geometry().right() < strip->width());
    }
    CHECK(rows.size() >= 2);  // wrapped, not squeezed into one row
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        window.grab().save(dir + "/tabs-wrapped.png");

    // clicking a tab shows it; middle-clicking closes it; the library tab cannot be closed
    auto buttonFor = [&](int index) -> QWidget* {
        auto list = strip->findChildren<QWidget*>("tabButton");
        std::sort(list.begin(), list.end(), [](QWidget* a, QWidget* b) {
            return a->y() != b->y() ? a->y() < b->y() : a->x() < b->x();
        });
        return list.value(index);
    };
    QTest::mouseClick(buttonFor(3), Qt::LeftButton, {}, QPoint(20, buttonFor(3)->height() / 2));
    CHECK(tabs->currentIndex() == 3);
    auto title = tabs->tabText(3);
    QTest::mouseClick(buttonFor(3), Qt::MiddleButton, {}, QPoint(20, 5));
    QApplication::processEvents();
    CHECK(tabs->count() == 8);
    CHECK(tabs->tabText(3) != title);
    QTest::mouseClick(buttonFor(0), Qt::MiddleButton, {}, QPoint(20, 5));
    QApplication::processEvents();
    CHECK(tabs->count() == 8);
    CHECK(tabs->tabText(0) == "Library");

    // dragging a tab onto another moves it
    auto moved = tabs->tabText(5);
    auto* from = buttonFor(5);
    auto* to = buttonFor(2);
    QTest::mousePress(from, Qt::LeftButton, {}, QPoint(20, 10));
    QTest::mouseMove(from, from->mapFromGlobal(to->mapToGlobal(QPoint(20, 10))));
    QTest::mouseRelease(from, Qt::LeftButton, {}, from->mapFromGlobal(to->mapToGlobal(QPoint(20, 10))));
    QApplication::processEvents();
    CHECK(tabs->tabText(2) == moved);
}

TEST_CASE("main window: tabs wrapping onto more rows never resize the window, which fits the screen")
{
    app();
    Env env;
    TempDir tmp;
    MainWindow window;
    window.resize(900, 600);
    window.show();
    QApplication::processEvents();
    QSize size = window.size();
    QSize minimum;  // with reader tabs open
    auto* strip = window.findChild<QWidget*>("tabStrip");
    REQUIRE(strip);
    int oneRow = strip->height();
    for (int i = 0; i < 14; ++i) {
        auto file = tmp.path / ("paper" + std::to_string(i) + ".pdf");
        writeSamplePdf(file);
        auto parent = env.library.createItem(
            env.lib, Json{{"itemType", "journalArticle"}, {"title", "A rather long paper title number " + std::to_string(i)}});
        window.openAttachment(env.lib, qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key")));
        QApplication::processEvents();
        QApplication::processEvents();
        // a wrapping layout asked for its size at no width puts every tab on a row of its
        // own: the window's minimum grew with each tab, and the window with it
        CHECK(window.size() == size);
        if (i == 0)
            minimum = window.minimumSize();
        CHECK(window.minimumSize() == minimum);
    }
    CHECK(strip->height() > 2 * oneRow);  // the tabs did wrap, below them got smaller

    // so the window still fits the screen: maximized, and full screen (which macOS refuses
    // to a window whose minimum size is larger than the screen)
    QRect available = window.screen()->availableGeometry();
    CHECK(window.minimumWidth() <= available.width());
    CHECK(window.minimumHeight() <= available.height());
    window.showMaximized();
    CHECK(QTest::qWaitFor([&] { return window.isMaximized(); }, 3000));
    QApplication::processEvents();
    CHECK(window.frameGeometry().width() <= available.width());
    CHECK(window.frameGeometry().height() <= available.height());
    window.showFullScreen();
    // (on a MacBook with a notch, full screen leaves out the strip beside the camera)
    CHECK(QTest::qWaitFor(
        [&] {
            return window.isFullScreen() && window.width() == window.screen()->geometry().width() &&
                   window.height() >= available.height();
        },
        5000));
    window.showNormal();
    CHECK(QTest::qWaitFor([&] { return !window.isFullScreen(); }, 5000));
}

TEST_CASE("main window: Control-Command-F (F11 elsewhere) enters and leaves full screen")
{
    app();
    Env env;
    MainWindow window;
    window.resize(900, 600);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
#ifdef Q_OS_MACOS
    QKeySequence key(Qt::CTRL | Qt::META | Qt::Key_F);  // Qt's Ctrl is Command, its Meta is Control
#else
    QKeySequence key = QKeySequence::keyBindings(QKeySequence::FullScreen).value(0);
    REQUIRE(!key.isEmpty());
#endif
    QTest::keySequence(&window, key);
    CHECK(QTest::qWaitFor([&] { return window.isFullScreen(); }, 5000));
    QTest::keySequence(&window, key);
    CHECK(QTest::qWaitFor([&] { return !window.isFullScreen(); }, 5000));
}

TEST_CASE("main window: Control-Tab and Control-Shift-Tab switch tabs")
{
    app();
    Env env;
    TempDir tmp;
    MainWindow window;
    window.resize(1200, 800);
    window.show();
    for (int i = 0; i < 2; ++i) {
        auto file = tmp.path / ("paper" + std::to_string(i) + ".pdf");
        writeSamplePdf(file);
        auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
        window.openAttachment(env.lib, qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key")));
    }
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    REQUIRE(tabs->count() == 3);
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
#ifdef Q_OS_MACOS
    auto control = Qt::MetaModifier;  // the Control key; Qt's Ctrl is Command, and Command-Tab switches applications
#else
    auto control = Qt::ControlModifier;
#endif
    QTest::keyClick(&window, Qt::Key_Tab, control);
    CHECK(tabs->currentIndex() == 1);
    QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &window, Qt::Key_Tab, control);
    CHECK(tabs->currentIndex() == 2);
    QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &window, Qt::Key_Backtab,
                    control | Qt::ShiftModifier);
    CHECK(tabs->currentIndex() == 1);
}

TEST_CASE("library: columns fill the list, the title column gives way, the item pane can be hidden")
{
    app();
    Env env;
    for (int i = 0; i < 5; ++i)
        env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper " + std::to_string(i)}});
    LibraryView view;
    view.resize(1500, 800);
    view.show();
    QApplication::processEvents();
    auto* items = view.itemsView();
    auto* header = items->header();
    CHECK(items->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    auto total = [&] {
        int sum = 0;
        for (int c = 0; c < header->count(); ++c)
            if (!header->isSectionHidden(c))
                sum += header->sectionSize(c);
        return sum;
    };
    auto otherWidths = [&] {
        std::vector<int> w;
        for (int c = 1; c < header->count(); ++c)
            w.push_back(header->isSectionHidden(c) ? 0 : header->sectionSize(c));
        return w;
    };
    // the last column ends where the list ends
    CHECK(total() == items->viewport()->width());

    QSplitter* splitter = nullptr;
    for (auto* sp : view.findChildren<QSplitter*>())
        if (sp->orientation() == Qt::Horizontal && sp->count() == 3)
            splitter = sp;
    REQUIRE(splitter);
    auto before = otherWidths();
    int title = header->sectionSize(ItemsModel::Title);
    // a wider item pane squeezes only the title column
    splitter->setSizes({240, 700, 560});
    QApplication::processEvents();
    CHECK(total() == items->viewport()->width());
    CHECK(otherWidths() == before);
    CHECK(header->sectionSize(ItemsModel::Title) < title);
    // the list's minimum is not the window's: squeezed by a narrow window, the title
    // column gives way further (dragging the pane's edge stops at 180, below)
    CHECK(view.minimumSizeHint().width() < 700);

    // dragging the item pane's edge far to the left stops at the list's minimum and
    // leaves the collections alone
    {
        int left = splitter->sizes()[0];
        auto* handle = splitter->handle(2);
        QPoint start(handle->width() / 2, handle->height() / 2);
        QTest::mousePress(handle, Qt::LeftButton, {}, start);
        QTest::mouseMove(handle, start - QPoint(300, 0));
        QTest::mouseMove(handle, start - QPoint(1200, 0));
        QTest::mouseRelease(handle, Qt::LeftButton, {}, start - QPoint(1200, 0));
        QApplication::processEvents();
        CHECK(splitter->sizes()[0] == left);
        CHECK(header->sectionSize(ItemsModel::Title) >= 180);
        CHECK(total() == items->viewport()->width());
        // and the collections can still be resized with their own edge
        CHECK(splitter->widget(0)->minimumWidth() < left);
        splitter->setSizes({240, 760, 500});
        QApplication::processEvents();
    }

    // dragging the title's edge trades width with the next column
    int creator = header->sectionSize(ItemsModel::Creator);
    header->resizeSection(ItemsModel::Title, header->sectionSize(ItemsModel::Title) + 40);
    QApplication::processEvents();
    CHECK(header->sectionSize(ItemsModel::Creator) == creator - 40);
    CHECK(total() == items->viewport()->width());

    // hidden item pane: the list reaches the right edge
    auto* toggle = view.findChild<QToolButton*>("itemPaneToggle");
    REQUIRE(toggle);
    CHECK(toggle->isChecked());
    toggle->click();
    QApplication::processEvents();
    CHECK(!view.isItemPaneVisible());
    CHECK(items->mapTo(&view, QPoint(items->width(), 0)).x() == view.width());
    CHECK(total() == items->viewport()->width());
    CHECK(env.config.get("/ui/itemPaneVisible").get<bool>() == false);
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        view.grab().save(dir + "/library-pane-hidden.png");
    toggle->click();
    QApplication::processEvents();
    CHECK(view.isItemPaneVisible());
    CHECK(total() == items->viewport()->width());
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        view.grab().save(dir + "/library-pane-shown.png");
}

TEST_CASE("item pane: the top shows where the item is filed, with links")
{
    app();
    Env env;
    auto& lib = env.library;
    auto robotics = jstr(lib.createCollection(env.lib, "Robotics"), "key");
    auto humanoid = jstr(lib.createCollection(env.lib, "Humanoid", robotics), "key");
    auto mimic = jstr(lib.createCollection(env.lib, "MimicKit", humanoid), "key");
    auto reading = jstr(lib.createCollection(env.lib, "Reading"), "key");
    auto paper = jstr(lib.createItem(env.lib, Json{{"itemType", "journalArticle"},
                                                   {"title", "Filed twice"},
                                                   {"collections", Json::array({mimic, reading})}}),
                      "key");
    auto note = jstr(lib.createNote(env.lib, "<p>About it</p>", paper), "key");
    auto loose = jstr(lib.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Unfiled"}}), "key");
    auto gone = jstr(lib.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Gone"}}), "key");
    lib.trashItems(env.lib, {gone});

    LibraryView view;
    view.resize(1500, 800);
    view.show();
    QApplication::processEvents();
    auto location = [&]() -> LocationBox* {
        for (auto* l : view.findChildren<LocationBox*>())
            if (l->isVisible())
                return l;
        return nullptr;
    };
    auto pathsOf = [&](const QString& key) {
        view.selectItem(key, env.lib);
        QApplication::processEvents();
        auto* l = location();
        return l ? l->paths() : QStringList{};
    };
    QString sep = " › ";
    CHECK(pathsOf(qs(paper)) == QStringList{"My Library" + sep + "Reading",
                                            "My Library" + sep + "Robotics" + sep + "Humanoid" + sep + "MimicKit"});
    CHECK(pathsOf(qs(note)) == pathsOf(qs(paper)));  // a note is where its item is
    CHECK(pathsOf(qs(loose)) == QStringList{"My Library" + sep + "Unfiled Items"});
    CHECK(pathsOf(qs(gone)) == QStringList{"My Library" + sep + "Trash"});

    // renaming a collection shows at once
    auto h = *lib.collection(env.lib, humanoid);
    h["name"] = "Humanoids";
    lib.saveCollection(env.lib, h);
    auto paths = pathsOf(qs(paper));
    CHECK(paths.contains("My Library" + sep + "Robotics" + sep + "Humanoids" + sep + "MimicKit"));
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        location()->grab().save(dir + "/item-location.png");

    // a part of the path shows that collection, with the item selected in it
    QLabel* line = nullptr;
    for (auto* label : location()->findChildren<QLabel*>())
        if (label->text().contains("MimicKit"))
            line = label;
    REQUIRE(line);
    emit line->linkActivated("collection:" + qs(mimic));
    QApplication::processEvents();
    CHECK(view.scope().kind == ScopeKind::Collection);
    CHECK(view.scope().collectionKey == mimic);
    CHECK(view.selectedKeys() == QStringList{qs(paper)});
    // a parent collection, where the item is not listed itself, is simply shown
    line = nullptr;
    for (auto* label : location()->findChildren<QLabel*>())
        if (label->isVisible() && label->text().contains("MimicKit"))
            line = label;
    REQUIRE(line);
    emit line->linkActivated("collection:" + qs(humanoid));
    QApplication::processEvents();
    CHECK(view.scope().kind == ScopeKind::Collection);
    CHECK(view.scope().collectionKey == humanoid);
}

TEST_CASE("main window: Enter on several items opens them top to bottom")
{
    app();
    Env env;
    TempDir tmp;
    QStringList papers;
    for (auto title : {"C third", "A first", "B second"}) {  // listed sorted by title: A, B, C
        auto file = tmp.path / (std::string(title) + ".pdf");
        writeSamplePdf(file);
        auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", title}});
        env.library.importAttachment(env.lib, file, jstr(parent, "key"));
        papers << qs(jstr(parent, "key"));
    }
    MainWindow window;
    window.resize(1400, 900);
    window.show();
    QApplication::processEvents();
    auto* library = window.findChild<LibraryView*>();
    REQUIRE(library);
    auto* items = library->itemsView();
    // selected in the order C, A, B
    auto* model = items->selectionModel();
    for (int i = 0; i < papers.size(); ++i) {
        auto idx = static_cast<ItemsModel*>(items->model())->indexOfKey(papers[i]);
        REQUIRE(idx.isValid());
        model->select(idx, (i == 0 ? QItemSelectionModel::ClearAndSelect : QItemSelectionModel::Select) |
                               QItemSelectionModel::Rows);
    }
    REQUIRE(library->selectedKeys().size() == 3);
    QTest::keyClick(items, Qt::Key_Return);
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    REQUIRE(tabs->count() == 4);
    CHECK(tabs->tabText(1) == "A first");
    CHECK(tabs->tabText(2) == "B second");
    CHECK(tabs->tabText(3) == "C third");
    CHECK(tabs->currentIndex() == 1);  // the first is shown
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    doctest::Context context(argc, argv);
    return context.run();
}
