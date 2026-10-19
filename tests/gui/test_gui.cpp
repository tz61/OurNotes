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
#include <QProxyStyle>
#include <QTreeView>
#include <QUndoStack>
#include <QTreeWidget>

#include <array>
#include <future>
#include <httplib.h>
#include <atomic>
#include <QNativeGestureEvent>
#include <QTest>
#include <QTextDocument>
#include <QMimeData>
#include <QPushButton>
#include <QMenu>
#include <QContextMenuEvent>
#include <QClipboard>
#include <QDropEvent>
#include <QPainter>
#include <QWheelEvent>

#include "../core/test_helpers.h"
#include "../pdf/sample_pdf.h"
#include "core/config.h"
#include "core/library.h"
#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/collection_tree.h"
#include "gui/common.h"
#include "gui/connector_server.h"
#include "gui/fancy_progress.h"
#include "gui/flow_layout.h"
#include "gui/item_pane.h"
#include "gui/library_view.h"
#include "gui/main_window.h"
#include "gui/items_model.h"
#include "gui/metadata_progress.h"
#include "gui/math_render.h"
#include "gui/note_editor.h"
#include "gui/note_html.h"
#include "gui/rich_text_images.h"
#include "gui/shift_wheel.h"
#include "gui/preferences.h"
#include "gui/sync_controller.h"
#include "gui/sync_history.h"
#include "gui/theme.h"
#include "gui/ui_scale.h"
#include "gui/wrap_toolbar.h"
#include "gui/reader/comment_bubble.h"
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

TEST_CASE("reader: the paper's top note opens with the PDF")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key");
    {
        ReaderTab tab(env.lib, qs(key));  // no notes: nothing open
        REQUIRE(tab.findChild<NoteEditor*>());
        CHECK(tab.findChild<NoteEditor*>()->key().isEmpty());
    }
    env.library.createNote(env.lib, "<p>First thoughts</p>", jstr(parent, "key"));
    env.library.createNote(env.lib, "<p>Second thoughts</p>", jstr(parent, "key"));
    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 800);
    tab.show();
    QApplication::processEvents();
    QListWidget* notes = nullptr;
    for (auto* l : tab.findChildren<QListWidget*>())
        if (l->count() == 2)
            notes = l;
    REQUIRE(notes);
    auto* editor = tab.findChild<NoteEditor*>();
    CHECK(notes->currentRow() == 0);
    CHECK(editor->key() == notes->item(0)->data(Qt::UserRole).toString());
    CHECK(!tab.findChild<NoteTextEdit*>()->hasFocus());  // the PDF keeps the keyboard
}

TEST_CASE("reader: page thumbnails stay in a grid while their pictures arrive")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "pages.pdf";
    writeManyPagesPdf(file, 12);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key");
    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 900);
    tab.show();
    QApplication::processEvents();
    int rendered = 0;
    QObject::connect(tab.view(), &PdfView::thumbnailRendered, [&] { ++rendered; });
    QToolButton* show = nullptr;
    for (auto* b : tab.findChildren<QToolButton*>())
        if (b->defaultAction() && b->defaultAction()->text() == "Thumbnails")
            show = b;
    REQUIRE(show);
    show->click();
    QListWidget* list = nullptr;
    for (auto* l : tab.findChildren<QListWidget*>())
        if (l->viewMode() == QListView::IconMode)
            list = l;
    REQUIRE(list);
    REQUIRE(list->count() == 12);
    REQUIRE(QTest::qWaitFor([&] { return rendered == 12; }, 5000));
    QApplication::processEvents();

    // without a resize, no two cells overlap and each has room for a picture
    for (int i = 0; i < list->count(); ++i) {
        QRect a = list->visualRect(list->model()->index(i, 0));
        CHECK(a.width() >= 120);
        CHECK(a.height() >= 160);
        for (int j = i + 1; j < list->count(); ++j)
            CHECK_FALSE(a.intersects(list->visualRect(list->model()->index(j, 0))));
    }
}

TEST_CASE("Shift+wheel scrolls sideways")
{
    app();
    ShiftWheelScrolling::install();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView view;
    view.resize(500, 600);
    view.open(file);
    view.show();
    view.setZoom(3);  // wider than the view
    QApplication::processEvents();
    auto* h = view.horizontalScrollBar();
    auto* v = view.verticalScrollBar();
    REQUIRE(h->maximum() > 0);
    h->setValue(0);
    v->setValue(0);
    auto wheel = [&](int dy, Qt::KeyboardModifiers mods) {
        QPointF at(200, 200);
        QWheelEvent e(at, view.viewport()->mapToGlobal(at), {}, QPoint(0, dy), Qt::NoButton, mods, Qt::NoScrollPhase,
                      false);
        QApplication::sendEvent(view.viewport(), &e);
    };
    wheel(-120, Qt::ShiftModifier);  // down: to the right
    CHECK(h->value() > 0);
    CHECK(v->value() == 0);
    int right = h->value();
    wheel(120, Qt::ShiftModifier);
    CHECK(h->value() < right);
    wheel(-120, Qt::NoModifier);  // without Shift, down as before
    CHECK(v->value() > 0);
}

TEST_CASE("pdf view: zooming deep into a long document keeps the pages in view")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "long.pdf";
    writeManyPagesPdf(file, 300);
    PdfView view;
    view.resize(900, 900);
    view.open(file);
    view.show();
    view.setDarkPages(false);
    QApplication::processEvents();
    // text on screen: the pages' rendered images, not blank paper
    auto inkShown = [&] {
        auto img = view.viewport()->grab().toImage().convertToFormat(QImage::Format_Grayscale8);
        int ink = 0;
        for (int y = 0; y < img.height(); y += 2)
            for (int x = 0; x < img.width(); x += 2)
                ink += qGray(img.pixel(x, y)) < 80;
        return ink;
    };
    for (int page : {2, 250}) {
        view.goToPage(page);
        REQUIRE(QTest::qWaitFor([&] { return inkShown() > 50; }, 10000));
        QPointF centre(view.viewport()->width() / 2.0, view.viewport()->height() / 2.0);
        for (double factor : {1.25, 0.8, 0.8, 1.6}) {
            view.setZoom(view.zoom() * factor, centre);
            // drawn before any new render can arrive: the old images, scaled
            INFO("page " << page << ", zoom x" << factor);
            CHECK(inkShown() > 20);
            CHECK(std::abs(view.currentPage() - page) <= 1);
            REQUIRE(QTest::qWaitFor([&] { return inkShown() > 50; }, 10000));
        }
    }
}

TEST_CASE("pdf view: pages are drawn pixel for pixel, not resampled")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView view;
    view.resize(801, 700);
    view.open(file);
    view.show();
    QPointF origin;
    for (double zoom : {0.71, 0.73, 0.77, 0.79, 0.83}) {  // a centred page whose edges fall between pixels
        view.setZoom(zoom);
        QApplication::processEvents();
        origin = view.pageToViewport(0, {0, 0});
        if (std::abs(origin.x() - std::round(origin.x())) > 0.2)
            break;
    }
    REQUIRE(std::abs(origin.x() - std::round(origin.x())) > 0.2);
    double scale = view.pageToViewport(0, {1, 0}).x() - origin.x();
    double dpr = view.devicePixelRatioF();
    auto r = view.document()->render(0, (float)(scale * dpr));
    QImage page = QImage(r.pixels.data(), r.width, r.height, r.stride, QImage::Format_RGB888).copy();
    QPoint at(std::lround(origin.x() * dpr), std::lround(origin.y() * dpr));

    int differing = -1;
    auto compare = [&] {
        auto shot = view.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB888);
        int w = std::min(page.width(), shot.width() - at.x()), h = std::min(page.height(), shot.height() - at.y());
        differing = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                differing += shot.pixel(at.x() + x, at.y() + y) != page.pixel(x, y);
        return differing == 0;
    };
    CHECK(QTest::qWaitFor(compare, 3000));
    CHECK(differing == 0);
}

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

TEST_CASE("comments: plain text with formulas, to the editor and back")
{
    app();
    for (QString comment : {QString("a $x_y$ b"), QString("first line\n$$\\sum_i x_i$$\nlast"),
                            QString("costs $5 and $10"), QString("no formula"), QString()}) {
        NoteTextEdit edit;
        edit.setHtml(commentHtmlForEditor(comment));
        edit.prepareDocument();
        CHECK(documentToComment(edit.document()) == comment);
    }
    NoteTextEdit edit;
    edit.setHtml(commentHtmlForEditor("a $x_y$ b, costs $5 and $10"));
    CHECK(countMath(edit.document()) == 1);
}

TEST_CASE("comments: shown over the page on hover, edited in place after a click")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView view;
    view.resize(800, 900);
    view.open(file);
    view.show();
    QApplication::processEvents();
    Json highlight;
    QObject::connect(&view, &PdfView::annotationCreated, [&](const Json& d) { highlight = d; });
    view.setTool(ReaderTool::Highlight);
    drag(view.viewport(), charCenter(view, 0), charCenter(view, 10));  // "Hello World"
    REQUIRE(highlight.is_object());
    view.setTool(ReaderTool::Select);
    highlight["annotationComment"] = "Energy $E=mc^2$ here";
    view.setAnnotations({highlight});
    std::vector<Json> changed;
    QObject::connect(&view, &PdfView::annotationChanged, [&](const Json& d) { changed.push_back(d); });
    auto* bubble = view.commentBubble();
    REQUIRE(bubble);
    QPoint on = charCenter(view, 4).toPoint(), off = view.pageToViewport(0, {300, 400}).toPoint();

    // resting on it shows the comment, as small as its text, below the highlight
    QTest::mouseMove(view.viewport(), off);
    QTest::mouseMove(view.viewport(), on);
    REQUIRE(QTest::qWaitFor([&] { return bubble->isVisible(); }, 2000));
    CHECK(!bubble->isEditing());
    CHECK(bubble->editor()->isReadOnly());
    CHECK(countMath(bubble->editor()->document()) == 1);
    CHECK(bubble->width() < 300);
    CHECK(bubble->y() > on.y());
    auto snapshot = [&](const QString& name) {
        if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
            view.grab().save(dir + "/" + name + ".png");
    };
    snapshot("comment-shown");
    QTest::mouseMove(view.viewport(), off);
    CHECK(QTest::qWaitFor([&] { return !bubble->isVisible(); }, 2000));

    // a click edits it; Escape saves
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, on);
    REQUIRE(bubble->isVisible());
    CHECK(bubble->isEditing());
    CHECK(!bubble->editor()->isReadOnly());
    QTest::keyClicks(bubble->editor(), " and $x_y$ too");
    snapshot("comment-edited");
    QTest::keyClick(bubble->editor(), Qt::Key_Escape);
    CHECK(!bubble->isVisible());
    REQUIRE(changed.size() == 1);
    CHECK(jstr(changed[0], "annotationComment") == "Energy $E=mc^2$ here and $x_y$ too");
    CHECK(jstr(changed[0], "key") == jstr(highlight, "key"));

    // an unchanged comment is not saved again; clicking elsewhere closes it
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, on);
    REQUIRE(bubble->isEditing());
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, off);
    CHECK(!bubble->isVisible());
    CHECK(changed.size() == 1);

    // a highlight without a comment gets an empty box to write in; read-only shows only
    highlight["annotationComment"] = "";
    view.setAnnotations({highlight});
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, on);
    CHECK(bubble->isEditing());
    QTest::keyClick(bubble->editor(), Qt::Key_Escape);
    CHECK(changed.size() == 1);
    highlight["annotationComment"] = "Read me";
    view.setAnnotations({highlight});
    view.setEditable(false);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, on);
    CHECK(bubble->isVisible());
    CHECK(!bubble->isEditing());
}

TEST_CASE("notes: Ctrl+B, Ctrl+I and Ctrl+U format the text")
{
    app();
    Env env;
    auto note = env.library.createNote(env.lib, "<p></p>");
    NoteEditor editor;
    editor.resize(600, 400);
    editor.show();
    editor.setNote(env.lib, qs(jstr(note, "key")));
    auto* edit = editor.findChild<NoteTextEdit*>();
    edit->setFocus();
    QApplication::processEvents();
    QTest::keyClicks(edit, "plain ");
    QTest::keyClick(edit, Qt::Key_B, Qt::ControlModifier);
    QTest::keyClicks(edit, "bold");
    QTest::keyClick(edit, Qt::Key_B, Qt::ControlModifier);
    QTest::keyClick(edit, Qt::Key_I, Qt::ControlModifier);
    QTest::keyClicks(edit, " slanted");
    auto block = edit->document()->begin();
    auto formatOf = [&](const QString& word) {
        QTextCursor c(edit->document());
        c.setPosition(block.position() + block.text().indexOf(word) + 1);
        return c.charFormat();
    };
    CHECK(block.text() == "plain bold slanted");
    CHECK(formatOf("plain").fontWeight() != QFont::Bold);
    CHECK(formatOf("bold").fontWeight() == QFont::Bold);
    CHECK(!formatOf("bold").fontItalic());
    CHECK(formatOf("slanted").fontItalic());
    CHECK(formatOf("slanted").fontWeight() != QFont::Bold);
    // on a selection
    QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(edit, Qt::Key_U, Qt::ControlModifier);
    CHECK(formatOf("plain").fontUnderline());
    CHECK(formatOf("slanted").fontUnderline());
}

TEST_CASE("library: a column's edge follows the mouse, the columns after it make room")
{
    app();
    Env env;
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Book"}});
    LibraryView view;
    view.resize(1400, 700);
    view.show();
    QApplication::processEvents();
    QTreeView* items = nullptr;
    for (auto* t : view.findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    auto* header = items->header();
    header->showSection(ItemsModel::Date);
    QApplication::processEvents();
    auto rightEdge = [&](int c) { return header->sectionViewportPosition(c) + header->sectionSize(c); };
    auto lastEnd = [&] {
        int end = 0;
        for (int c = 0; c < header->count(); ++c)
            if (!header->isSectionHidden(c))
                end = std::max(end, rightEdge(c));
        return end;
    };
    REQUIRE(lastEnd() == items->viewport()->width());
    int title = header->sectionSize(ItemsModel::Title), creator = header->sectionSize(ItemsModel::Creator);
    int edge = rightEdge(ItemsModel::Creator);
    int y = header->height() / 2;
    QTest::mousePress(header->viewport(), Qt::LeftButton, {}, QPoint(edge - 1, y));
    for (int dx = 5; dx <= 40; dx += 5)
        QTest::mouseMove(header->viewport(), QPoint(edge - 1 + dx, y));
    QTest::mouseRelease(header->viewport(), Qt::LeftButton, {}, QPoint(edge - 1 + 40, y));
    CHECK(std::abs(rightEdge(ItemsModel::Creator) - (edge + 40)) <= 1);  // where the mouse let go
    CHECK(header->sectionSize(ItemsModel::Creator) == creator + 40);
    CHECK(header->sectionSize(ItemsModel::Title) == title);  // the columns before it stay
    CHECK(lastEnd() == items->viewport()->width());

    // and back, narrower
    edge = rightEdge(ItemsModel::Creator);
    QTest::mousePress(header->viewport(), Qt::LeftButton, {}, QPoint(edge - 1, y));
    for (int dx = 5; dx <= 60; dx += 5)
        QTest::mouseMove(header->viewport(), QPoint(edge - 1 - dx, y));
    QTest::mouseRelease(header->viewport(), Qt::LeftButton, {}, QPoint(edge - 1 - 60, y));
    CHECK(std::abs(rightEdge(ItemsModel::Creator) - (edge - 60)) <= 1);
    CHECK(header->sectionSize(ItemsModel::Title) == title);
    CHECK(lastEnd() == items->viewport()->width());
}

TEST_CASE("notes: Markdown typed or pasted becomes formatting")
{
    app();
    Env env;
    auto note = env.library.createNote(env.lib, "<p></p>");
    NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    editor.setNote(env.lib, qs(jstr(note, "key")));
    auto* edit = editor.findChild<NoteTextEdit*>();
    REQUIRE(edit);
    edit->setFocus();
    auto* doc = edit->document();

    QTest::keyClicks(edit, "# Title");
    CHECK(doc->begin().blockFormat().headingLevel() == 1);
    CHECK(doc->begin().text() == "Title");
    QTest::keyClick(edit, Qt::Key_Return);
    QTest::keyClicks(edit, "- first");
    CHECK(edit->textCursor().currentList());
    CHECK(edit->textCursor().block().text() == "first");
    QTest::keyClick(edit, Qt::Key_Return);
    QTest::keyClick(edit, Qt::Key_Return);  // out of the list
    QTest::keyClicks(edit, "> quoted");
    CHECK(edit->textCursor().blockFormat().leftMargin() >= 20);
    QTest::keyClick(edit, Qt::Key_Return);
    auto quote = edit->textCursor().blockFormat();
    quote.setLeftMargin(0);
    quote.setRightMargin(0);
    edit->textCursor().setBlockFormat(quote);

    QTest::keyClicks(edit, "a **bold** and *slanted* and `code` and ~~gone~~ and snake_case_name");
    auto block = edit->textCursor().block();
    CHECK(block.text() == "a bold and slanted and code and gone and snake_case_name");
    auto formatAt = [&](const QString& word) {
        QTextCursor c(doc);
        c.setPosition(block.position() + block.text().indexOf(word) + 1);
        return c.charFormat();
    };
    CHECK(formatAt("bold").fontWeight() == QFont::Bold);
    CHECK(formatAt("slanted").fontItalic());
    CHECK(formatAt("code").fontFixedPitch());
    CHECK(formatAt("gone").fontStrikeOut());
    CHECK(formatAt("and").fontWeight() != QFont::Bold);
    CHECK(!formatAt("snake").fontItalic());

    // a link, and Ctrl+Z gives back what was typed
    QTest::keyClicks(edit, " [site](https://example.org)");
    CHECK(edit->textCursor().block().text().endsWith(" site"));
    CHECK(formatAt("site").anchorHref() == "https://example.org");
    QTest::keyClick(edit, Qt::Key_Z, Qt::ControlModifier);
    CHECK(edit->textCursor().block().text().endsWith(" [site](https://example.org)"));
    QTest::keyClick(edit, Qt::Key_Y, Qt::ControlModifier);
    CHECK(edit->textCursor().block().text().endsWith(" site"));

    editor.flush();
    auto html = jstr(*env.library.item(env.lib, jstr(note, "key")), "note");
    CHECK(html.find("<h1>Title</h1>") != std::string::npos);
    CHECK(html.find("<li>") != std::string::npos);
    CHECK(html.find("bold</") != std::string::npos);
    CHECK(html.find("**") == std::string::npos);
    CHECK(html.find("href=\"https://example.org\"") != std::string::npos);

    // pasted as plain text
    edit->moveCursor(QTextCursor::End);
    QTest::keyClick(edit, Qt::Key_Return);
    QApplication::clipboard()->setText("## Pasted\n\n- one\n- **two** with $x_y$\n\n$$E=mc^2$$\n");
    int blocksBefore = doc->blockCount();
    edit->paste();
    CHECK(doc->blockCount() > blocksBefore + 2);
    bool heading = false, bold = false;
    for (auto b = doc->begin(); b.isValid(); b = b.next()) {
        heading |= b.text() == "Pasted" && b.blockFormat().headingLevel() == 2;
        for (auto it = b.begin(); !it.atEnd(); ++it)
            bold |= it.fragment().text() == "two" && it.fragment().charFormat().fontWeight() == QFont::Bold;
    }
    CHECK(heading);
    CHECK(bold);
    QStringList latex;
    CHECK(countMath(doc, &latex) == 2);
    CHECK(latex.contains("x_y"));
    CHECK(latex.contains("E=mc^2"));

    // comments stay as typed
    NoteTextEdit comment;
    comment.show();
    QTest::keyClicks(&comment, "# not a heading **nor bold**");
    CHECK(comment.toPlainText() == "# not a heading **nor bold**");
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
    // red, not how bright: the thin strokes come out lighter on Windows (few pixels > 180)
    CHECK(brightest([](QColor c) { return c.red() > 120 && c.green() < 60; }) > 3);
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

TEST_CASE("notes: formulas are neither clipped nor blurred")
{
    app();
    // no ink touches the image's edges (the TeX box leaves out ink outside it)
    for (const char* tex : {"x_y", "g_j", "\\int_0^1 f(x)\\,dx", "\\sqrt{y}", "f^2", "\\frac{a}{b}"}) {
        auto m = renderMath(tex, false, 19, Qt::black, 1);
        REQUIRE(m.ok);
        const QImage& img = m.image;
        QString edges;
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, 0)))
                edges += "top ";
            if (qAlpha(img.pixel(x, img.height() - 1)))
                edges += "bottom ";
        }
        for (int y = 0; y < img.height(); ++y) {
            if (qAlpha(img.pixel(0, y)))
                edges += "left ";
            if (qAlpha(img.pixel(img.width() - 1, y)))
                edges += "right ";
        }
        INFO(std::string(tex));
        CHECK(edges.toStdString() == "");
    }

    // drawn pixel for pixel, at 150 % and with the text between pixels
    const qreal dpr = 1.5;
    QTextDocument doc;
    QFont font;
    font.setPixelSize(16);
    doc.setDefaultFont(font);
    FittedImageHandler::install(&doc, &doc)->setColorSource([] { return QColor(Qt::black); });
    QTextCursor c(&doc);
    c.insertText("a ");
    QTextImageFormat f;
    f.setName(mathResourceName("x_y", false));
    c.insertImage(f);
    c.insertText(" b");
    QImage canvas(450, 120, QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::white);
    {
        QPainter p(&canvas);
        p.translate(10.5, 7.25);
        doc.drawContents(&p);
    }
    QImage formula = renderMath("x_y", false, QFontInfo(font).pixelSize() * 1.2, Qt::black, dpr).image;
    formula.setDevicePixelRatio(1);  // compared in device pixels
    QImage expected(formula.size(), QImage::Format_ARGB32_Premultiplied);
    expected.fill(Qt::white);
    {
        QPainter p(&expected);
        p.drawImage(0, 0, formula);
    }
    auto matchesAt = [&](int x0, int y0) {
        for (int y = 0; y < expected.height(); ++y)
            for (int x = 0; x < expected.width(); ++x)
                if (canvas.pixel(x0 + x, y0 + y) != expected.pixel(x, y))
                    return false;
        return true;
    };
    bool found = false;
    for (int y = 0; y + expected.height() <= canvas.height() && !found; ++y)
        for (int x = 0; x + expected.width() <= canvas.width() && !found; ++x)
            found = matchesAt(x, y);
    CHECK(found);
}

TEST_CASE("notes: text size from the preferences and per view, without touching the note")
{
    app();
    Env env;
    env.config.set("/notes/zoom", 125);
    auto note = env.library.createNote(env.lib, "<h1>Title</h1><p>Text <span class=\"math\">$x_y$</span> here</p>");
    auto stored = [&] { return jstr(*env.library.item(env.lib, jstr(note, "key")), "note"); };
    auto before = stored();
    NoteEditor editor;
    editor.resize(500, 400);
    editor.show();
    editor.setNote(env.lib, qs(jstr(note, "key")));
    QApplication::processEvents();
    auto* edit = editor.findChild<NoteTextEdit*>();
    REQUIRE(edit);
    auto* button = editor.findChild<QToolButton*>("noteZoom");
    REQUIRE(button);
    CHECK(editor.zoom() == 125);
    CHECK(button->text() == "125%");
    auto height = [&] { return edit->document()->documentLayout()->documentSize().height(); };
    double size125 = edit->font().pointSizeF(), height125 = height();

    edit->setFocus();
    QTest::keyClick(edit, Qt::Key_Equal, Qt::ControlModifier);
    CHECK(editor.zoom() == 150);
    CHECK(button->text() == "150%");
    CHECK(edit->font().pointSizeF() == doctest::Approx(size125 * 150 / 125));
    CHECK(height() > height125 * 1.12);  // the heading and the formula grow as well
    QWheelEvent wheel(QPointF(50, 50), edit->viewport()->mapToGlobal(QPointF(50, 50)), {}, QPoint(0, -120),
                      Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(edit->viewport(), &wheel);
    CHECK(editor.zoom() == 125);
    QTest::keyClick(edit, Qt::Key_Minus, Qt::ControlModifier);
    CHECK(editor.zoom() == 110);
    QTest::keyClick(edit, Qt::Key_0, Qt::ControlModifier);
    CHECK(editor.zoom() == 125);
    button->click();
    CHECK(editor.zoom() == 125);
    // a new default applies to open notes
    env.config.set("/notes/zoom", 100);
    emit AppContext::get().appearanceChanged();
    CHECK(editor.zoom() == 100);
    editor.flush();
    CHECK(stored() == before);  // only the view changed

    // the undo history survives, and a heading made here follows the size
    edit->moveCursor(QTextCursor::End);
    QTest::keyClicks(edit, "!");
    editor.setZoom(150);
    CHECK(edit->document()->isUndoAvailable());
    emit editor.findChild<QComboBox*>()->activated(2);
    auto block = edit->textCursor().block();
    CHECK(block.blockFormat().headingLevel() == 2);
    auto cf = block.begin().fragment().charFormat();
    CHECK(!cf.hasProperty(QTextFormat::FontPointSize));
    CHECK(cf.property(QTextFormat::FontSizeAdjustment).toInt() == 2);
    double h150 = height();
    editor.setZoom(100);
    CHECK(height() < h150 * 0.8);
}

TEST_CASE("notes: autosaving keeps the whole undo history")
{
    app();
    Env env;
    auto note = env.library.createNote(env.lib, "<p></p>");
    NoteEditor editor;
    editor.resize(500, 400);
    editor.show();
    editor.setNote(env.lib, qs(jstr(note, "key")));
    auto* edit = editor.findChild<NoteTextEdit*>();
    REQUIRE(edit);
    edit->setFocus();
    // each part is saved (as when typing pauses) before the next is typed
    for (const char* part : {"man ", "$x_y$", " can"}) {
        QTest::keyClicks(edit, part);
        editor.flush();
        QApplication::processEvents();
    }
    REQUIRE(countMath(edit->document()) == 1);
    REQUIRE(jstr(*env.library.item(env.lib, jstr(note, "key")), "note").find("can") != std::string::npos);
    auto full = edit->toPlainText();

    // undo goes back past every save, to the empty note, and redo all the way forward
    for (int i = 0; i < 40 && !edit->toPlainText().isEmpty(); ++i)
        QTest::keyClick(edit, Qt::Key_Z, Qt::ControlModifier);
    CHECK(edit->toPlainText().isEmpty());
    CHECK(countMath(edit->document()) == 0);
    for (int i = 0; i < 40 && edit->document()->isRedoAvailable(); ++i)
        QTest::keyClick(edit, Qt::Key_Y, Qt::ControlModifier);
    CHECK(edit->toPlainText() == full);
    CHECK(countMath(edit->document()) == 1);
}

TEST_CASE("sync history: the last syncs, each object once under the last that changed it")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sync-history.json";
    auto change = [](const std::string& key, const std::string& title, const std::string& action, bool incoming) {
        sync::SyncChange c;
        c.libraryId = 1;
        c.library = "Main";
        c.type = "item";
        c.key = key;
        c.kind = "note";
        c.title = title;
        c.action = action;
        c.incoming = incoming;
        return c;
    };
    auto report = [](std::vector<sync::SyncChange> changes) {
        sync::SyncReport r;
        r.changes = std::move(changes);
        return r;
    };
    {
        SyncHistory h(file);
        h.add("server", report({change("NOTE0001", "a", "added", false), change("BOOK0001", "Book", "added", false)}),
              true, "pushed 2");
        h.add("server", report({change("NOTE0001", "abc", "changed", false)}), true, "pushed 1");
        REQUIRE(h.runs().size() == 2);
        CHECK(h.runs()[0]["changes"].size() == 1);
        CHECK(h.runs()[0]["changes"][0]["title"] == "abc");
        REQUIRE(h.runs()[1]["changes"].size() == 1);  // the note moved up to the last sync
        CHECK(h.runs()[1]["changes"][0]["key"] == "BOOK0001");

        // deleted from here: the title comes from before
        sync::SyncChange gone = change("BOOK0001", "", "deleted", false);
        h.add("server", report({gone}), true, "pushed 1");
        CHECK(h.runs()[0]["changes"][0]["title"] == "Book");
        CHECK(h.runs().size() == 2);  // the first sync has nothing left

        // syncs without changes: only the last is kept; failures stay
        h.add("server", report({}), true, "pulled 0, pushed 0");
        h.addFailure("server", "cannot reach the server");
        h.add("server", report({}), true, "pulled 0, pushed 0");
        REQUIRE(h.runs().size() == 4);
        CHECK(h.runs()[0]["changes"].empty());
        CHECK(h.runs()[1]["ok"] == false);
        CHECK(h.runs()[2]["changes"][0]["action"] == "deleted");
    }
    SyncHistory again(file);  // kept in the file
    REQUIRE(again.runs().size() == 4);
    for (int i = 0; i < 40; ++i)
        again.add("server", report({change("N" + std::to_string(i), "x", "added", true)}), true, "pulled 1");
    CHECK(again.runs().size() == SyncHistory::kMaxRuns);

    SyncHistoryWindow window;
    window.reload(again);
    auto* tree = window.findChild<QTreeWidget*>("syncHistory");
    REQUIRE(tree);
    CHECK(tree->topLevelItemCount() == SyncHistory::kMaxRuns);
    auto* first = tree->topLevelItem(0);
    CHECK(first->text(0).contains("1 received"));
    REQUIRE(first->childCount() == 1);
    CHECK(first->child(0)->text(0).contains("Note"));
    CHECK(first->child(0)->text(1) == "Added");
}

TEST_CASE("sync history: filled by the syncs the app runs")
{
    app();
    Env env;
    TempDir folder;
    SyncTarget target;
    target.id = "folder1";
    target.name = "USB stick";
    target.kind = "local";
    target.path = pathString(folder.path);
    env.config.upsertTarget(target);
    env.config.setActiveTarget("folder1");
    env.config.set("/sync/auto", false);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Synced Book"}});
    SyncController sync;
    bool done = false;
    QObject::connect(&sync, &SyncController::finished, [&] { done = true; });
    sync.syncNow();
    REQUIRE(QTest::qWaitFor([&] { return done; }, 10000));
    auto& runs = sync.history().runs();
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]["target"] == "USB stick");
    CHECK(runs[0]["ok"] == true);
    bool listed = false;
    for (auto& c : runs[0]["changes"])
        listed |= c["key"] == jstr(paper, "key") && c["title"] == "Synced Book" && c["incoming"] == false;
    CHECK(listed);
    CHECK(fs::exists(env.library.dataDir() / "sync-history.json"));
}

TEST_CASE("library: a PDF dropped on a collection or an item goes there")
{
    app();
    Env env;
    TempDir tmp;
    auto pdf = tmp.path / "datasheet.pdf";
    writeSamplePdf(pdf);
    auto datasheets = jstr(env.library.createCollection(env.lib, "Datasheet"), "key");
    auto other = jstr(env.library.createCollection(env.lib, "Other"), "key");
    auto book = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "TMS"}});
    env.library.addToCollection(env.lib, datasheets, {jstr(book, "key")});
    LibraryView view;
    view.resize(1200, 700);
    view.show();
    auto* tree = view.findChild<CollectionTree*>();
    REQUIRE(tree);
    tree->selectScope({env.lib, ScopeKind::Collection, other});
    QApplication::processEvents();

    auto dropOn = [&](QWidget* target, QPoint at) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(qpath(pdf))});
        QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QDragMoveEvent move(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        QDropEvent drop(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &drop);
        QApplication::processEvents();
        return drop.isAccepted();
    };
    auto attachments = [&] {
        std::vector<Json> out;
        for (auto& d : env.library.allItems(env.lib))
            if (jstr(d, "itemType") == "attachment")
                out.push_back(d);
        return out;
    };

    // onto a collection that is not the one shown
    QTreeWidgetItem* target = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == "Datasheet")
            target = *it;
    REQUIRE(target);
    CHECK(dropOn(tree->viewport(), tree->visualItemRect(target).center()));
    auto added = attachments();
    REQUIRE(added.size() == 1);
    CHECK(jstr(added[0], "parentItem").empty());
    CHECK(env.library.collectionItemKeys(env.lib, datasheets).count(jstr(added[0], "key")));
    CHECK(!env.library.collectionItemKeys(env.lib, other).count(jstr(added[0], "key")));

    // onto an item: its attachment
    tree->selectScope({env.lib, ScopeKind::Collection, datasheets});
    QApplication::processEvents();
    auto* items = view.findChild<QTreeView*>("itemsView");
    if (!items)
        for (auto* t : view.findChildren<QTreeView*>())
            if (t != tree && qobject_cast<ItemsModel*>(t->model()))
                items = t;
    REQUIRE(items);
    QModelIndex row;
    for (int r = 0; r < items->model()->rowCount(); ++r)
        if (items->model()->index(r, 0).data().toString() == "TMS")
            row = items->model()->index(r, 0);
    REQUIRE(row.isValid());
    CHECK(dropOn(items->viewport(), items->visualRect(row).center()));
    bool attached = false;
    for (auto& a : attachments())
        attached |= jstr(a, "parentItem") == jstr(book, "key");
    CHECK(attached);
}

TEST_CASE("library: a PDF item dragged onto an item becomes its attachment, the target lit up")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "tms320.pdf";
    writeSamplePdf(file);
    auto datasheets = jstr(env.library.createCollection(env.lib, "Datasheet"), "key");
    auto book = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "TMS"}});
    auto pdf = env.library.importAttachment(env.lib, file, "");
    env.library.addToCollection(env.lib, datasheets, {jstr(book, "key"), jstr(pdf, "key")});
    LibraryView view;
    view.resize(1200, 700);
    view.show();
    auto* tree = view.findChild<CollectionTree*>();
    tree->selectScope({env.lib, ScopeKind::Collection, datasheets});
    QApplication::processEvents();
    QTreeView* items = nullptr;
    for (auto* t : view.findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    auto* model = static_cast<ItemsModel*>(items->model());
    auto rowOf = [&](const QString& title) {
        for (int r = 0; r < model->rowCount(); ++r)
            if (model->index(r, 0).data().toString() == title)
                return model->index(r, 0);
        return QModelIndex();
    };
    auto target = rowOf("TMS"), dragged = rowOf(jqs(pdf, "title"));
    REQUIRE(target.isValid());
    REQUIRE(dragged.isValid());
    std::unique_ptr<QMimeData> mime(model->mimeData({dragged}));
    REQUIRE(mime->hasFormat(kItemsMime));
    CHECK(mime->hasUrls());  // its file, for dropping outside the app

    // the window does not import the list's own items as files
    MainWindow window;
    QDragEnterEvent intoWindow(QPoint(10, 10), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &intoWindow);
    CHECK(!intoWindow.isAccepted());

    QPoint at = items->visualRect(target).center();
    QDragEnterEvent enter(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(items->viewport(), &enter);
    QDragMoveEvent move(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(items->viewport(), &move);
    CHECK(move.isAccepted());
    // lit up like a selected row, at its far end where there is no text
    auto shot = items->viewport()->grab().toImage();
    QRect r = items->visualRect(target);
    QColor lit = shot.pixelColor(QPoint(r.right() - 4, r.center().y()) * shot.devicePixelRatio());
    QColor highlight = items->palette().color(QPalette::Highlight);
    CHECK(std::abs(lit.red() - highlight.red()) + std::abs(lit.green() - highlight.green()) +
              std::abs(lit.blue() - highlight.blue()) < 60);
    QDropEvent drop(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(items->viewport(), &drop);
    QApplication::processEvents();
    CHECK(jstr(*env.library.item(env.lib, jstr(pdf, "key")), "parentItem") == jstr(book, "key"));
    int attachments = 0;
    for (auto& d : env.library.allItems(env.lib))
        attachments += jstr(d, "itemType") == "attachment";
    CHECK(attachments == 1);  // moved, not imported again

    // a collection a drag hovers on is lit up too
    QTreeWidgetItem* coll = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == "Datasheet")
            coll = *it;
    REQUIRE(coll);
    tree->selectScope({env.lib, ScopeKind::Library, {}});
    QApplication::processEvents();
    std::unique_ptr<QMimeData> bookMime(model->mimeData({rowOf("TMS")}));
    QPoint onColl = tree->visualItemRect(coll).center();
    QDragEnterEvent enterTree(onColl, Qt::CopyAction, bookMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &enterTree);
    QDragMoveEvent moveTree(onColl, Qt::CopyAction, bookMime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &moveTree);
    CHECK(moveTree.isAccepted());
    shot = tree->viewport()->grab().toImage();
    QRect cr = tree->visualItemRect(coll);
    lit = shot.pixelColor(QPoint(cr.right() - 4, cr.center().y()) * shot.devicePixelRatio());
    highlight = tree->palette().color(QPalette::Highlight);
    CHECK(std::abs(lit.red() - highlight.red()) + std::abs(lit.green() - highlight.green()) +
              std::abs(lit.blue() - highlight.blue()) < 60);
    QDragLeaveEvent leave;
    QApplication::sendEvent(tree->viewport(), &leave);
    shot = tree->viewport()->grab().toImage();
    lit = shot.pixelColor(QPoint(cr.right() - 4, cr.center().y()) * shot.devicePixelRatio());
    CHECK(std::abs(lit.red() - highlight.red()) + std::abs(lit.green() - highlight.green()) +
              std::abs(lit.blue() - highlight.blue()) >= 60);
}

TEST_CASE("connector: a page's item is cleaned up to what OurNotes keeps")
{
    auto item = connectorItem(Json{{"itemType", "journalArticle"},
                                   {"title", "  Deep Learning \n"},
                                   {"publicationTitle", "Nature"},
                                   {"websiteTitle", "not a journal article field"},
                                   {"key", "ABCD1234"},
                                   {"creators", Json::array({Json{{"creatorType", "author"}, {"firstName", "Yann"}, {"lastName", "LeCun"}},
                                                             Json{{"creatorType", "nonsense"}, {"name", "OpenAI"}},
                                                             Json{{"firstName", ""}}})},
                                   {"tags", Json::array({"ml", Json{{"tag", "review"}}, ""})}});
    CHECK(item["itemType"] == "journalArticle");
    CHECK(item["title"] == "Deep Learning");
    CHECK(item["publicationTitle"] == "Nature");
    CHECK(!item.contains("websiteTitle"));
    CHECK(!item.contains("key"));
    REQUIRE(item["creators"].size() == 2);
    CHECK(item["creators"][1]["creatorType"] == "author");
    CHECK(item["creators"][1]["name"] == "OpenAI");
    CHECK(item["tags"].size() == 2);
    CHECK(!jstr(item, "accessDate").empty() == schema::hasField("journalArticle", "accessDate"));
    auto page = connectorItem(Json{{"itemType", "spaceship"}, {"url", "https://example.org/a"}});
    CHECK(page["itemType"] == "webpage");
    CHECK(page["title"] == "https://example.org/a");
    CHECK(!jstr(page, "accessDate").empty());
}

TEST_CASE("connector: the browser extension saves pages, and nothing else gets in")
{
    app();
    Env env;
    TempDir tmp;
    auto reading = jstr(env.library.createCollection(env.lib, "Reading"), "key");
    auto other = jstr(env.library.createCollection(env.lib, "Other"), "key");
    ConnectorServer server;
    server.targetScope = [&] { return Scope{env.lib, ScopeKind::Collection, reading}; };
    QStringList lookUp;
    QObject::connect(&server, &ConnectorServer::metadataWanted, [&](qint64, const QStringList& keys) { lookUp = keys; });
    int port = 0;
    for (int p = 23190; p < 23290 && !port; ++p)
        if (server.start(p))
            port = p;
    REQUIRE(port);
    CHECK(!ConnectorServer(nullptr).start(port));  // taken

    // requests block until the GUI thread has done the work: run them aside
    auto call = [&](std::function<httplib::Result(httplib::Client&)> fn) {
        auto f = std::async(std::launch::async, [&] {
            httplib::Client c("127.0.0.1", port);
            c.set_read_timeout(30);
            auto r = fn(c);
            return r ? std::make_pair(r->status, r->body) : std::make_pair(-1, std::string());
        });
        REQUIRE(QTest::qWaitFor([&] { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, 30000));
        return f.get();
    };
    httplib::Headers ext{{"Origin", "chrome-extension://abcdefghijklmnop"}, {"X-OurNotes-Connector", "1"}};
    Json page{{"item",
               {{"itemType", "journalArticle"},
                {"title", "Deep Learning"},
                {"creators", Json::array({Json{{"creatorType", "author"}, {"firstName", "Yann"}, {"lastName", "LeCun"}}})},
                {"publicationTitle", "Nature"},
                {"DOI", "10.1038/nature14539"},
                {"url", "https://www.nature.com/articles/nature14539"}}}};
    auto body = page.dump();

    auto [pingStatus, pingBody] = call([&](httplib::Client& c) { return c.Get("/connector/ping", ext); });
    CHECK(pingStatus == 200);
    CHECK(Json::parse(pingBody)["app"] == "OurNotes");

    // web pages, other host names and requests without the header are turned away
    auto count = [&] { return env.library.allItems(env.lib).size(); };
    auto before = count();
    httplib::Headers page1{{"Origin", "https://evil.example"}, {"X-OurNotes-Connector", "1"}};
    CHECK(call([&](httplib::Client& c) { return c.Post("/connector/save", page1, body, "application/json"); }).first == 403);
    httplib::Headers rebound{{"Origin", "chrome-extension://abcdefghijklmnop"}, {"X-OurNotes-Connector", "1"},
                             {"Host", "evil.example:" + std::to_string(port)}};
    CHECK(call([&](httplib::Client& c) { return c.Post("/connector/save", rebound, body, "application/json"); }).first == 403);
    httplib::Headers bare{{"Origin", "chrome-extension://abcdefghijklmnop"}};
    CHECK(call([&](httplib::Client& c) { return c.Post("/connector/save", bare, body, "application/json"); }).first == 400);
    CHECK(count() == before);

    // into the selected collection
    auto [status, savedBody] = call([&](httplib::Client& c) { return c.Post("/connector/save", ext, body, "application/json"); });
    REQUIRE(status == 200);
    auto saved = Json::parse(savedBody);
    auto key = jstr(saved, "key");
    auto item = env.library.item(env.lib, key);
    REQUIRE(item);
    CHECK(jstr(*item, "title") == "Deep Learning");
    CHECK(jstr(*item, "publicationTitle") == "Nature");
    CHECK(env.library.collectionItemKeys(env.lib, reading).count(key));
    CHECK(saved["target"] == std::to_string(env.lib) + "/" + reading);
    bool listed = false;
    for (auto& t : saved["targets"])
        listed |= t["name"] == "Other" && t["depth"] == 1;
    CHECK(listed);

    // its PDF, as the browser fetched it
    auto pdf = tmp.path / "paper.pdf";
    writeSamplePdf(pdf);
    auto bytes = readFile(pdf);
    std::string query = "/connector/attachment?parent=" + key + "&library=" + std::to_string(env.lib) +
                        "&url=https%3A%2F%2Fwww.nature.com%2Fpaper.pdf&title=Deep%20Learning";
    CHECK(call([&](httplib::Client& c) { return c.Post(query, ext, "not a pdf", "application/pdf"); }).first == 415);
    auto [pdfStatus, pdfBody] = call([&](httplib::Client& c) { return c.Post(query, ext, bytes, "application/pdf"); });
    REQUIRE(pdfStatus == 200);
    auto att = env.library.item(env.lib, jstr(Json::parse(pdfBody), "key"));
    REQUIRE(att);
    CHECK(jstr(*att, "parentItem") == key);
    CHECK(jstr(*att, "linkMode") == "imported_url");
    CHECK(jstr(*att, "url") == "https://www.nature.com/paper.pdf");
    CHECK(fs::exists(env.library.attachmentFile(env.lib, *att)));

    // the popup moves it and tags it
    Json move{{"key", key}, {"library", env.lib}, {"target", std::to_string(env.lib) + "/" + other}, {"tags", {"ml", "review"}}};
    CHECK(call([&](httplib::Client& c) { return c.Post("/connector/update", ext, move.dump(), "application/json"); }).first == 200);
    CHECK(env.library.collectionItemKeys(env.lib, other).count(key));
    CHECK(!env.library.collectionItemKeys(env.lib, reading).count(key));
    CHECK(env.library.item(env.lib, key)->value("tags", Json::array()).size() == 2);

    // a PDF tab: the file on its own, its details looked up afterwards
    auto [aloneStatus, aloneBody] = call([&](httplib::Client& c) {
        return c.Post("/connector/attachment?url=https%3A%2F%2Farxiv.org%2Fpdf%2F1234.pdf", ext, bytes, "application/pdf");
    });
    REQUIRE(aloneStatus == 200);
    auto alone = jstr(Json::parse(aloneBody), "key");
    CHECK(jstr(*env.library.item(env.lib, alone), "parentItem").empty());
    CHECK(env.library.collectionItemKeys(env.lib, reading).count(alone));
    CHECK(lookUp == QStringList{qs(alone)});
    server.stop();
}

// Answers the next message box (yes: its Yes / OK / destructive button), noting its text.
void answerNextMessageBox(bool yes, QString* text)
{
    auto* timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, [timer, yes, text] {
        for (auto* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (!box || !box->isVisible())
                continue;
            *text = box->text();
            for (auto* b : box->buttons()) {
                auto role = box->buttonRole(b);
                bool isYes = role == QMessageBox::YesRole || role == QMessageBox::AcceptRole ||
                             role == QMessageBox::DestructiveRole;
                if (isYes == yes) {
                    b->click();
                    break;
                }
            }
            timer->stop();
            timer->deleteLater();
            return;
        }
    });
    timer->start();
}

TEST_CASE("library: Delete on a PDF trashes the PDF, Shift+Delete deletes for good, the trash restores")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "tms.pdf";
    writeSamplePdf(file);
    auto coll = jstr(env.library.createCollection(env.lib, "Datasheet"), "key");
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "TMS"}});
    auto pdf = env.library.importAttachment(env.lib, file, jstr(paper, "key"));
    auto other = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Gone for good"}});
    env.library.addToCollection(env.lib, coll, {jstr(paper, "key"), jstr(other, "key")});
    LibraryView view;
    view.resize(1300, 800);
    view.show();
    auto* tree = view.findChild<CollectionTree*>();
    tree->selectScope({env.lib, ScopeKind::Collection, coll});
    QApplication::processEvents();
    QTreeView* items = nullptr;
    for (auto* t : view.findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    auto* model = items->model();
    auto row = [&](const QString& title, const QModelIndex& parent = {}) {
        for (int r = 0; r < model->rowCount(parent); ++r)
            if (model->index(r, 0, parent).data().toString() == title)
                return model->index(r, 0, parent);
        return QModelIndex();
    };
    auto select = [&](const QModelIndex& i) {
        REQUIRE(i.isValid());
        items->setFocus();
        items->selectionModel()->setCurrentIndex(i, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    };
    auto item = [&](const Json& d) { return env.library.item(env.lib, jstr(d, "key")); };

    // Delete on the PDF of an item in a collection: the PDF goes to the trash, the item stays
    items->expand(row("TMS"));
    select(row(jqs(pdf, "title"), row("TMS")));
    QString asked;
    answerNextMessageBox(true, &asked);
    QTest::keyClick(items, Qt::Key_Delete);
    CHECK(asked.contains("trash"));
    CHECK(jbool(*item(pdf), "deleted"));
    CHECK(!jbool(*item(paper), "deleted"));
    CHECK(env.library.collectionItemKeys(env.lib, coll).count(jstr(paper, "key")));

    // Shift+Delete: gone, not in the trash
    QApplication::processEvents();
    select(row("Gone for good"));
    asked.clear();
    answerNextMessageBox(true, &asked);
    QTest::keyClick(items, Qt::Key_Delete, Qt::ShiftModifier);
    CHECK(asked.contains("Permanently"));
    CHECK(!item(other));

    // in the trash: the PDF says whose it is, and the item pane restores it
    tree->selectScope({env.lib, ScopeKind::Trash, {}});
    QApplication::processEvents();
    REQUIRE(model->rowCount() == 1);
    CHECK(model->index(0, 0).data().toString().contains("TMS"));
    select(model->index(0, 0));
    QApplication::processEvents();
    QApplication::processEvents();
    auto* restore = view.findChild<QPushButton*>("restoreFromTrash");
    REQUIRE(restore);
    CHECK(view.findChild<QPushButton*>("deletePermanently"));
    restore->click();
    CHECK(!jbool(*item(pdf), "deleted"));
    CHECK(QTest::qWaitFor([&] { return model->rowCount() == 0; }, 3000));
}

TEST_CASE("main window: only an empty library is deleted, after asking")
{
    app();
    Env env;
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Kept"}});
    auto extra = env.library.createLibrary("Extra", "group");
    MainWindow window;
    window.show();
    auto* view = window.findChild<LibraryView*>();
    REQUIRE(view);
    auto ask = [&](int64_t id, const QString& box, bool confirm) {
        QString text;
        answerConfirmation(box, confirm, &text);
        emit view->deleteLibraryRequested(id);
        REQUIRE(QTest::qWaitFor([&] { return !text.isEmpty(); }));
        QApplication::processEvents();
        return text;
    };

    auto text = ask(env.lib, "libraryNotDeletable", false);
    CHECK(text.contains("1 item"));
    CHECK(text.contains("empty the trash"));
    CHECK(env.library.library(env.lib));

    ask(extra, "confirmDeleteLibrary", false);
    CHECK(env.library.library(extra));
    text = ask(extra, "confirmDeleteLibrary", true);
    CHECK(text.contains("Extra"));
    CHECK(!env.library.library(extra));
    CHECK(window.findChild<QTreeWidget*>()->topLevelItemCount() == 1);

    CHECK(ask(env.lib, "libraryNotDeletable", false).contains("only library"));
}

TEST_CASE("main window: a short tab title is shown whole")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "tms.pdf";
    writeSamplePdf(file);
    MainWindow window;
    window.resize(1200, 700);
    auto* strip = window.findChild<QWidget*>("tabStrip");
    REQUIRE(strip);
    QFont f = strip->font();
    f.setPointSizeF(14);  // "TMS" is 40.03 px wide here, which QFontMetrics rounds to 40
    strip->setFont(f);
    window.show();
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "TMS"}});
    window.openAttachment(env.lib, qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key")));
    QApplication::processEvents();
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    auto buttons = strip->findChildren<QWidget*>("tabButton");
    REQUIRE(buttons.size() == tabs->count());
    int index = -1;
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == "TMS")
            index = i;
    REQUIRE(index > 0);
    QWidget* tab = nullptr;
    for (auto* b : buttons)  // in tab order, laid out left to right in one row here
        if (!tab || b->x() > tab->x())
            tab = b;
    REQUIRE(tab);
    // the title at its own width looks as it does with room to spare
    QImage natural = tab->grab().toImage();
    int w = tab->width();
    tab->resize(w + 60, tab->height());
    QImage roomy = tab->grab().toImage();
    tab->resize(w, tab->height());
    int x0 = 32, x1 = std::min(natural.width(), w - 32);  // the title's space (icon and close button aside)
    int differing = 0;
    for (int y = 0; y < natural.height(); ++y)
        for (int x = x0; x < x1; ++x)
            differing += natural.pixel(x, y) != roomy.pixel(x, y);
    CHECK(differing == 0);
}

TEST_CASE("reader: keys 1 to 8 pick the tools, but not while typing")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    env.library.createNote(env.lib, "<p>Thoughts</p>", jstr(parent, "key"));
    auto key = jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key");
    ReaderTab tab(env.lib, qs(key));
    tab.resize(1200, 800);
    tab.show();
    QApplication::processEvents();
    auto* view = tab.view();
    view->setFocus();
    QTest::keyClick(view, Qt::Key_2);
    CHECK(view->tool() == ReaderTool::Highlight);
    QTest::keyClick(view, Qt::Key_8);
    CHECK(view->tool() == ReaderTool::Eraser);
    QTest::keyClick(view, Qt::Key_1);
    CHECK(view->tool() == ReaderTool::Select);
    QTest::keyClick(view, Qt::Key_9);  // not a tool
    CHECK(view->tool() == ReaderTool::Select);

    // typed into the note next to the PDF, or into a comment: digits
    auto* note = tab.findChild<NoteTextEdit*>();
    REQUIRE(note);
    note->setFocus();
    QTest::keyClicks(note, "34");
    CHECK(note->toPlainText().contains("34"));
    CHECK(view->tool() == ReaderTool::Select);
    view->setFocus();
    Json highlight;
    QObject::connect(view, &PdfView::annotationCreated, [&](const Json& d) { highlight = d; });
    QTest::keyClick(view, Qt::Key_2);
    drag(view->viewport(), charCenter(*view, 0), charCenter(*view, 4));
    REQUIRE(highlight.is_object());
    view->setAnnotations({highlight});
    QTest::keyClick(view, Qt::Key_1);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, charCenter(*view, 2).toPoint());
    auto* bubble = view->commentBubble();
    REQUIRE(bubble->isEditing());
    QTest::keyClicks(bubble->editor(), "page 5");
    CHECK(bubble->editor()->toPlainText() == "page 5");
    CHECK(view->tool() == ReaderTool::Select);
}

TEST_CASE("reader: F fits the width, F again the page")
{
    app();
    TempDir tmp;
    auto file = tmp.path / "sample.pdf";
    writeSamplePdf(file);
    PdfView view;
    view.resize(900, 600);  // wide: the whole page is smaller than the width
    view.open(file);
    view.show();
    QApplication::processEvents();
    view.fitPage();
    double pageZoom = view.zoom();
    view.fitWidth();
    double widthZoom = view.zoom();
    REQUIRE(pageZoom < widthZoom);
    view.setZoom(1.0);
    view.setFocus();
    QTest::keyClick(&view, Qt::Key_F);
    CHECK(view.isFitWidth());
    CHECK(view.zoom() == doctest::Approx(widthZoom));
    QTest::keyClick(&view, Qt::Key_F);
    CHECK(!view.isFitWidth());
    CHECK(view.zoom() == doctest::Approx(pageZoom));
    QTest::keyClick(&view, Qt::Key_F);
    CHECK(view.isFitWidth());
}

TEST_CASE("reader: in select mode a drag from beside the text selects it")
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
    REQUIRE(view.tool() == ReaderTool::Select);
    // from the left margin, level with the first line, into it
    QPointF first = charCenter(view, 0);
    QPointF margin = view.pageToViewport(0, {30, 0});
    margin.setY(first.y());
    drag(view.viewport(), margin, charCenter(view, 10));
    REQUIRE(view.hasTextSelection());
    CHECK(view.selectedText().startsWith("Hello"));
    // a click beside the text still selects nothing
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, margin.toPoint());
    CHECK(!view.hasTextSelection());
}

TEST_CASE("main window: Ctrl+Shift+T reopens closed tabs, last closed first, where they were")
{
    app();
    Env env;
    TempDir tmp;
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    std::vector<QString> keys;
    for (int i = 0; i < 3; ++i) {
        auto file = tmp.path / ("paper" + std::to_string(i) + ".pdf");
        writeSamplePdf(file);
        auto parent = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Paper " + std::to_string(i)}});
        keys.push_back(qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key")));
        window.openAttachment(env.lib, keys.back());
    }
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    REQUIRE(tabs->count() == 4);
    auto titles = [&] {
        QStringList t;
        for (int i = 1; i < tabs->count(); ++i)
            t << tabs->tabText(i);
        return t;
    };
    // H on the page goes back to the library
    auto* reader = qobject_cast<ReaderTab*>(tabs->currentWidget());
    REQUIRE(reader);
    reader->view()->setFocus();
    QTest::keyClick(reader->view(), Qt::Key_H);
    CHECK(tabs->currentIndex() == 0);
    auto before = titles();
    emit tabs->tabCloseRequested(2);  // the middle one
    emit tabs->tabCloseRequested(2);  // then the last
    QApplication::processEvents();
    REQUIRE(tabs->count() == 2);
    QTest::keyClick(&window, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::processEvents();
    CHECK(tabs->count() == 3);
    CHECK(tabs->tabText(tabs->currentIndex()) == before[2]);  // the last closed comes back first
    QTest::keyClick(&window, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::processEvents();
    CHECK(titles() == before);  // each in its place
    QTest::keyClick(&window, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);  // nothing left
    CHECK(tabs->count() == 4);

    // another window starts with nothing to reopen
    MainWindow second;
    second.show();
    auto* secondTabs = second.findChild<QTabWidget*>("mainTabs");
    int count = secondTabs->count();
    QTest::keyClick(&second, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(secondTabs->count() == count);
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
    // rows that wrap reach the edge (their tabs share the space). The last one does too
    // when it is mostly full, else its tabs keep their natural widths; which it is
    // depends on the platform's font (on Linux it is nearly full here, on macOS and
    // Windows about half).
    {
        auto* area = buttons.first()->parentWidget();
        int lastRow = *std::max_element(rows.begin(), rows.end());
        for (int row : rows) {
            int right = 0;
            bool natural = true;
            for (auto* b : buttons)
                if (b->y() == row) {
                    right = std::max(right, b->geometry().right());
                    natural = natural && b->width() == b->sizeHint().width();
                }
            if (row != lastRow)
                CHECK(right == area->width() - 1);
            else
                CHECK((right == area->width() - 1 || natural));
        }
    }
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

TEST_CASE("library: with a band picture the list has no stripes, in light and dark mode")
{
    app();
    // macOS's style also paints alternating rows into the empty area below the items, as
    // Finder does; Fusion does not, so make it
    struct EmptyAreaStripes : QProxyStyle {
        int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                      QStyleHintReturn* data) const override
        {
            return hint == SH_ItemView_PaintAlternatingRowColorsForEmptyArea
                       ? 1
                       : QProxyStyle::styleHint(hint, option, widget, data);
        }
    };
    for (auto appearance : {Appearance::Light, Appearance::Dark}) {
        INFO((appearance == Appearance::Light ? "light" : "dark") << " mode");
        applyAppearance(appearance);
        Env env;
        env.config.set("/ui/bandBackground", true);  // off unless asked for
        auto bands = env.library.dataDir() / "backgrounds" / "bands";
        fs::create_directories(bands);
        QImage picture(100, 2000, QImage::Format_RGB32);  // fitted and centred: the whole height, mid-list
        picture.fill(Qt::red);
        REQUIRE(picture.save(qpath(bands / "band.png")));
        env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "One"}});
        env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Two"}});
        EmptyAreaStripes style;
        LibraryView view;
        view.itemsView()->setStyle(&style);
        view.resize(1200, 700);
        view.show();
        QApplication::processEvents();
        auto* items = view.itemsView();
        REQUIRE(items->model()->rowCount() == 2);
        CHECK(!items->alternatingRowColors());

        QRect first = items->visualRect(items->model()->index(0, 0));
        QRect second = items->visualRect(items->model()->index(1, 0));
        QImage shot = items->viewport()->grab().toImage();
        int x = items->viewport()->width() / 2;
        double dpr = shot.devicePixelRatio();
        auto at = [&](int y) { return shot.pixelColor(int(x * dpr), int(y * dpr)); };
        QColor row = at(first.center().y());
        CHECK(row.red() - row.green() > 8);  // the picture, faintly
        CHECK(at(second.center().y()) == row);
        for (int band = 1; band <= 4; ++band) {
            int y = second.center().y() + band * second.height();
            REQUIRE(y < items->viewport()->height());
            INFO("stripe " << band << " below the items");
            CHECK(at(y) == row);
        }

        // without a picture the rows alternate again
        env.config.set("/ui/bandBackground", false);
        emit AppContext::get().appearanceChanged();
        CHECK(items->alternatingRowColors());
        items->setStyle(nullptr);
    }
    applyAppearance(Appearance::System);
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

namespace {

// A widget with a given preferred size, free to grow sideways (like a tab).
struct Box : QWidget {
    QSize hint;
    explicit Box(QSize h, QWidget* parent) : QWidget(parent), hint(h)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override { return hint; }
};

} // namespace

TEST_CASE("flow layout: justified rows reach the edge; a short last row keeps its widths")
{
    app();
    auto layout = [](std::vector<int> widths) {
        auto* w = new QWidget;
        auto* flow = new FlowLayout(w, 0);
        flow->setJustified(true);
        std::vector<Box*> boxes;
        for (int width : widths) {
            boxes.push_back(new Box(QSize(width, 20), w));
            flow->addWidget(boxes.back());
        }
        flow->setGeometry(QRect(0, 0, 300, flow->heightForWidth(300)));
        return std::pair{std::unique_ptr<QWidget>(w), boxes};
    };
    {
        // 120 + 120 fill a row of 300 (the third does not fit): shared to 150 each; the
        // third alone is a 40 % last row and keeps its 120
        auto [w, b] = layout({120, 120, 120});
        CHECK(b[0]->geometry() == QRect(0, 0, 150, 20));
        CHECK(b[1]->geometry() == QRect(150, 0, 150, 20));
        CHECK(b[2]->geometry() == QRect(0, 20, 120, 20));
    }
    {
        // a last row 73 % full (120 + 100) is filled too, in proportion to the widths
        auto [w, b] = layout({120, 120, 120, 100});
        CHECK(b[2]->geometry().left() == 0);
        CHECK(b[3]->geometry().right() == 299);
        CHECK(b[2]->width() > b[3]->width());
    }
}

TEST_CASE("main window: the tab strip never shows a stray or misplaced tab")
{
    app();
    Env env;
    TempDir tmp;
    QStringList keys;
    for (int i = 0; i < 12; ++i) {
        auto file = tmp.path / ("paper" + std::to_string(i) + ".pdf");
        writeSamplePdf(file);
        auto parent = env.library.createItem(
            env.lib, Json{{"itemType", "journalArticle"}, {"title", "Quite a long paper title, number " + std::to_string(i)}});
        keys << qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key"));
    }
    // what the next frame shows: one round of events after a change
    auto consistent = [](MainWindow& window) {
        auto* tabs = window.findChild<QTabWidget*>("mainTabs");
        QList<QWidget*> shown;
        for (auto* b : window.findChild<QWidget*>("tabStrip")->findChildren<QWidget*>("tabButton"))
            if (b->isVisible())
                shown << b;
        if (shown.size() != tabs->count())
            return false;  // a tab too many (one on its way out) or too few
        auto* area = shown.first()->parentWidget();
        for (int i = 0; i < shown.size(); ++i) {
            auto g = shown[i]->geometry();
            if (g.right() >= area->width() || g.bottom() >= area->height() || g.left() < 0)
                return false;  // outside the strip, or in a row it has no room for yet
            for (int j = i + 1; j < shown.size(); ++j)
                if (g.intersects(shown[j]->geometry()))
                    return false;
        }
        return true;
    };
    {
        MainWindow window;
        window.resize(1000, 700);
        window.show();
        QApplication::processEvents();
        for (auto& k : keys) {
            window.openAttachment(env.lib, k);
            QApplication::processEvents();
            CHECK(consistent(window));
        }
        window.findChild<QTabWidget*>("mainTabs")->setCurrentIndex(5);
        window.close();
    }
    // restarted: the restored tabs, then the selected one loading, then another
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    CHECK(QTest::qWaitFor([&] { return tabs->count() == 13; }));
    QApplication::processEvents();
    CHECK(consistent(window));
    for (int i : {8, 2, 11}) {
        tabs->setCurrentIndex(i);  // a pending tab becomes a reader
        QApplication::processEvents();
        CHECK(consistent(window));
    }
    for (int i = 0; i < 4; ++i) {
        emit tabs->tabCloseRequested(tabs->count() - 2);
        QApplication::processEvents();
        CHECK(consistent(window));
    }
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    // On Windows the offscreen platform looks for fonts only in Qt's lib/fonts, which
    // does not exist: without fonts every character is a 12-pixel box, and every width
    // the tests check is too large. Use the system's fonts and Windows' UI font.
    if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", (qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts").toLocal8Bit());
#endif
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    QApplication::setFont(QFont("Segoe UI", 9));
#endif
    doctest::Context context(argc, argv);
    return context.run();
}
