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
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QToolBar>
#include <QListWidget>
#include <QFormLayout>
#include <QMenuBar>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTreeView>
#include <QUndoStack>
#include <QTreeWidget>

#include <array>
#include <future>
#include <sstream>
#include <httplib.h>
#include <atomic>
#include <QNativeGestureEvent>
#include <QTest>
#include <QTextDocument>
#include <QMimeData>
#include <QPushButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QMenu>
#include <QContextMenuEvent>
#include <QClipboard>
#include <QDropEvent>
#include <QPainter>
#include <QWheelEvent>
#ifdef OURNOTES_TEST_BACKING_STORE
#include <QBackingStore>
#include <qpa/qplatformbackingstore.h>
#endif

#include "../core/test_helpers.h"
#include "../pdf/sample_pdf.h"
#include "core/config.h"
#include "core/library.h"
#include "core/schema.h"
#include "core/sync/engine.h"
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
#include "gui/search_field.h"
#include "gui/storage_window.h"
#include "gui/shift_wheel.h"
#include "gui/preferences.h"
#include "gui/sync_controller.h"
#include "gui/sync_history.h"
#include "gui/theme.h"
#include "gui/ui_scale.h"
#include "gui/wrap_toolbar.h"
#include "gui/reader/annotation_sidebar.h"
#include "gui/reader/comment_bubble.h"
#include "gui/reader/pdf_view.h"
#include "gui/reader/reader_tab.h"
#include "sync/local_transport.h"

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

TEST_CASE("reader: the paper's info is beside the PDF, next to its notes")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(
        env.lib, Json{{"itemType", "journalArticle"},
                      {"title", "Deep learning"},
                      {"date", "2015"},
                      {"creators", Json::array({Json{{"creatorType", "author"}, {"firstName", "Yann"}, {"lastName", "LeCun"}}})}});
    auto key = qs(jstr(env.library.importAttachment(env.lib, file, jstr(paper, "key")), "key"));
    auto tabsOf = [](ReaderTab& tab) { return tab.findChild<QTabWidget*>("readerContextTabs"); };
    auto shows = [](ReaderTab& tab, const QString& text) {
        for (auto* e : tab.findChildren<QLineEdit*>())
            if (e->text() == text)
                return true;
        for (auto* e : tab.findChildren<QPlainTextEdit*>())
            if (e->toPlainText() == text)
                return true;
        return false;
    };
    {
        ReaderTab tab(env.lib, key);  // no notes: Info
        auto* tabs = tabsOf(tab);
        REQUIRE(tabs);
        CHECK(tabs->currentIndex() == 0);
        CHECK(tabs->tabText(0) == "Info");
        CHECK(shows(tab, "Deep learning"));
        CHECK(shows(tab, "LeCun"));
        CHECK(shows(tab, "2015"));
    }
    env.library.createNote(env.lib, "<p>Thoughts</p>", jstr(paper, "key"));
    {
        ReaderTab tab(env.lib, key);  // with a note, too: Info, until Notes is chosen
        auto* tabs = tabsOf(tab);
        CHECK(tabs->currentIndex() == 0);
        tab.show();
        QApplication::processEvents();
        QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, {}, tabs->tabBar()->tabRect(1).center());
        CHECK(tabs->currentIndex() == 1);
    }
    {
        ReaderTab tab(env.lib, key);
        CHECK(tabsOf(tab)->currentIndex() == 1);
    }
    // a PDF of its own has no paper to show
    env.config.set("/ui/itemPaneTab", "info");
    auto alone = qs(jstr(env.library.importAttachment(env.lib, file, ""), "key"));
    ReaderTab tab(env.lib, alone);
    CHECK(!tabsOf(tab)->isTabEnabled(0));
    CHECK(tabsOf(tab)->currentIndex() == 1);
}

TEST_CASE("main window: Info or Notes is one choice, for every item and every PDF")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto noted = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "With a note"}});
    env.library.createNote(env.lib, "<p>Thoughts</p>", jstr(noted, "key"));
    auto plain = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Without"}});
    auto notedPdf = qs(jstr(env.library.importAttachment(env.lib, file, jstr(noted, "key")), "key"));
    auto plainPdf = qs(jstr(env.library.importAttachment(env.lib, file, jstr(plain, "key")), "key"));
    MainWindow window;
    window.resize(1400, 800);
    window.show();
    window.openAttachment(env.lib, notedPdf);
    window.openAttachment(env.lib, plainPdf);
    QApplication::processEvents();
    auto* mainTabs = window.findChild<QTabWidget*>("mainTabs");
    std::vector<QTabWidget*> readers;
    for (int i = 1; i < mainTabs->count(); ++i)
        readers.push_back(mainTabs->widget(i)->findChild<QTabWidget*>("readerContextTabs"));
    REQUIRE(readers.size() == 2);
    auto* pane = window.findChild<ItemPane*>();
    auto* library = pane->findChild<QTabWidget*>();
    auto click = [&](QTabWidget* tabs, int index) {  // on the tab bar, in its tab of the window
        for (int i = 0; i < mainTabs->count(); ++i)
            if (mainTabs->widget(i)->isAncestorOf(tabs))
                mainTabs->setCurrentIndex(i);
        QApplication::processEvents();
        QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, {}, tabs->tabBar()->tabRect(index).center());
        QApplication::processEvents();
    };
    auto shown = [&] {
        std::set<int> s{library->currentIndex()};
        for (auto* r : readers)
            s.insert(r->currentIndex());
        return s;
    };
    pane->showItems(env.lib, {qs(jstr(plain, "key"))});
    CHECK(shown() == std::set<int>{0});
    // Notes in one PDF: Notes everywhere, also for the item without notes
    click(readers[0], 1);
    CHECK(shown() == std::set<int>{1});
    for (auto& item : {noted, plain, noted}) {
        pane->showItems(env.lib, {qs(jstr(item, "key"))});
        CHECK(library->currentIndex() == 1);
    }
    // Info in the library: Info everywhere, and for a PDF opened later
    click(library, 0);
    CHECK(shown() == std::set<int>{0});
    CHECK(env.config.get("/ui/itemPaneTab", "").get<std::string>() == "info");
    window.openAttachment(env.lib, notedPdf);  // already open: its tab
    QApplication::processEvents();
    CHECK(readers[0]->currentIndex() == 0);
    // closing a tab leaves the choice as it was
    click(readers[1], 1);
    mainTabs->setCurrentIndex(2);
    QTest::keyClick(&window, Qt::Key_W, Qt::ControlModifier);
    QApplication::processEvents();
    CHECK(mainTabs->count() == 2);
    CHECK(env.config.get("/ui/itemPaneTab", "").get<std::string>() == "notes");
    CHECK(readers[0]->currentIndex() == 1);
    CHECK(library->currentIndex() == 1);
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
    CHECK(toggle->isVisible());  // (dark pages in a light theme too, see the next test)
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

TEST_CASE("reader: L turns the pages dark or light, the page colour tints them, in every PDF")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key"));
    applyAppearance(Appearance::Light);
    ReaderTab a(env.lib, key), b(env.lib, key);
    for (auto* t : {&a, &b}) {
        t->resize(1200, 900);
        t->show();
    }
    auto paper = [](ReaderTab& t) {  // a blank spot on the page
        auto img = t.view()->viewport()->grab().toImage();
        return img.pixelColor((t.view()->pageToViewport(0, {500, 500}) * img.devicePixelRatio()).toPoint());
    };
    auto* tone = a.findChild<QSlider*>("pageTone");
    REQUIRE(tone);
    REQUIRE(!a.view()->darkPages());
    CHECK(QTest::qWaitFor([&] { return paper(a) == QColor(Qt::white); }));

    // L on the page: dark pages, here and in the other PDF; the application stays light
    a.view()->setFocus();
    QTest::keyClick(a.view(), Qt::Key_L);
    CHECK(a.view()->darkPages());
    CHECK(b.view()->darkPages());
    CHECK(!isDarkTheme());
    CHECK(env.config.get("/reader/darkPagesInLightMode", false).get<bool>());
    CHECK(darkPageColor() == QColor(Qt::black));
    CHECK(QTest::qWaitFor([&] { return paper(b) == QColor(Qt::black); }));
    // the page colour: black to gray, for every PDF
    tone->setValue(100);
    CHECK(b.findChild<QSlider*>("pageTone")->value() == 100);
    CHECK(paper(b) == QColor(128, 128, 128));

    // light pages again: their own page colour, white to gray
    QTest::keyClick(a.view(), Qt::Key_L);
    CHECK(!a.view()->darkPages());
    CHECK(tone->value() == 0);
    CHECK(QTest::qWaitFor([&] { return paper(a) == QColor(Qt::white); }));
    tone->setValue(50);
    CHECK(paper(a) == QColor(191, 191, 191));
    CHECK(paper(b) == QColor(191, 191, 191));
    CHECK(env.config.get("/reader/pageTone/light", 0).get<int>() == 50);
    // and the dark pages kept theirs
    QTest::keyClick(a.view(), Qt::Key_L);
    CHECK(tone->value() == 100);
    QTest::keyClick(a.view(), Qt::Key_L);
    applyAppearance(Appearance::System);
}

TEST_CASE("reader: [ and ] show or hide the panels, I and N open Info and Notes")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key"));
    ReaderTab tab(env.lib, key);
    tab.resize(1200, 900);
    tab.show();
    QApplication::processEvents();
    auto* view = tab.view();
    view->setFocus();
    auto* left = tab.findChild<AnnotationSidebar*>();
    auto* right = tab.findChild<QTabWidget*>("readerContextTabs");
    REQUIRE(left);
    REQUIRE(right);
    CHECK(left->isVisible());
    CHECK(right->isVisible());
    QTest::keyClick(view, Qt::Key_BracketLeft);  // each press: hidden, shown
    CHECK(!left->isVisible());
    CHECK(right->isVisible());
    QTest::keyClick(view, Qt::Key_BracketLeft);
    CHECK(left->isVisible());
    QTest::keyClick(view, Qt::Key_BracketRight);
    CHECK(!right->isVisible());
    QTest::keyClick(view, Qt::Key_BracketRight);
    CHECK(right->isVisible());
    // N: Notes, the panel shown if it was hidden; I: Info; the choice is everyone's
    QTest::keyClick(view, Qt::Key_BracketRight);
    QTest::keyClick(view, Qt::Key_N);
    CHECK(right->isVisible());
    CHECK(right->currentIndex() == 1);
    CHECK(env.config.get("/ui/itemPaneTab", "").get<std::string>() == "notes");
    QTest::keyClick(view, Qt::Key_I);
    CHECK(right->currentIndex() == 0);
    CHECK(view->hasFocus());  // the page keeps the keys
}

TEST_CASE("reader: Q, W and E give three colours of one's own, set with the gear, in every PDF")
{
    app();
    Env env;
    TempDir tmp;
    QStringList keys;
    for (auto name : {"one.pdf", "two.pdf"}) {
        auto file = tmp.path / name;
        writeSamplePdf(file);
        keys << qs(jstr(env.library.importAttachment(env.lib, file), "key"));
    }
    ReaderTab tab(env.lib, keys[0]), other(env.lib, keys[1]);
    tab.resize(1200, 900);
    tab.show();
    QApplication::processEvents();
    auto* view = tab.view();
    view->setFocus();
    CHECK(annotationPalette() == QStringList{"#ffd400", "#ff6666", "#5fb236"});  // yellow, red, green
    for (auto [key, color] : {std::pair{Qt::Key_W, "#ff6666"}, {Qt::Key_E, "#5fb236"}, {Qt::Key_Q, "#ffd400"}}) {
        QTest::keyClick(view, key);
        CHECK(view->color() == color);
        CHECK(env.config.get("/reader/color", "").get<std::string>() == color);  // kept, as when picked
    }

    // the gear: W becomes blue, here and in the other PDF at once
    auto paletteMenu = [](ReaderTab& t, const char* name) {
        return t.findChild<QToolButton*>("colorPalette")->menu()->findChild<QMenu*>(name);
    };
    REQUIRE(paletteMenu(tab, "paletteW"));
    CHECK(paletteMenu(tab, "paletteW")->title() == "W: Red");
    QAction* blue = nullptr;
    for (auto* a : paletteMenu(tab, "paletteW")->actions())
        if (a->text() == "Blue")
            blue = a;
    REQUIRE(blue);
    blue->trigger();
    CHECK(env.config.get("/reader/palette", Json())[1] == "#2ea8e5");
    CHECK(paletteMenu(tab, "paletteW")->title() == "W: Blue");
    CHECK(blue->isChecked());
    CHECK(paletteMenu(other, "paletteW")->title() == "W: Blue");
    other.view()->setFocus();
    QTest::keyClick(other.view(), Qt::Key_W);
    CHECK(other.view()->color() == "#2ea8e5");

    // typed into a field, a q is just a q
    auto* find = tab.findChild<SearchField*>("findInDocument");
    find->setFocus();
    QTest::keyClick(find, Qt::Key_W);
    CHECK(find->text() == "w");
    CHECK(view->color() == "#ffd400");

    // and back to yellow, red and green
    for (auto* a : tab.findChild<QToolButton*>("colorPalette")->menu()->actions())
        if (a->text() == "Back to Yellow, Red and Green")
            a->trigger();
    CHECK(annotationPalette() == QStringList{"#ffd400", "#ff6666", "#5fb236"});
    CHECK(paletteMenu(other, "paletteW")->title() == "W: Red");
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        tab.findChild<QToolBar*>()->grab().save(dir + "/reader-toolbar.png");
}

TEST_CASE("reader: on a page the tool's icon is the cursor; Select keeps the usual ones")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    ReaderTab tab(env.lib, qs(jstr(env.library.importAttachment(env.lib, file), "key")));
    tab.resize(1200, 900);
    tab.show();
    QApplication::processEvents();
    auto* view = tab.view();
    view->setFocus();
    QPoint onPage = view->pageToViewport(0, QPointF(400, 600)).toPoint();
    REQUIRE(view->viewport()->rect().contains(onPage));
    auto cursorAt = [&](QPoint at) {
        QTest::mouseMove(view->viewport(), at + QPoint(1, 0));
        QTest::mouseMove(view->viewport(), at);
        return view->viewport()->cursor();
    };
    CHECK(cursorAt(onPage).shape() == Qt::ArrowCursor);
    std::set<qint64> icons;
    for (auto key : {Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5, Qt::Key_6, Qt::Key_7, Qt::Key_8}) {
        QTest::keyClick(view, key);
        auto cursor = cursorAt(onPage);
        CHECK(cursor.shape() == Qt::BitmapCursor);
        CHECK(!cursor.pixmap().isNull());
        icons.insert(cursor.pixmap().cacheKey());
    }
    CHECK(icons.size() == 7);  // each its own
    QTest::keyClick(view, Qt::Key_2);
    CHECK(cursorAt(onPage).hotSpot() == QPoint(3, 21));  // the marker's tip
    // off the pages, and with Select: as before
    CHECK(cursorAt(QPoint(2, 2)).shape() == Qt::ArrowCursor);
    QTest::keyClick(view, Qt::Key_1);
    CHECK(cursorAt(onPage).shape() == Qt::ArrowCursor);
    CHECK(cursorAt(charCenter(*view, 1).toPoint()).shape() == Qt::IBeamCursor);
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty()) {
        QTest::keyClick(view, Qt::Key_2);
        cursorAt(onPage).pixmap().save(dir + "/cursor-highlight.png");
    }
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

// A format whose font families are an empty list is not the note's font: Qt then picks a
// fallback, on macOS Apple Color Emoji, whose spaces and digits are three or four times as
// wide and whose lines are taller.
static bool usesNoteFont(const QTextCharFormat& f) { return !f.hasProperty(QTextFormat::FontFamilies); }

TEST_CASE("notes: text copied from a note pastes as it was shown: font, spaces, lines")
{
    app();
    Env env;
    auto from = env.library.createNote(
        env.lib, "<div data-schema-version=\"9\"><p>Test</p><ol><li>在note 内选中复制文本, 请复制源码, paste到note "
                 "也要和被选中的显示一样. 现在在macos上面选中再复制会乱; 图片的话可以直接复制.</li><li>有时候note 1234</li></ol></div>");
    auto to = env.library.createNote(env.lib, "<p></p>");
    NoteEditor a, b;
    for (auto* e : {&a, &b}) {
        e->resize(600, 400);
        e->show();
    }
    a.setNote(env.lib, qs(jstr(from, "key")));
    b.setNote(env.lib, qs(jstr(to, "key")));
    QApplication::processEvents();
    auto* source = a.findChild<NoteTextEdit*>();
    auto* target = b.findChild<NoteTextEdit*>();
    source->selectAll();
    source->copy();
    target->setFocus();
    target->paste();
    QApplication::processEvents();

    auto blocks = [](QTextDocument* d) {
        std::vector<QTextBlock> v;
        for (auto block = d->begin(); block.isValid(); block = block.next())
            v.push_back(block);
        return v;
    };
    auto was = blocks(source->document()), now = blocks(target->document());
    REQUIRE(was.size() == now.size());
    for (size_t i = 0; i < was.size(); ++i) {
        INFO("paragraph " << i << ": " << was[i].text().toStdString());
        CHECK(now[i].text() == was[i].text());
        for (auto it = now[i].begin(); !it.atEnd(); ++it)
            CHECK(usesNoteFont(it.fragment().charFormat()));
        // laid out alike: as many lines, as wide and as high
        auto* before = was[i].layout();
        auto* after = now[i].layout();
        REQUIRE(after->lineCount() == before->lineCount());
        for (int l = 0; l < before->lineCount(); ++l) {
            CHECK(after->lineAt(l).naturalTextWidth() == doctest::Approx(before->lineAt(l).naturalTextWidth()));
            CHECK(after->lineAt(l).height() == doctest::Approx(before->lineAt(l).height()));
        }
    }
}

TEST_CASE("notes: the next paragraph is as far below as the next line, opened, pasted or typed")
{
    app();
    Env env;
    const QString longText = "A paragraph long enough to take two lines in a narrow note, so that the room "
                             "between its own lines can be measured against the room between paragraphs.";
    auto note = env.library.createNote(
        env.lib, qs("<div data-schema-version=\"9\"><h2>Title</h2><p>" + longText.toStdString() +
                    "</p><p>One</p><p>Two</p><ul><li>a</li><li>b</li></ul><blockquote><p>Quoted</p></blockquote><p>After</p></div>")
                     .toStdString());
    NoteEditor editor;
    editor.resize(420, 500);
    editor.show();
    editor.setNote(env.lib, qs(jstr(note, "key")));
    QApplication::processEvents();
    auto* edit = editor.findChild<NoteTextEdit*>();
    auto* layout = edit->document()->documentLayout();
    // from one line's top to the next one's: within a paragraph, and across into the next
    auto lineTops = [&] {
        std::vector<double> tops;
        for (auto block = edit->document()->begin(); block.isValid(); block = block.next()) {
            double y = layout->blockBoundingRect(block).top();
            for (int i = 0; i < block.layout()->lineCount(); ++i)
                tops.push_back(y + block.layout()->lineAt(i).y());
        }
        return tops;
    };
    auto paragraph = edit->document()->begin().next();
    REQUIRE(paragraph.text() == longText);
    REQUIRE(paragraph.layout()->lineCount() >= 2);
    double line = paragraph.layout()->lineAt(1).y() - paragraph.layout()->lineAt(0).y();
    auto everyLine = [&](const char* what) {
        auto tops = lineTops();
        // past the heading (its line is taller): every step is one line of text
        for (size_t i = 2; i < tops.size(); ++i) {
            INFO(std::string(what) << ", line " << i);
            CHECK(tops[i] - tops[i - 1] == doctest::Approx(line));
        }
        return tops.size();
    };
    size_t lines = everyLine("as opened");
    // the heading's own line is no taller than its text
    auto title = edit->document()->begin();
    CHECK(layout->blockBoundingRect(title).height() == doctest::Approx(title.layout()->lineAt(0).height()));

    // pasted: from another program's HTML with Qt's 12 px around paragraphs, and Markdown
    edit->moveCursor(QTextCursor::End);
    auto* html = new QMimeData;
    html->setHtml("<p style=\"margin-top:12px; margin-bottom:12px;\">Pasted one</p><p style=\"margin-top:12px; "
                  "margin-bottom:12px;\">Pasted two</p>");
    QApplication::clipboard()->setMimeData(html);
    QTest::keyClick(edit, Qt::Key_Return);
    edit->paste();
    auto* markdown = new QMimeData;
    markdown->setText("# Pasted heading\n\nParagraph.\n\n- one\n- two\n");
    QApplication::clipboard()->setMimeData(markdown);
    QTest::keyClick(edit, Qt::Key_Return);
    edit->paste();
    QApplication::processEvents();
    // a heading typed in Markdown, and the line after it
    QTest::keyClick(edit, Qt::Key_Return);
    QTest::keyClicks(edit, "## Typed");
    QTest::keyClick(edit, Qt::Key_Return);
    QTest::keyClicks(edit, "text");
    QApplication::processEvents();
    auto tops = lineTops();
    REQUIRE(tops.size() > lines + 6);
    for (auto block = edit->document()->begin(); block.isValid(); block = block.next()) {
        INFO(block.text().toStdString());
        CHECK(block.blockFormat().topMargin() == 0);
        CHECK(block.blockFormat().bottomMargin() == 0);
        // every block is as high as its lines, with nothing around them
        double linesHeight = 0;
        for (int i = 0; i < block.layout()->lineCount(); ++i)
            linesHeight += block.layout()->lineAt(i).height();
        CHECK(layout->blockBoundingRect(block).height() == doctest::Approx(linesHeight));
    }
}

TEST_CASE("notes: a formula has the colour of the text it is typed in, kept when saved")
{
    app();
    Env env;
    auto note = env.library.createNote(env.lib, "<p></p>");
    QColor green("#00aa00");
    // where the formula is, and whether it is drawn in green
    auto formulaIn = [](NoteTextEdit* edit) -> std::optional<QTextFragment> {
        for (auto block = edit->document()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().isImageFormat() &&
                    parseMathResourceName(it.fragment().charFormat().toImageFormat().name(), nullptr, nullptr))
                    return it.fragment();
        return std::nullopt;
    };
    auto drawnGreen = [](NoteTextEdit* edit, const QTextFragment& formula) {
        QTextCursor at(edit->document());
        at.setPosition(formula.position());
        QRect from = edit->cursorRect(at);
        at.setPosition(formula.position() + 1);
        QRect to = edit->cursorRect(at);
        QImage shot = edit->viewport()->grab().toImage();
        double dpr = shot.devicePixelRatio();
        int greenish = 0;
        for (int x = from.left(); x < to.left(); ++x)
            for (int y = from.top(); y <= from.bottom(); ++y) {
                QColor c = shot.pixelColor(int(x * dpr), int(y * dpr));
                if (c.green() > c.red() + 60 && c.green() > c.blue() + 60)
                    ++greenish;
            }
        return greenish > 5;
    };
    {
        NoteEditor editor;
        editor.resize(600, 300);
        editor.show();
        editor.setNote(env.lib, qs(jstr(note, "key")));
        auto* edit = editor.findChild<NoteTextEdit*>();
        edit->setFocus();
        QApplication::processEvents();
        edit->setTextColor(green);
        QTest::keyClicks(edit, "a $x^2+y$ b");
        QApplication::processEvents();
        auto formula = formulaIn(edit);
        REQUIRE(formula);
        CHECK(formula->charFormat().foreground().color() == green);
        CHECK(drawnGreen(edit, *formula));
        editor.flush();
    }
    auto html = jstr(*env.library.item(env.lib, jstr(note, "key")), "note");
    CHECK(html.find("<span style=\"color: #00aa00\"><span class=\"math\">$x^2+y$</span></span>") != std::string::npos);
    {
        NoteEditor editor;  // opened again
        editor.resize(600, 300);
        editor.show();
        editor.setNote(env.lib, qs(jstr(note, "key")));
        QApplication::processEvents();
        auto* edit = editor.findChild<NoteTextEdit*>();
        auto formula = formulaIn(edit);
        REQUIRE(formula);
        CHECK(formula->charFormat().foreground().color() == green);
        CHECK(drawnGreen(edit, *formula));
    }

    // a formula on a line of its own, too
    auto own = env.library.createNote(
        env.lib, "<div data-schema-version=\"9\"><p>before</p><pre class=\"math\" style=\"color: #00aa00\">$$a+b$$</pre></div>");
    {
        NoteEditor editor;
        editor.resize(600, 300);
        editor.show();
        editor.setNote(env.lib, qs(jstr(own, "key")));
        QApplication::processEvents();
        auto* edit = editor.findChild<NoteTextEdit*>();
        auto formula = formulaIn(edit);
        REQUIRE(formula);
        CHECK(formula->charFormat().foreground().color() == green);
        CHECK(drawnGreen(edit, *formula));
        edit->moveCursor(QTextCursor::Start);
        QTest::keyClicks(edit, "x");
        editor.flush();
    }
    CHECK(jstr(*env.library.item(env.lib, jstr(own, "key")), "note").find("<pre class=\"math\" style=\"color: #00aa00\">$$a+b$$</pre>") !=
          std::string::npos);

    // in dark mode a black formula is shown readable, and still saved black
    applyAppearance(Appearance::Dark);
    auto dark = env.library.createNote(
        env.lib, "<div data-schema-version=\"9\"><p>z <span style=\"color: #000000\"><span class=\"math\">$y$</span></span></p></div>");
    {
        NoteEditor editor;
        editor.resize(600, 300);
        editor.show();
        editor.setNote(env.lib, qs(jstr(dark, "key")));
        QApplication::processEvents();
        auto* edit = editor.findChild<NoteTextEdit*>();
        auto formula = formulaIn(edit);
        REQUIRE(formula);
        CHECK(!formula->charFormat().hasProperty(QTextFormat::ForegroundBrush));  // the formula colour, readable
        QTest::keyClicks(edit, "!");
        editor.flush();
    }
    CHECK(jstr(*env.library.item(env.lib, jstr(dark, "key")), "note").find("<span style=\"color: #000000\"><span class=\"math\">$y$</span></span>") !=
          std::string::npos);
    applyAppearance(Appearance::System);
}

TEST_CASE("notes: after `code`, what is typed next is in the note's font again")
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
    QTest::keyClicks(edit, "run `make` then 1 2");
    auto block = edit->document()->begin();
    REQUIRE(block.text() == "run make then 1 2");
    QTextCursor c(edit->document());
    c.setPosition(block.position() + block.text().indexOf("make") + 1);
    CHECK(c.charFormat().fontFixedPitch());  // the code
    c.setPosition(block.position() + block.text().indexOf("then") + 1);
    CHECK(usesNoteFont(c.charFormat()));
    CHECK(usesNoteFont(edit->currentCharFormat()));
}

TEST_CASE("notes: a tab is as wide as four spaces, at any text size")
{
    app();
    Env env;
    auto note = env.library.createNote(env.lib, "<p></p>");
    NoteEditor editor;
    editor.resize(600, 400);
    editor.show();
    editor.setNote(env.lib, qs(jstr(note, "key")));
    auto* edit = editor.findChild<NoteTextEdit*>();
    for (int zoom : {100, 150}) {
        editor.setZoom(zoom);
        INFO("zoom " << zoom);
        CHECK(edit->tabStopDistance() == doctest::Approx(4 * QFontMetricsF(edit->font()).horizontalAdvance(' ')));
    }
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
    CHECK(tabs->currentIndex() == 0);  // Info, until Notes is chosen
    CHECK(tabs->tabText(1) == "Notes (2)");
    QApplication::processEvents();
    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, {}, tabs->tabBar()->tabRect(1).center());
    CHECK(tabs->currentIndex() == 1);
    auto* panel = pane.findChild<ItemNotesPanel*>();
    REQUIRE(panel);
    CHECK(panel->count() == 2);
    CHECK(!panel->editor()->key().isEmpty());

    pane.showItems(env.lib, {qs(jstr(bare, "key"))});
    CHECK(tabs->currentIndex() == 1);  // Notes still, with none to show
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

TEST_CASE("item pane: a long field shows its start")
{
    app();
    Env env;
    auto venue = std::string("Advances in Neural Information Processing Systems");
    auto item = env.library.createItem(
        env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}, {"publicationTitle", venue},
                      {"creators", Json::array({Json{{"creatorType", "author"}, {"firstName", "Ashish"},
                                                     {"lastName", "Vaswani-Shazeer-Parmar-Uszkoreit"}}})}});
    ItemPane pane;
    pane.resize(260, 700);
    pane.show();
    pane.showItems(env.lib, {qs(jstr(item, "key"))});
    QApplication::processEvents();
    int checked = 0;
    for (auto* e : pane.findChildren<QLineEdit*>())
        if (e->text() == qs(venue) || e->text().startsWith("Vaswani")) {
            CHECK(e->cursorPosition() == 0);
            ++checked;
        }
    CHECK(checked == 2);
}

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

TEST_CASE("notes: a formula left empty keeps its signs, and opens again between them")
{
    app();
    NoteTextEdit edit;
    edit.resize(500, 300);
    edit.show();
    const QString formula(QChar::ObjectReplacementCharacter);
    QTest::keyClicks(&edit, "Intro");
    QTest::keyClick(&edit, Qt::Key_Return);
    QTest::keyClicks(&edit, "$$");  // on an empty line: $$|$$
    CHECK(mathSource(edit.document()) == "$$$$");
    QTest::keyClick(&edit, Qt::Key_Up);  // somewhere else: the signs stay, as text
    CHECK(mathSource(edit.document()).isEmpty());
    CHECK(countMath(edit.document()) == 0);
    CHECK(edit.toPlainText() == "Intro\n$$$$");
    CHECK(!documentToNoteHtml(edit.document()).contains("math"));
    // back between them, it is a formula again: typed into and left, rendered
    QTest::keyClick(&edit, Qt::Key_End, Qt::ControlModifier);
    QTest::keyClick(&edit, Qt::Key_Left);
    CHECK(mathSource(edit.document()).isEmpty());  // ($$$|$: not between them)
    QTest::keyClick(&edit, Qt::Key_Left);
    CHECK(mathSource(edit.document()) == "$$$$");
    CHECK(documentToNoteHtml(edit.document()).contains("$$$$"));  // saved as it shows
    QTest::keyClicks(&edit, "a^2");
    QTest::keyClick(&edit, Qt::Key_Up);
    QStringList latex;
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"a^2"});
    QTest::keyClick(&edit, Qt::Key_Z, Qt::ControlModifier);  // one step: back to the signs
    CHECK(countMath(edit.document()) == 0);
    CHECK(edit.toPlainText() == "Intro\n$$$$");

    // in a line, $$ is an inline formula, $|$; left empty, "$$" stays; a click between
    // the signs opens it
    edit.clear();
    QTest::keyClicks(&edit, "Area $$");
    CHECK(mathSource(edit.document()) == "$$");
    QTest::keyClick(&edit, Qt::Key_Home);
    CHECK(mathSource(edit.document()).isEmpty());
    CHECK(edit.toPlainText() == "Area $$");
    QTextCursor between(edit.document());
    between.setPosition(6);
    QTest::mouseClick(edit.viewport(), Qt::LeftButton, {}, edit.cursorRect(between).center());
    REQUIRE(edit.textCursor().position() == 6);
    CHECK(mathSource(edit.document()) == "$$");
    QTest::keyClicks(&edit, "r$");
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"r"});
    CHECK(edit.toPlainText() == "Area " + formula);
}

TEST_CASE("notes: pasting into a formula just opened makes it at once")
{
    app();
    NoteTextEdit edit;
    edit.resize(500, 300);
    edit.show();
    QStringList latex;
    QApplication::clipboard()->setText("\\frac{a}{b}");
    QTest::keyClicks(&edit, "$$");
    QTest::keyClick(&edit, Qt::Key_V, Qt::ControlModifier);
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"\\frac{a}{b}"});
    CHECK(mathSource(edit.document()).isEmpty());
    CHECK((edit.document()->begin().blockFormat().alignment() & Qt::AlignHCenter) != 0);
    CHECK(edit.textCursor().blockNumber() == 1);  // on the next line, as after typing $$

    // in a line, an inline one; the pasted text's own $ signs are not doubled
    edit.clear();
    QApplication::clipboard()->setText("$x_1$");
    QTest::keyClicks(&edit, "Let $$");
    QTest::keyClick(&edit, Qt::Key_V, Qt::ControlModifier);
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"x_1"});
    CHECK(edit.toPlainText() == "Let " + QString(QChar::ObjectReplacementCharacter));

    // into a formula already being typed: part of it, still being typed
    edit.clear();
    QApplication::clipboard()->setText("+b");
    QTest::keyClicks(&edit, "Let $$a");
    QTest::keyClick(&edit, Qt::Key_V, Qt::ControlModifier);
    CHECK(countMath(edit.document()) == 0);
    CHECK(mathSource(edit.document()) == "$a+b$");
}

TEST_CASE("notes: a typed $ closes the formula the $$ or $ before it opened")
{
    app();
    NoteTextEdit edit;
    edit.resize(500, 300);
    edit.show();
    const QString formula(QChar::ObjectReplacementCharacter);
    QStringList latex;
    // "$$" before and one $ typed: a display formula; the second $ typed is not text
    edit.setPlainText("Then $$\\sum x");
    edit.moveCursor(QTextCursor::End);
    QTest::keyClicks(&edit, "$");
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"\\sum x"});
    QTest::keyClicks(&edit, "$ ok");
    CHECK(edit.toPlainText() == "Then " + formula + " ok");
    CHECK(edit.document()->blockCount() == 1);  // in its line, which is not split

    // the whole line: centred, on a line of its own
    edit.setPlainText("$$z");
    edit.moveCursor(QTextCursor::End);
    QTest::keyClicks(&edit, "$$");
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"z"});
    CHECK((edit.document()->begin().blockFormat().alignment() & Qt::AlignHCenter) != 0);
    CHECK(edit.document()->blockCount() == 2);
    CHECK(edit.document()->lastBlock().text().isEmpty());

    // "$$x$$" typed in a line: an inline formula, and no $ left over
    edit.clear();
    QTest::keyClicks(&edit, "so $$x$$ then");
    latex.clear();
    CHECK(countMath(edit.document(), &latex) == 1);
    CHECK(latex == QStringList{"x"});
    CHECK(edit.toPlainText() == "so " + formula + " then");
}

TEST_CASE("notes: Backspace at a heading's start shows its #s; the line left, a heading again")
{
    app();
    Env env;
    auto note = env.library.createNote(env.lib, "<h2>Results</h2><p>text</p>");
    NoteEditor editor;
    editor.resize(600, 500);
    editor.show();
    editor.setNote(env.lib, qs(jstr(note, "key")));
    auto* edit = editor.findChild<NoteTextEdit*>();
    REQUIRE(edit);
    edit->setFocus();
    auto* doc = edit->document();
    REQUIRE(doc->begin().blockFormat().headingLevel() == 2);
    edit->moveCursor(QTextCursor::Start);
    QTest::keyClick(edit, Qt::Key_Backspace);
    CHECK(doc->begin().text() == "## Results");
    CHECK(doc->begin().blockFormat().headingLevel() == 0);
    CHECK(edit->textCursor().position() == 3);  // after the "## "
    // left unchanged: the same heading
    QTest::keyClick(edit, Qt::Key_Down);
    CHECK(doc->begin().text() == "Results");
    CHECK(doc->begin().blockFormat().headingLevel() == 2);
    // one # less: a level up
    edit->moveCursor(QTextCursor::Start);
    QTest::keyClick(edit, Qt::Key_Backspace);
    edit->moveCursor(QTextCursor::Start);
    QTest::keyClick(edit, Qt::Key_Delete);
    CHECK(doc->begin().text() == "# Results");
    QTest::keyClick(edit, Qt::Key_Down);
    CHECK(doc->begin().text() == "Results");
    CHECK(doc->begin().blockFormat().headingLevel() == 1);
    // the space after the #s deleted: text, as it is
    edit->moveCursor(QTextCursor::Start);
    QTest::keyClick(edit, Qt::Key_Backspace);
    QTest::keyClick(edit, Qt::Key_Backspace);
    CHECK(doc->begin().text() == "#Results");
    QTest::keyClick(edit, Qt::Key_Down);
    CHECK(doc->begin().text() == "#Results");
    CHECK(doc->begin().blockFormat().headingLevel() == 0);
    CHECK(documentToNoteHtml(doc).contains("<p>#Results</p>"));
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

    // a formula left empty leaves its signs, as text
    QTest::mouseClick(edit.viewport(), Qt::LeftButton, {}, formulaCenter(edit));
    for (int i = 0; i < 5; ++i)
        QTest::keyClick(&edit, Qt::Key_Backspace);
    CHECK(mathSource(edit.document()) == "$$");
    QTest::keyClick(&edit, Qt::Key_Return);
    CHECK(countMath(edit.document()) == 0);
    CHECK(mathSource(edit.document()).isEmpty());
    CHECK(edit.toPlainText() == "Let $$ be.");
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

TEST_CASE("appearance: Follow system follows the system turning light or dark")
{
    app();
    Env env;
    auto styleBefore = QApplication::style()->name();
    int changes = 0;
    QObject::connect(&env.context, &AppContext::appearanceChanged, [&] { ++changes; });
    applyAppearance(Appearance::Dark);
    emit QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Light);
    QApplication::processEvents();
    CHECK(changes == 0);  // Dark chosen: the system's change does not matter
    applyAppearance(Appearance::System);
    QApplication::processEvents();
    changes = 0;
    emit QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    CHECK(changes == 1);  // PDF pages, notes and the rest drawn again in the new colours
#ifdef Q_OS_WIN
    // Windows' own styles look unlike Light and Dark (windowsvista has no dark look) and
    // cover the picture behind the item list: there Follow system is Light or Dark
    bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    applyAppearance(dark ? Appearance::Dark : Appearance::Light);
    auto chosen = QApplication::palette().color(QPalette::Window);
    applyAppearance(Appearance::System);
    CHECK(QApplication::style()->name().compare("fusion", Qt::CaseInsensitive) == 0);
    CHECK(QApplication::palette().color(QPalette::Window) == chosen);
    QApplication::setStyle(QStyleFactory::create(styleBefore));  // as for the other tests
    QApplication::setPalette(QPalette());
#endif
    (void)styleBefore;
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
        sync::SyncHistory h(file);
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
    sync::SyncHistory again(file);  // kept in the file
    REQUIRE(again.runs().size() == 4);
    for (int i = 0; i < 40; ++i)
        again.add("server", report({change("N" + std::to_string(i), "x", "added", true)}), true, "pulled 1");
    CHECK(again.runs().size() == sync::SyncHistory::kMaxRuns);

    SyncHistoryWindow window;
    window.reload(again);
    auto* tree = window.findChild<QTreeWidget*>("syncHistory");
    REQUIRE(tree);
    CHECK(tree->topLevelItemCount() == sync::SyncHistory::kMaxRuns);
    auto* first = tree->topLevelItem(0);
    CHECK(first->text(0).contains("1 received"));
    REQUIRE(first->childCount() == 1);
    CHECK(first->child(0)->text(0).contains("Note"));
    CHECK(first->child(0)->text(1) == "Added");
    CHECK(tree->isColumnHidden(2));  // no other program's changes: no By column

    // ournotes-cli synced what an agent changed
    auto agent = change("BOOK0002", "Renamed", "changed", false);
    agent.by = "ournotes-cli";
    sync::SyncHistory(file, "ournotes-cli").add("server", report({agent}), true, "pushed 1");
    CHECK(again.reload());
    window.reload(again);
    CHECK(!tree->isColumnHidden(2));
    CHECK(tree->topLevelItem(0)->text(0).contains("synced by ournotes-cli"));
    CHECK(tree->topLevelItem(0)->child(0)->text(2) == "ournotes-cli");
    CHECK(tree->topLevelItem(1)->child(0)->text(2).isEmpty());
}

TEST_CASE("storage window: this computer's disk and libraries, and the sync target's")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    env.library.createNote(env.lib, "<p>A note</p>", jstr(paper, "key"));
    env.library.importAttachment(env.lib, file, jstr(paper, "key"));
    StorageWindow window;
    window.show();
    // no target yet
    window.refresh();
    REQUIRE(QTest::qWaitFor([&] { return !window.isLoading(); }, 10000));
    auto* local = window.findChild<QTreeWidget*>("localLibraries");
    REQUIRE(local->topLevelItemCount() == 1);
    CHECK(local->topLevelItem(0)->text(1) == "1");  // items
    CHECK(local->topLevelItem(0)->text(2) == "1");  // notes
    CHECK(local->topLevelItem(0)->text(4) == "1");  // files
    CHECK(local->topLevelItem(0)->text(5).isEmpty());  // none missing
    CHECK(window.findChild<QLabel*>("localFiles")->text().startsWith("1 file,"));
    CHECK(window.findChild<QLabel*>("otherFiles")->text().startsWith("0 files"));
    CHECK(window.findChild<QLabel*>("localDisk")->text().contains("free of"));
    CHECK(window.findChild<QLabel*>("targetState")->text().contains("No sync target"));

    // a folder, synced to
    SyncTarget target;
    target.id = "folder1";
    target.name = "USB stick";
    target.kind = "local";
    target.path = pathString(tmp.path / "folder");
    env.config.upsertTarget(target);
    env.config.setActiveTarget("folder1");
    {
        Library lib(env.library.dataDir());
        sync::LocalTransport transport(tmp.path / "folder");
        REQUIRE(sync::SyncEngine(lib, transport, "folder1").syncAll().errors.empty());
    }
    window.refresh();
    REQUIRE(QTest::qWaitFor([&] { return !window.isLoading(); }, 10000));
    auto* there = window.findChild<QTreeWidget*>("targetLibraries");
    REQUIRE(there->isVisible());
    REQUIRE(there->topLevelItemCount() == 1);
    CHECK(there->topLevelItem(0)->text(1) == "owner");
    CHECK(there->topLevelItem(0)->text(3) == "1");  // items
    CHECK(there->topLevelItem(0)->text(6) == "1");  // files
    CHECK(window.findChild<QLabel*>("server")->text().startsWith("a folder"));
    CHECK(window.findChild<QLabel*>("targetDisk")->text().contains("free of"));
    // an attachment whose file is not here: counted apart, named in its tooltip
    env.library.createItem(env.lib, Json{{"itemType", "attachment"},
                                         {"linkMode", "imported_file"},
                                         {"parentItem", jstr(paper, "key")},
                                         {"filename", "Never downloaded.pdf"}});
    window.refresh();
    REQUIRE(QTest::qWaitFor([&] { return !window.isLoading(); }, 10000));
    CHECK(local->topLevelItem(0)->text(4) == "1");
    CHECK(local->topLevelItem(0)->text(5) == "1");
    CHECK(local->topLevelItem(0)->toolTip(5).contains("Never downloaded.pdf"));
    CHECK(window.findChild<QLabel*>("localFiles")->text().contains("1 missing"));
    // the disks' bars filled as far as they are used (once eased in)
    QTest::qWait(1500);
    for (auto* bar : window.findChildren<FancyProgressBar*>())
        CHECK(bar->fraction() > 0);
    if (auto dir = qEnvironmentVariable("OURNOTES_TEST_SNAPSHOT_DIR"); !dir.isEmpty())
        window.grab().save(dir + "/storage-window.png");
}

TEST_CASE("storage window: the server line has the window's width and shows all of its text, also on macOS")
{
    app();
    Env env;
    TempDir tmp;
    SyncTarget target;
    target.id = "folder1";
    target.name = "USB stick";
    target.kind = "local";
    target.path = pathString(tmp.path / "a folder with a rather long name, so that the line is long");
    env.config.upsertTarget(target);
    env.config.setActiveTarget("folder1");
    {
        Library lib(env.library.dataDir());
        sync::LocalTransport transport(target.path);
        REQUIRE(sync::SyncEngine(lib, transport, "folder1").syncAll().errors.empty());
    }
    // macOS's style keeps a form's fields at their size hint: a wrapped label stayed
    // narrow, and lost what did not fit ("ournotesd 0.1.14," and no more)
    struct MacForms : QProxyStyle {
        int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                      QStyleHintReturn* data) const override
        {
            return hint == SH_FormLayoutFieldGrowthPolicy ? QFormLayout::FieldsStayAtSizeHint
                                                          : QProxyStyle::styleHint(hint, option, widget, data);
        }
    } style;
    StorageWindow window;
    for (auto* form : window.findChildren<QFormLayout*>())
        if (form->parentWidget())
            form->parentWidget()->setStyle(&style);
    window.resize(760, 680);
    window.show();
    window.refresh();
    REQUIRE(QTest::qWaitFor([&] { return !window.isLoading(); }, 10000));
    QApplication::processEvents();
    auto* server = window.findChild<QLabel*>("server");
    REQUIRE(server->text().startsWith("a folder"));
    CHECK(server->width() > window.width() / 2);
    CHECK(server->height() >= server->heightForWidth(server->width()));
    for (auto* form : window.findChildren<QFormLayout*>())
        if (form->parentWidget())
            form->parentWidget()->setStyle(nullptr);
}

TEST_CASE("main window: what another program changes shows at once; its own sync is not taken for that")
{
    app();
    Env env;
    TempDir tmp;
    SyncTarget target;
    target.id = "folder1";
    target.name = "Shared folder";
    target.kind = "local";
    target.path = pathString(tmp.path / "folder");
    env.config.upsertTarget(target);
    env.config.setActiveTarget("folder1");
    env.config.set("/sync/auto", false);
    auto paper = jstr(env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Before"}}), "key");
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
    int elsewhere = 0;
    QObject::connect(&env.context, &AppContext::changedElsewhere, [&] { ++elsewhere; });
    auto* model = window.findChild<ItemsModel*>();
    REQUIRE(model);
    auto shownTitle = [&] {
        for (int r = 0; r < model->rowCount(); ++r)
            if (model->index(r, 0).data().toString() == "After")
                return true;
        return false;
    };
    // ournotes-cli, an agent: another connection to the same database
    {
        Library other(env.library.dataDir());
        other.updateItem(env.lib, paper, [](Json& d) { d["title"] = "After"; });
    }
    CHECK(QTest::qWaitFor([&] { return elsewhere == 1; }, 5000));
    CHECK(QTest::qWaitFor(shownTitle, 3000));
    // the app's sync writes on a connection of its own: not another program's
    auto* sync = window.findChild<SyncController*>();
    REQUIRE(sync);
    bool done = false;
    QObject::connect(sync, &SyncController::finished, [&] { done = true; });
    sync->syncNow();
    REQUIRE(QTest::qWaitFor([&] { return done; }, 20000));
    QTest::qWait(2500);
    CHECK(elsewhere == 1);
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

    // ournotes-cli changes it and syncs while the app runs: its sync shows here soon
    bool changed = false;
    QObject::connect(&sync, &SyncController::historyChanged, [&] { changed = true; });
    {
        Library cli(env.library.dataDir());
        cli.setOrigin("ournotes-cli");
        cli.updateItem(env.lib, jstr(paper, "key"), [](Json& d) { d["title"] = "Renamed by an agent"; });
        auto transport = makeTransport(target);
        sync::SyncEngine engine(cli, *transport, target.id);
        sync::SyncHistory(cli.dataDir() / "sync-history.json", "ournotes-cli")
            .add(target.name, engine.syncAll(), true, "pushed 1");
    }
    REQUIRE(QTest::qWaitFor([&] { return changed; }, 5000));
    REQUIRE(runs.size() == 1);  // the book was the first sync's only change, now under this one
    CHECK(runs[0]["by"] == "ournotes-cli");
    REQUIRE(runs[0]["changes"].size() == 1);
    CHECK(runs[0]["changes"][0]["by"] == "ournotes-cli");
    CHECK(runs[0]["changes"][0]["title"] == "Renamed by an agent");
}

TEST_CASE("sync: what another computer changed shows in the open PDF, beside it and in the item pane")
{
    app();
    Env env;
    TempDir tmp;  // the folder both computers sync through, and the other computer's library
    SyncTarget target;
    target.id = "folder1";
    target.name = "Shared folder";
    target.kind = "local";
    target.path = pathString(tmp.path / "folder");
    env.config.upsertTarget(target);
    env.config.setActiveTarget("folder1");
    env.config.set("/sync/auto", false);
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paperKey = jstr(env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}}), "key");
    auto noteKey = jstr(env.library.createNote(env.lib, "<p>First words</p>", paperKey), "key");
    auto attKey = jstr(env.library.importAttachment(env.lib, file, paperKey), "key");
    auto annKey = jstr(env.library.createItem(
                           env.lib, Json{{"itemType", "annotation"},
                                         {"parentItem", attKey},
                                         {"annotationType", "highlight"},
                                         {"annotationColor", "#ffd400"},
                                         {"annotationText", "Hello"},
                                         {"annotationComment", ""},
                                         {"annotationPosition", Json{{"pageIndex", 0}, {"rects", Json::array({Json::array({72, 690, 160, 710})})}}}}),
                       "key");
    SyncController sync;
    bool done = false;
    QObject::connect(&sync, &SyncController::finished, [&](bool ok, const QString& summary) {
        INFO(summary.toStdString());
        CHECK(ok);
        done = true;
    });
    auto syncHere = [&] {
        done = false;
        sync.syncNow();
        REQUIRE(QTest::qWaitFor([&] { return done; }, 20000));
        QTest::qWait(100);  // the reloads after it
    };
    Library other(tmp.path / "other");
    auto syncThere = [&] {
        sync::LocalTransport transport(tmp.path / "folder");
        REQUIRE(sync::SyncEngine(other, transport, "folder1").syncAll().errors.empty());
    };
    syncHere();
    syncThere();
    auto there = other.libraryByUid(env.library.library(env.lib)->uid)->id;

    // here, the PDF open: the top note beside it, the paper's info in the other tab
    ReaderTab tab(env.lib, qs(attKey));
    tab.resize(1200, 900);
    tab.show();
    QApplication::processEvents();
    auto* view = tab.view();
    auto* editor = tab.findChild<NoteEditor*>();
    REQUIRE(editor);
    REQUIRE(editor->key() == qs(noteKey));
    auto* noteText = editor->findChild<NoteTextEdit*>();
    auto* info = tab.findChild<InfoEditor*>();
    REQUIRE(info);
    auto title = [&]() -> AutoTextEdit* {
        for (auto* e : info->findChildren<AutoTextEdit*>())
            if (e->toPlainText().startsWith("Paper"))
                return e;
        return nullptr;
    };
    REQUIRE(title());
    CHECK(view->commentBadgeRect(qs(annKey)).isEmpty());
    int notes = tab.findChild<QListWidget*>()->count();

    // there: a comment, the note edited, a new note, the title changed
    other.updateItem(there, annKey, [](Json& d) { d["annotationComment"] = "Why?"; });
    other.updateItem(there, noteKey, [](Json& d) { d["note"] = "<p>First words, and more</p>"; });
    other.createNote(there, "<p>A second note</p>", paperKey);
    other.updateItem(there, paperKey, [](Json& d) { d["title"] = "Paper, retitled"; });
    syncThere();
    syncHere();
    CHECK(!view->commentBadgeRect(qs(annKey)).isEmpty());
    auto cards = tab.findChildren<AnnotationCard*>();
    REQUIRE(cards.size() == 1);
    CHECK(jstr(cards[0]->data(), "annotationComment") == "Why?");
    CHECK(noteText->toPlainText().toStdString() == "First words, and more");
    CHECK(tab.findChild<QListWidget*>()->count() == notes + 1);
    REQUIRE(title());
    CHECK(title()->toPlainText().toStdString() == "Paper, retitled");

    // a field being typed in here is left alone (it is saved when left)
    title()->setFocus();
    title()->moveCursor(QTextCursor::End);
    QTest::keyClicks(title(), " typed here");
    other.updateItem(there, paperKey, [](Json& d) { d["date"] = "2024"; });
    syncThere();
    syncHere();
    REQUIRE(title());
    CHECK(title()->hasFocus());
    CHECK(title()->toPlainText().toStdString() == "Paper, retitled typed here");
    // left: what was typed is kept, and what came meanwhile shown
    view->setFocus();
    QTest::qWait(100);
    CHECK(jstr(*env.library.item(env.lib, paperKey), "title") == "Paper, retitled typed here");
    bool dateShown = false;
    for (auto* e : info->findChildren<QLineEdit*>())
        dateShown |= e->text() == "2024";
    CHECK(dateShown);
}

TEST_CASE("sync: another computer's push is pulled by itself, at once")
{
    app();
    Env env;
    TempDir tmp;
    SyncTarget target;
    target.id = "folder1";
    target.name = "Shared folder";
    target.kind = "local";
    target.path = pathString(tmp.path / "folder");
    env.config.upsertTarget(target);
    env.config.setActiveTarget("folder1");
    env.config.set("/sync/auto", true);
    auto paperKey = jstr(env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Here"}}), "key");
    SyncController sync;
    CHECK(sync.isWatching());
    int syncs = 0;
    QObject::connect(&sync, &SyncController::finished, [&](bool ok, const QString& summary) {
        INFO(summary.toStdString());
        CHECK(ok);
        ++syncs;
    });
    sync.syncNow();
    REQUIRE(QTest::qWaitFor([&] { return syncs == 1; }, 20000));

    // the other computer: gets it, writes, sends it
    Library other(tmp.path / "other");
    auto syncThere = [&] {
        sync::LocalTransport transport(tmp.path / "folder");
        REQUIRE(sync::SyncEngine(other, transport, "folder1").syncAll().errors.empty());
    };
    syncThere();
    auto there = other.libraryByUid(env.library.library(env.lib)->uid)->id;
    other.updateItem(there, paperKey, [](Json& d) { d["title"] = "Changed there"; });
    syncThere();

    // here, nobody syncing: it comes in a few seconds (a folder is looked at every 3)
    REQUIRE(QTest::qWaitFor([&] { return jstr(*env.library.item(env.lib, paperKey), "title") == "Changed there"; },
                            sync::LocalTransport::kPollSeconds * 1000 + 5000));
    CHECK(syncs == 2);
    // and its own sync (nothing new) is not taken for another computer's
    QTest::qWait(sync::LocalTransport::kPollSeconds * 1000 + 500);
    CHECK(syncs == 2);

    // off with automatic syncs
    env.config.set("/sync/auto", false);
    sync.reloadSettings();
    CHECK(!sync.isWatching());
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

TEST_CASE("library: items dragged onto another library or one of its collections are copied there")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto group = env.library.createLibrary("Group");
    auto reading = jstr(env.library.createCollection(group, "Reading"), "key");
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    env.library.importAttachment(env.lib, file, jstr(paper, "key"));
    auto lone = env.library.importAttachment(env.lib, file, "", false, "A PDF on its own");
    LibraryView view;
    view.resize(1200, 700);
    view.show();
    auto* tree = view.findChild<CollectionTree*>();
    tree->selectScope({env.lib, ScopeKind::Library, {}});
    tree->expandAll();
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
    auto node = [&](const QString& name) -> QTreeWidgetItem* {
        auto found = tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
        return found.size() == 1 ? found[0] : nullptr;
    };
    auto drop = [&](const QModelIndex& row, QTreeWidgetItem* onto) {
        REQUIRE(row.isValid());
        REQUIRE(onto);
        std::unique_ptr<QMimeData> mime(model->mimeData({row}));
        QPoint at = tree->visualItemRect(onto).center();
        QDragEnterEvent enter(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDragMoveEvent move(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        CHECK(move.isAccepted());
        QDropEvent dropped(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &dropped);
        QApplication::processEvents();
    };
    auto inGroup = [&](const std::string& title) {
        std::vector<Json> found;
        for (auto& d : env.library.allItems(group))
            if (jstr(d, "title") == title)
                found.push_back(d);
        return found;
    };
    // the paper, with its PDF, onto the other library
    drop(rowOf("Paper"), node("Group"));
    REQUIRE(inGroup("Paper").size() == 1);
    auto copy = inGroup("Paper")[0];
    CHECK(jstr(copy, "key") != jstr(paper, "key"));
    auto pdfs = env.library.children(group, jstr(copy, "key"));
    REQUIRE(pdfs.size() == 1);
    CHECK(env.library.attachmentFileExists(group, pdfs[0]));
    // a PDF on its own, onto a collection of it
    drop(rowOf("A PDF on its own"), node("Reading"));
    REQUIRE(inGroup("A PDF on its own").size() == 1);
    CHECK(inGroup("A PDF on its own")[0]["collections"] == Json::array({reading}));
    CHECK(env.library.attachmentFileExists(group, inGroup("A PDF on its own")[0]));
    // where they came from, they stay
    CHECK(env.library.item(env.lib, jstr(paper, "key")));
    CHECK(env.library.item(env.lib, jstr(lone, "key")));
    CHECK(env.library.children(env.lib, jstr(paper, "key")).size() == 1);
}

TEST_CASE("collections: a drag within four rows of the top or bottom scrolls, faster nearer the edge")
{
    app();
    Env env;
    for (int i = 0; i < 80; ++i)
        env.library.createCollection(env.lib, "Collection " + std::to_string(100 + i));
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Paper"}});
    LibraryView view;
    view.resize(1000, 700);
    view.show();
    QApplication::processEvents();
    auto* tree = view.findChild<CollectionTree*>();
    auto* bar = tree->verticalScrollBar();
    REQUIRE(bar->maximum() > 0);
    auto* model = view.findChild<ItemsModel*>();
    REQUIRE(model);
    std::unique_ptr<QMimeData> mime(model->mimeData({model->index(0, 0)}));
    int row = tree->visualItemRect(tree->topLevelItem(0)).height();
    int h = tree->viewport()->height();
    REQUIRE(h > 12 * row);  // the zones are four rows each
    int x = tree->viewport()->width() / 2;
    auto hold = [&](int y, int ms) {  // the scrolling done with the drag held there
        bar->setValue(bar->maximum() / 2);
        QPoint at(x, y);
        QDragEnterEvent enter(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &enter);
        QDragMoveEvent move(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &move);
        int from = bar->value();
        QTest::qWait(ms);
        int scrolled = bar->value() - from;
        QDragLeaveEvent leave;
        QApplication::sendEvent(tree->viewport(), &leave);
        return scrolled;
    };
    int slow = hold(h - row * 7 / 2, 400);  // three and a half rows from the bottom
    int fast = hold(h - row / 3, 400);       // at the very bottom
    CAPTURE(slow);
    CAPTURE(fast);
    CHECK(slow > 0);
    CHECK(fast > 4 * slow);
    CHECK(hold(h - row * 9 / 2, 400) == 0);  // four and a half rows away: not
    CHECK(hold(row * 7 / 2, 400) < 0);      // near the top: up
    // dropped: no more scrolling
    bar->setValue(bar->maximum() / 2);
    QPoint at(x, h - row / 3);
    QDragEnterEvent enter(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &enter);
    QDragMoveEvent move(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &move);
    QTest::qWait(100);
    QDropEvent dropped(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &dropped);
    int after = bar->value();
    QTest::qWait(300);
    CHECK(bar->value() == after);
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

TEST_CASE("item pane: an item's fields are in place when first drawn")
{
    app();
    Env env;
    auto item = env.library.createItem(
        env.lib, Json{{"itemType", "journalArticle"},
                      {"title", "A rather long title that wraps over several lines when the box is narrow, as new boxes are"},
                      {"creators", Json::array({Json{{"creatorType", "author"}, {"firstName", "Ada"}, {"lastName", "Lovelace"}},
                                                Json{{"creatorType", "author"}, {"firstName", "Alan"}, {"lastName", "Turing"}}})},
                      {"abstractNote", std::string(600, 'x')}});
    auto other = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Short"}});
    ItemPane pane;
    pane.resize(500, 900);
    pane.show();
    pane.showItems(env.lib, {qs(jstr(other, "key"))});
    QApplication::processEvents();
    auto where = [&] {  // the title box's height and the first creator's place
        std::pair<int, int> out{-1, -1};
        for (auto* e : pane.findChildren<AutoTextEdit*>())
            if (e->isVisible() && e->toPlainText().startsWith("A rather long"))
                out.first = e->height();
        for (auto* c : pane.findChildren<QComboBox*>())
            if (c->isVisible() && c->currentText() == "Author:")
                out.second = out.second < 0 ? c->mapTo(&pane, QPoint(0, 0)).y() : std::min(out.second, c->mapTo(&pane, QPoint(0, 0)).y());
        return out;
    };
    pane.showItems(env.lib, {qs(jstr(item, "key"))});
    auto first = where();  // as the next frame would draw it
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
    QTest::qWait(50);
    auto settled = where();
    REQUIRE(settled.first > 0);
    CHECK(first == settled);
    // and nothing of the item before is left to be drawn over it
    pane.showItems(env.lib, {qs(jstr(other, "key"))});
    int stale = 0;
    for (auto* e : pane.findChildren<AutoTextEdit*>())
        stale += e->isVisible() && e->toPlainText().startsWith("A rather long");
    CHECK(stale == 0);
}

TEST_CASE("library: moving between items does not change the window's minimum size")
{
    app();
    Env env;
    std::vector<std::string> colls;
    for (int i = 0; i < 8; ++i)
        colls.push_back(jstr(env.library.createCollection(env.lib, "Robotics subject " + std::to_string(i)), "key"));
    auto many = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "In many collections"}});
    auto one = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "In one collection"}});
    for (auto& c : colls)  // a location line each, in the item pane
        env.library.addToCollection(env.lib, c, {jstr(many, "key")});
    env.library.addToCollection(env.lib, colls[0], {jstr(one, "key")});
    // and no note, one, or two (the notes tab beside Info shows the list for two)
    auto noted = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "With a note"}});
    env.library.addToCollection(env.lib, colls[0], {jstr(noted, "key")});
    env.library.createNote(env.lib, "<p>one</p>", jstr(noted, "key"));
    env.library.createNote(env.lib, "<p>first</p>", jstr(many, "key"));
    env.library.createNote(env.lib, "<p>second</p>", jstr(many, "key"));
    MainWindow window;
    window.resize(1200, 600);
    window.show();
    QApplication::processEvents();
    auto* view = window.findChild<LibraryView*>();
    view->findChild<CollectionTree*>()->selectScope({env.lib, ScopeKind::Collection, colls[0]});
    QApplication::processEvents();
    QTreeView* items = nullptr;
    for (auto* t : view->findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    items->setFocus();
    // in the Info tab and in the Notes tab
    std::set<int> heights, widths;
    for (int tab : {0, 1}) {
        view->findChild<ItemPane*>()->findChild<QTabWidget*>()->setCurrentIndex(tab);
        items->setCurrentIndex(items->model()->index(0, 0));
        for (auto k : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Up, Qt::Key_Up, Qt::Key_Down, Qt::Key_Down}) {
            QTest::keyClick(items, k);
            QApplication::processEvents();
            heights.insert(window.minimumSizeHint().height());
            widths.insert(window.minimumSizeHint().width());
        }
    }
    CHECK(heights.size() == 1);
    // (each change of the width made the window a pixel narrower at a 150% scale)
    CHECK(widths.size() == 1);
    CHECK(view->findChild<LocationBox*>());
}

TEST_CASE("reader: [ and ] again and again never change the window's minimum size")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    env.library.createNote(env.lib, "<p>A note</p>", jstr(paper, "key"));
    env.library.createNote(env.lib, "<p>Another note</p>", jstr(paper, "key"));
    auto att = env.library.importAttachment(env.lib, file, jstr(paper, "key"));
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    window.openAttachment(env.lib, qs(jstr(att, "key")));
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    auto* reader = qobject_cast<ReaderTab*>(tabs->currentWidget());
    REQUIRE(reader);
    auto* view = reader->view();
    view->setFocus();
    QApplication::processEvents();
    QSize size = window.size();
    // (each change of the minimum moved the window's edge at a 150% scale: hiding Info
    // and Notes let the window be less tall)
    std::set<int> widths{window.minimumWidth()}, heights{window.minimumHeight()};
    for (auto key : {Qt::Key_I, Qt::Key_N}) {  // in the Info tab and in the Notes tab
        QTest::keyClick(view, key);
        for (int i = 0; i < 8; ++i) {
            QTest::keyClick(view, i % 4 < 2 ? Qt::Key_BracketLeft : Qt::Key_BracketRight);
            QApplication::processEvents();
            widths.insert(window.minimumWidth());
            heights.insert(window.minimumHeight());
        }
    }
    CHECK(widths.size() == 1);
    CHECK(heights.size() == 1);
    CHECK(window.size() == size);

    // the library's item pane, hidden and shown with its button
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    auto* paneButton = window.findChild<QToolButton*>("itemPaneToggle");
    REQUIRE(paneButton);
    widths = {window.minimumWidth()};
    heights = {window.minimumHeight()};
    for (int i = 0; i < 8; ++i) {
        paneButton->click();
        QApplication::processEvents();
        widths.insert(window.minimumWidth());
        heights.insert(window.minimumHeight());
    }
    CHECK(widths.size() == 1);
    CHECK(heights.size() == 1);
    CHECK(window.size() == size);
}

TEST_CASE("library: the collections come back as wide as they were, after a small start")
{
    app();
    Env env;
    auto splitOf = [](MainWindow& w) {
        return qobject_cast<QSplitter*>(w.findChild<LibraryView*>()->findChild<ItemPane*>()->parentWidget());
    };
    QByteArray state;
    {
        MainWindow window;
        window.resize(1600, 900);
        window.show();
        QApplication::processEvents();
        splitOf(window)->setSizes({300, 880, 420});
        QApplication::processEvents();
        state = window.findChild<LibraryView*>()->saveState();
    }
    env.config.set("/ui/libraryState", state.toBase64().toStdString());
    env.config.set("/ui/sidePaneWidth", 420);
    // shown small first, as before the window manager gives the window its size
    MainWindow window;
    window.resize(600, 500);
    window.show();
    QApplication::processEvents();
    window.resize(1600, 900);
    QApplication::processEvents();
    auto sizes = splitOf(window)->sizes();
    CHECK(sizes[0] == 300);
    CHECK(sizes[2] == 420);
}

TEST_CASE("main window: the pane beside the PDFs and the library is one width")
{
    app();
    Env env;
    TempDir tmp;
    std::vector<QString> keys;
    for (int i = 0; i < 3; ++i) {
        auto file = tmp.path / ("paper" + std::to_string(i) + ".pdf");
        writeSamplePdf(file);
        auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper " + std::to_string(i)}});
        keys.push_back(qs(jstr(env.library.importAttachment(env.lib, file, jstr(paper, "key")), "key")));
    }
    MainWindow window;
    window.resize(1500, 900);
    window.show();
    window.openAttachment(env.lib, keys[0]);
    window.openAttachment(env.lib, keys[1]);
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    auto paneWidth = [&](QWidget* tab) {
        QSplitter* split = nullptr;  // the tab's own, whose last widget is the pane
        for (auto* s : tab->findChildren<QSplitter*>())
            if (s->orientation() == Qt::Horizontal && s->count() == 3 && (!split || s->parentWidget() == tab))
                split = s;
        return split ? split->sizes().back() : -1;
    };
    auto* first = tabs->widget(1);
    tabs->setCurrentWidget(first);
    QApplication::processEvents();
    QSplitter* split = nullptr;
    for (auto* s : first->findChildren<QSplitter*>())
        if (s->orientation() == Qt::Horizontal && s->count() == 3)
            split = s;
    REQUIRE(split);
    int before = split->sizes().back();
    auto* handle = split->handle(2);
    QPoint mid(handle->width() / 2, handle->height() / 2);
    QTest::mousePress(handle, Qt::LeftButton, {}, mid);
    QTest::mouseMove(handle, mid - QPoint(40, 0));
    QTest::mouseMove(handle, mid - QPoint(80, 0));
    QTest::mouseRelease(handle, Qt::LeftButton, {}, mid - QPoint(80, 0));
    QApplication::processEvents();
    int width = split->sizes().back();
    REQUIRE(width > before);
    CHECK(env.config.get("/ui/sidePaneWidth", 0).get<int>() == width);
    tabs->setCurrentIndex(2);  // the other PDF
    QApplication::processEvents();
    CHECK(paneWidth(tabs->widget(2)) == width);
    tabs->setCurrentIndex(0);  // the library's item pane
    QApplication::processEvents();
    CHECK(paneWidth(window.findChild<LibraryView*>()) == width);
    window.openAttachment(env.lib, keys[2]);  // opened later
    QApplication::processEvents();
    CHECK(paneWidth(tabs->currentWidget()) == width);
}

TEST_CASE("notes: the list over the note is as high everywhere, and stays so")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    env.library.createNote(env.lib, "<p>One</p>", jstr(paper, "key"));
    env.library.createNote(env.lib, "<p>Two</p>", jstr(paper, "key"));  // two: the list shows
    auto key = qs(jstr(env.library.importAttachment(env.lib, file, jstr(paper, "key")), "key"));
    env.config.set("/ui/itemPaneTab", "notes");  // in view beside the PDFs
    auto splitOf = [](QWidget& w) {
        for (auto* s : w.findChildren<QSplitter*>())
            if (s->orientation() == Qt::Vertical && s->findChild<NoteEditor*>() && s->findChild<QListWidget*>())
                return s;
        return static_cast<QSplitter*>(nullptr);
    };
    ReaderTab a(env.lib, key), b(env.lib, key);
    ItemNotesPanel library;
    for (QWidget* w : {static_cast<QWidget*>(&a), static_cast<QWidget*>(&b), static_cast<QWidget*>(&library)}) {
        w->resize(1100, 800);
        w->show();
    }
    library.setItem(env.lib, qs(jstr(paper, "key")));
    QApplication::processEvents();
    auto *sa = splitOf(a), *sb = splitOf(b), *sl = splitOf(library);
    REQUIRE(sa);
    REQUIRE(sb);
    REQUIRE(sl);
    int before = sa->sizes()[0];
    // drag a's handle down
    auto* handle = sa->handle(1);
    QPoint mid(handle->width() / 2, handle->height() / 2);
    QTest::mousePress(handle, Qt::LeftButton, {}, mid);
    QTest::mouseMove(handle, mid + QPoint(0, 40));
    QTest::mouseMove(handle, mid + QPoint(0, 80));
    QTest::mouseRelease(handle, Qt::LeftButton, {}, mid + QPoint(0, 80));
    QApplication::processEvents();
    int after = sa->sizes()[0];
    REQUIRE(after > before);
    CHECK(sb->sizes()[0] == after);  // the other PDF's
    CHECK(sl->sizes()[0] == after);  // and the library's
    CHECK(env.config.get("/ui/notesListHeight", 0).get<int>() == after);
    ReaderTab c(env.lib, key);  // opened later: the same
    c.resize(1100, 800);
    c.show();
    QApplication::processEvents();
    CHECK(splitOf(c)->sizes()[0] == after);
}

TEST_CASE("main window: maximized, its minimum size stays as it is; restored, it follows the contents")
{
    app();
    Env env;
    MainWindow window;
    window.resize(1000, 650);
    window.show();
    QApplication::processEvents();
    int minimum = window.minimumHeight();
    REQUIRE(minimum > 0);
    // a normal window: at once
    window.centralWidget()->setMinimumHeight(minimum + 20);
    QApplication::processEvents();
    CHECK(window.minimumHeight() >= minimum + 20);
    minimum = window.minimumHeight();
    // maximized: not (at 150% KWin shrank the window by a pixel at every change)
    window.showMaximized();
    REQUIRE(QTest::qWaitFor([&] { return window.isMaximized(); }, 3000));
    QApplication::processEvents();
    window.centralWidget()->setMinimumHeight(minimum + 60);
    QApplication::processEvents();
    CHECK(window.minimumHeight() == minimum);
    // restored: the contents' again
    window.showNormal();
    REQUIRE(QTest::qWaitFor([&] { return !window.isMaximized(); }, 3000));
    QApplication::processEvents();
    CHECK(window.minimumHeight() >= minimum + 60);
}

TEST_CASE("main window: a PDF's tab is named after its item as it is now")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Old title"}});
    auto att = env.library.importAttachment(env.lib, file, jstr(paper, "key"));
    auto lone = env.library.importAttachment(env.lib, file, "", false, "A PDF on its own");
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    window.openAttachment(env.lib, qs(jstr(att, "key")));
    window.openAttachment(env.lib, qs(jstr(lone, "key")));
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    REQUIRE(tabs->count() == 3);
    CHECK(tabs->tabText(1) == "Old title");
    tabs->setCurrentIndex(0);
    // renamed in the library: the item's title, and the PDF on its own
    env.library.updateItem(env.lib, jstr(paper, "key"), [](Json& d) { d["title"] = "New title"; });
    env.library.updateItem(env.lib, jstr(lone, "key"), [](Json& d) {
        d["title"] = "A rather long title for a PDF that has no item of its own";
    });
    CHECK(QTest::qWaitFor([&] { return tabs->tabText(1) == "New title"; }, 2000));
    CHECK(tabs->tabToolTip(1) == "New title");
    CHECK(tabs->tabText(2) == "A rather long title for a PDF that has…");  // shortened, whole in its tooltip
    CHECK(tabs->tabToolTip(2) == "A rather long title for a PDF that has no item of its own");
    QApplication::processEvents();
    bool shown = false;
    for (auto* button : window.findChildren<QWidget*>("tabButton"))
        shown |= button->toolTip() == "New title";
    CHECK(shown);  // in the strip of tabs too
}

TEST_CASE("main window: Ctrl+Shift+Tab goes to the previous tab, also from a note")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    env.library.createNote(env.lib, "<p>A note</p>", jstr(paper, "key"));
    auto att = env.library.importAttachment(env.lib, file, jstr(paper, "key"));
    MainWindow window;
    window.resize(1400, 800);
    window.show();
    window.openAttachment(env.lib, qs(jstr(att, "key")));
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>("mainTabs");
    auto* reader = qobject_cast<ReaderTab*>(tabs->currentWidget());
    REQUIRE(reader);
    auto* side = reader->findChild<QTabWidget*>("readerContextTabs");
    REQUIRE(side);
    // as Backtab or as Tab with Shift, whichever the keyboard sends
    for (auto key : {Qt::Key_Backtab, Qt::Key_Tab}) {
        tabs->setCurrentWidget(reader);
        side->setCurrentIndex(1);
        auto* note = side->widget(1)->findChild<NoteTextEdit*>();
        REQUIRE(note);
        note->setFocus();
        QApplication::processEvents();
        QTest::keyClick(note, key, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::processEvents();
        CHECK(tabs->currentIndex() == 0);  // the library, the tab before
        CHECK(side->currentIndex() == 1);  // still Notes
    }
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

TEST_CASE("main window: the tabs make room for the title bar macOS slides over a full-screen window")
{
    app();
    Env env;
    MainWindow window;
    window.resize(900, 600);
    window.show();
    QApplication::processEvents();
    auto* strip = window.findChild<QWidget*>("tabStrip");
    REQUIRE(strip);
    int top = strip->mapTo(&window, QPoint(0, 0)).y();
    int height = window.centralWidget()->height();
    // with the pointer at the top of the screen, macOS shows the menu and title bar over the
    // window; the window moves its content below them, and back when they go
    window.setTitleBarOverlap(27);
    QApplication::processEvents();
    CHECK(strip->mapTo(&window, QPoint(0, 0)).y() == top + 27);
    CHECK(window.size() == QSize(900, 600));
    CHECK(window.centralWidget()->height() == height);  // the rest of the window gets shorter
    window.setTitleBarOverlap(0);
    QApplication::processEvents();
    CHECK(strip->mapTo(&window, QPoint(0, 0)).y() == top);
}

TEST_CASE("main window: no menu item is taken for Quit, Preferences or About because of its text")
{
    app();
    Env env;
    MainWindow window;
    window.resize(900, 600);
    window.show();
    window.setWindowState(Qt::WindowFullScreen);
    QApplication::processEvents();
    auto menus = window.menuBar()->findChildren<QMenu*>();
    for (auto* menu : menus)
        emit menu->aboutToShow();  // the View menu then calls its item "Exit Full Screen"
    // Qt on macOS moves an item into the application menu by the start of its text unless
    // its role says otherwise: "Exit Full Screen" became Quit, with Command-Control-F
    static const QRegularExpression guessed("^(quit|exit|about|config|preference|options|setting|setup)|about$",
                                            QRegularExpression::CaseInsensitiveOption);
    QAction* quit = nullptr;
    for (auto* menu : menus)
        for (auto* action : menu->actions()) {
            QString text = action->text().remove('&');
            if (action->menuRole() == QAction::TextHeuristicRole) {
                INFO(text.toStdString());
                CHECK(!text.contains(guessed));
            }
            if (action->menuRole() == QAction::QuitRole)
                quit = action;
        }
    REQUIRE(quit);
    CHECK(quit->shortcut() == QKeySequence(QKeySequence::Quit));
    window.setWindowState(Qt::WindowNoState);
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
        env.config.set("/ui/bandBackground", true);
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

// Run at 150% too (ctest: test_gui_scaled), where it failed: a row's edge is inside a pixel
// the row above shares, and repainting only the rows whose selection changed left a line
// of the old highlight behind (and cut one off the new).
TEST_CASE("library: a new selection leaves no line of the old one")
{
    app();
    Env env;
    for (int i = 0; i < 15; ++i)
        env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper " + std::to_string(i)}});
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    QTest::qWait(100);
    std::vector<QTreeView*> views;  // the items, then the collections (a click there changes the items)
    for (auto* t : window.findChild<LibraryView*>()->findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            views.push_back(t);
    views.push_back(window.findChild<CollectionTree*>());
    REQUIRE(views.size() == 2);
#ifndef OURNOTES_TEST_BACKING_STORE
    MESSAGE("Qt's private headers are not there: the pixels are not compared");
#endif
    // the window's pixels as they are on the screen (painted bit by bit), not drawn anew; a
    // screen grab gives only part of them offscreen when scaled
    auto shown = [&] {
#ifdef OURNOTES_TEST_BACKING_STORE
        return window.backingStore()->handle()->toImage();
#else
        return QImage();
#endif
    };
    for (auto* view : views) {
        if (view == views.back())
            view->expandAll();
        QTest::qWait(100);
        std::vector<QModelIndex> rows;  // as shown
        for (auto i = view->model()->index(0, 0); i.isValid() && rows.size() < 10; i = view->indexBelow(i))
            rows.push_back(i);
        REQUIRE(rows.size() >= 4);
        // one row, then one far from it (drawing a row next to the first would paint over
        // what it left behind)
        for (size_t n : {size_t(1), rows.size() - 1}) {
            QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->visualRect(rows[n]).center());
            QTest::qWait(100);
        }
        auto painted = shown().convertToFormat(QImage::Format_RGB32);
        auto fresh = window.grab().toImage().convertToFormat(QImage::Format_RGB32);  // drawn anew
        if (!painted.isNull())
            CHECK(painted == fresh);
    }
}

TEST_CASE("library: Esc in the search clears it and the selection")
{
    app();
    Env env;
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Alpha"}});
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Beta"}});
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    QApplication::processEvents();
    auto* search = window.findChild<SearchField*>("librarySearch");
    REQUIRE(search);
    QTreeView* items = nullptr;
    for (auto* t : window.findChild<LibraryView*>()->findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    search->setFocus();
    QTest::keyClicks(search, "Alp");
    CHECK(QTest::qWaitFor([&] { return items->model()->rowCount() == 1; }));
    items->setCurrentIndex(items->model()->index(0, 0));
    REQUIRE(items->selectionModel()->hasSelection());
    QTest::keyClick(search, Qt::Key_Escape);
    CHECK(search->text().isEmpty());
    CHECK(items->model()->rowCount() == 2);  // at once
    CHECK(!items->selectionModel()->hasSelection());
}

TEST_CASE("search: Up and Down go through earlier searches, those starting with what was typed")
{
    app();
    Env env;
    SearchField field;
    field.show();
    field.setFocus();
    for (auto s : {"robot", "rocket", "vision"}) {
        QTest::keyClicks(&field, s);
        QTest::keyClick(&field, Qt::Key_Return);  // kept
        field.clear();
    }
    CHECK(field.history() == QStringList{"robot", "rocket", "vision"});
    CHECK(env.config.get("/ui/searchHistory", Json()).dump() == R"(["robot","rocket","vision"])");
    std::vector<QString> seen;
    auto key = [&](Qt::Key k) {
        QTest::keyClick(&field, k);
        seen.push_back(field.text());
    };
    for (auto k : {Qt::Key_Up, Qt::Key_Up, Qt::Key_Up, Qt::Key_Up, Qt::Key_Down, Qt::Key_Down, Qt::Key_Down})
        key(k);
    CHECK(seen == std::vector<QString>{"vision", "rocket", "robot", "robot", "rocket", "vision", ""});
    // with "ro" typed, only those starting with it, and back to "ro"
    seen.clear();
    QTest::keyClicks(&field, "ro");
    for (auto k : {Qt::Key_Up, Qt::Key_Up, Qt::Key_Down, Qt::Key_Down})
        key(k);
    CHECK(seen == std::vector<QString>{"rocket", "robot", "rocket", "ro"});
    // the search just made still in the field (not typed now): Up goes through the others
    field.clear();
    QTest::keyClicks(&field, "vision");
    QTest::keyClick(&field, Qt::Key_Return);
    seen.clear();
    for (auto k : {Qt::Key_Up, Qt::Key_Up, Qt::Key_Down, Qt::Key_Down})
        key(k);
    CHECK(seen == std::vector<QString>{"rocket", "robot", "rocket", "vision"});
    // the next field (the next start) has them too
    SearchField again;
    CHECK(again.history() == QStringList{"robot", "rocket", "vision"});
}

TEST_CASE("reader: Esc in the find clears it and its hits; it has a history of its own")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto key = qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key"));
    ReaderTab tab(env.lib, key);
    tab.resize(1200, 900);
    tab.show();
    tab.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&tab));
    auto* find = tab.findChild<SearchField*>("findInDocument");
    REQUIRE(find);
    auto status = [&] {
        for (auto* l : tab.findChildren<QLabel*>())
            if (l->text().contains(" of ") || l->text() == "No results")
                return l->text();
        return QString();
    };
    find->setFocus();
    QTest::keyClicks(find, "line");
    QTest::keyClick(find, Qt::Key_Return);
    CHECK(QTest::qWaitFor([&] { return status() == "1 of 1"; }));
    QTest::keyClick(find, Qt::Key_Escape);
    CHECK(find->text().isEmpty());
    CHECK(status().isEmpty());
    CHECK(tab.view()->hasFocus());  // the page keys work again
    // Shift+Enter: backwards
    find->setFocus();
    QTest::keyClicks(find, "o");
    QTest::keyClick(find, Qt::Key_Return);
    CHECK(QTest::qWaitFor([&] { return status().startsWith("1 of "); }));
    QString total = status().mid(5);
    QTest::keyClick(find, Qt::Key_Return, Qt::ShiftModifier);
    CHECK(QTest::qWaitFor([&] { return status() == total + " of " + total; }));
    QTest::keyClick(find, Qt::Key_Escape);
    // Up: the word found before, from the find's own history
    find->setFocus();
    QTest::keyClick(find, Qt::Key_Up);
    QTest::keyClick(find, Qt::Key_Up);
    CHECK(find->text() == "line");
    CHECK(find->history() == QStringList{"line", "o"});
    CHECK(env.config.get("/reader/findHistory", Json()).dump() == R"(["line","o"])");
    CHECK(env.config.get("/ui/searchHistory", Json::array()).empty());
    // Esc in any other field: the keys back on the page
    find->clear();
    for (auto* field : tab.findChildren<QLineEdit*>()) {
        if (!field->isVisible() || field == find)
            continue;
        field->setFocus();
        REQUIRE(field->hasFocus());
        QTest::keyClick(field, Qt::Key_Escape);
        CHECK(tab.view()->hasFocus());
    }
}

TEST_CASE("library: I and N open Info and Notes, Esc on the list selects nothing")
{
    app();
    Env env;
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Alpha"}});
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    QTreeView* items = nullptr;
    for (auto* t : window.findChild<LibraryView*>()->findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    auto* tabs = window.findChild<ItemPane*>()->findChild<QTabWidget*>();
    items->setFocus();
    items->setCurrentIndex(items->model()->index(0, 0));
    QApplication::processEvents();
    QTest::keyClick(items, Qt::Key_N);
    CHECK(tabs->currentIndex() == 1);
    CHECK(env.config.get("/ui/itemPaneTab", "").get<std::string>() == "notes");
    QTest::keyClick(items, Qt::Key_I);
    CHECK(tabs->currentIndex() == 0);
    CHECK(items->hasFocus());
    REQUIRE(items->selectionModel()->hasSelection());
    QTest::keyClick(items, Qt::Key_Escape);
    CHECK(!items->selectionModel()->hasSelection());
}

TEST_CASE("library: Esc in a field gives the keys back to the item list")
{
    app();
    Env env;
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Alpha"}});
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    QTreeView* items = nullptr;
    for (auto* t : window.findChild<LibraryView*>()->findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    items->setCurrentIndex(items->model()->index(0, 0));
    QApplication::processEvents();
    int tried = 0;
    for (auto* field : window.findChild<LibraryView*>()->findChildren<QLineEdit*>()) {
        if (!field->isVisible() || field->isReadOnly())
            continue;
        field->setFocus();
        REQUIRE(field->hasFocus());
        QTest::keyClick(field, Qt::Key_Escape);
        CHECK(items->hasFocus());
        ++tried;
    }
    CHECK(tried >= 2);  // the search, the tag filter, the item's fields
}

TEST_CASE("search: Tab completes from earlier searches, then lists them")
{
    app();
    Env env;
    QWidget window;
    auto* field = new SearchField(&window);
    auto* next = new QLineEdit(&window);
    field->setGeometry(10, 10, 300, 30);
    next->setGeometry(10, 300, 300, 30);
    window.resize(400, 400);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    for (auto s : {"humanoid robot", "humanoid control", "vision"})
        field->remember(s);
    field->setFocus();
    // one search starting with it: the whole of it
    QTest::keyClicks(field, "vis");
    QTest::keyClick(field, Qt::Key_Tab);
    CHECK(field->text() == "vision");
    CHECK(field->hasFocus());
    // two: as far as both agree
    field->clear();
    QTest::keyClicks(field, "hum");
    QTest::keyClick(field, Qt::Key_Tab);
    CHECK(field->text() == "humanoid ");
    CHECK(!field->isListShown());
    // and Tab again: the list, the latest first, highlighted and in the field
    QTest::keyClick(field, Qt::Key_Tab);
    REQUIRE(field->isListShown());
    CHECK(field->listed() == QStringList{"humanoid control", "humanoid robot"});
    CHECK(field->listedCurrent() == 0);
    CHECK(field->text() == "humanoid control");
    QTest::keyClick(field, Qt::Key_Tab);
    CHECK(field->listedCurrent() == 1);
    CHECK(field->text() == "humanoid robot");
    QTest::keyClick(field, Qt::Key_Tab);  // round again
    CHECK(field->listedCurrent() == 0);
    QTest::keyClick(field, Qt::Key_Tab, Qt::ShiftModifier);
    CHECK(field->text() == "humanoid robot");
    QTest::keyClick(field, Qt::Key_Return);  // taken
    CHECK(!field->isListShown());
    CHECK(field->text() == "humanoid robot");
    // Esc in the list: back to what was typed
    field->clear();
    QTest::keyClicks(field, "humanoid ");
    QTest::keyClick(field, Qt::Key_Tab);
    REQUIRE(field->isListShown());
    QTest::keyClick(field, Qt::Key_Escape);
    CHECK(!field->isListShown());
    CHECK(field->text() == "humanoid ");
    // the one taken with Enter was searched again: the latest now, it comes first
    QTest::keyClick(field, Qt::Key_Tab);
    CHECK(field->listed() == QStringList{"humanoid robot", "humanoid control"});
    // typing goes on from the one shown, without the list
    QTest::keyClicks(field, "s");
    CHECK(!field->isListShown());
    CHECK(field->text() == "humanoid robots");
    // Tab is for completing: after a completion, or with nothing to complete, it stays
    field->clear();
    QTest::keyClicks(field, "vis");
    QTest::keyClick(field, Qt::Key_Tab);
    QTest::keyClick(field, Qt::Key_Tab);
    CHECK(field->text() == "vision");
    CHECK(field->hasFocus());
    field->clear();
    QTest::keyClicks(field, "zzz");
    QTest::keyClick(field, Qt::Key_Tab);
    CHECK(field->text() == "zzz");
    CHECK(field->hasFocus());
    // nothing typed (just after Ctrl+F, say): all of them, the latest first
    field->clear();
    QTest::keyClick(field, Qt::Key_Tab);
    REQUIRE(field->isListShown());
    QStringList latestFirst = field->history();
    std::reverse(latestFirst.begin(), latestFirst.end());
    CHECK(field->listed() == latestFirst);
    CHECK(field->text() == latestFirst.front());
    QTest::keyClick(field, Qt::Key_Escape);  // back to nothing
    CHECK(field->text().isEmpty());
    CHECK(field->hasFocus());
    // Shift+Tab, outside the list, moves on as anywhere else
    QTest::keyClick(field, Qt::Key_Tab, Qt::ShiftModifier);
    CHECK(!field->hasFocus());
    (void)next;
}

TEST_CASE("search: the list opens over a field with no room below it")
{
    app();
    Env env;
    QWidget window;
    auto* top = new SearchField(&window);
    auto* bottom = new SearchField(&window);
    top->setGeometry(10, 10, 300, 30);
    bottom->setGeometry(10, 360, 300, 30);
    window.resize(400, 400);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    for (auto* f : {top, bottom}) {
        for (auto s : {"one", "two", "three"})
            f->remember(s);
        f->setFocus();
        QTest::keyClick(f, Qt::Key_Tab);  // all of them
        REQUIRE(f->isListShown());
        QWidget* list = nullptr;
        for (auto* w : window.findChildren<QWidget*>("searchHistoryList"))
            if (w->isVisible())
                list = w;
        REQUIRE(list);
        if (f == top)
            CHECK(list->geometry().top() >= f->geometry().bottom());
        else
            CHECK(list->geometry().bottom() <= f->geometry().top());
        CHECK(window.rect().contains(list->geometry()));
        QTest::keyClick(f, Qt::Key_Escape);
    }
}

TEST_CASE("search: the list shows five, PgDn and PgUp go by five, Ctrl+Backspace forgets one")
{
    app();
    Env env;
    QWidget window;
    auto* field = new SearchField(&window);
    field->setGeometry(10, 10, 300, 30);
    window.resize(400, 400);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    for (int i = 1; i <= 12; ++i)
        field->remember(QString("paper %1").arg(i, 2, 10, QChar('0')));
    field->setFocus();
    QTest::keyClicks(field, "paper ");
    QTest::keyClick(field, Qt::Key_Tab);  // "paper 0" and "paper 1" share "paper " only: the list
    REQUIRE(field->isListShown());
    CHECK(field->listed().size() == 12);
    auto* list = window.findChild<QListWidget*>();
    REQUIRE(list);
    CHECK(list->height() == 5 * list->sizeHintForRow(0));  // five at a time
    CHECK(field->listedCount() == "1/12");
    CHECK(field->text() == "paper 12");  // the latest first
    QTest::keyClick(field, Qt::Key_PageDown);
    CHECK(field->listedCount() == "6/12");
    CHECK(field->text() == "paper 07");
    QTest::keyClick(field, Qt::Key_PageDown);
    QTest::keyClick(field, Qt::Key_PageDown);  // no further than the last
    CHECK(field->listedCount() == "12/12");
    QTest::keyClick(field, Qt::Key_PageUp);
    CHECK(field->listedCount() == "7/12");
    // Ctrl+Backspace: the highlighted one forgotten, the next one highlighted in its place
    QString gone = field->text();
    QTest::keyClick(field, Qt::Key_Backspace, Qt::ControlModifier);
    CHECK(!field->history().contains(gone));
    CHECK(field->listed().size() == 11);
    CHECK(field->listedCount() == "7/11");
    CHECK(field->text() != gone);
    CHECK(env.config.get("/ui/searchHistory", Json()).dump().find(ss(gone)) == std::string::npos);
    QTest::keyClick(field, Qt::Key_Escape);
    field->clear();
    QTest::keyClicks(field, "paper 0");  // 01 … 09, without the one forgotten
    QTest::keyClick(field, Qt::Key_Tab);
    REQUIRE(field->isListShown());
    CHECK(field->listed().size() == 8);
    CHECK(field->listedCount() == "1/8");
    QTest::keyClick(field, Qt::Key_Escape);
    // five or fewer: no count
    field->clear();
    QTest::keyClicks(field, "paper 1");  // 10, 11, 12
    QTest::keyClick(field, Qt::Key_Tab);
    REQUIRE(field->isListShown());
    CHECK(field->listed().size() == 3);
    CHECK(field->listedCount().isEmpty());
    QTest::keyClick(field, Qt::Key_Escape);
    // and the last 1000 kept
    for (int i = 0; i < 1005; ++i)
        field->remember(QString("search %1").arg(i));
    CHECK(field->history().size() == 1000);
    CHECK(field->history().back() == "search 1004");
}

TEST_CASE("search fields: Aa (or Alt+C) matches case, in the library and in a PDF")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Robot Arms"}});
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "robot legs"}});
    env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Vision"}});  // (the search applied: 2)
    auto pdf = qs(jstr(env.library.importAttachment(env.lib, file, jstr(paper, "key")), "key"));
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    QApplication::processEvents();
    auto* search = window.findChild<SearchField*>("librarySearch");
    REQUIRE(search);
    auto* button = search->findChild<QAction*>("matchCase");
    REQUIRE(button);
    QTreeView* items = nullptr;
    for (auto* t : window.findChild<LibraryView*>()->findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    search->setFocus();
    QTest::keyClicks(search, "Robot");
    CHECK(QTest::qWaitFor([&] { return items->model()->rowCount() == 2; }));
    button->trigger();  // as written now
    CHECK(search->caseSensitive());
    CHECK(items->model()->rowCount() == 1);
    CHECK(env.config.get("/ui/searchCaseSensitive", false).get<bool>());
    QTest::keyClick(search, Qt::Key_C, Qt::AltModifier);  // and back
    CHECK(!search->caseSensitive());
    CHECK(items->model()->rowCount() == 2);

    // a PDF's find
    ReaderTab tab(env.lib, pdf);
    tab.resize(1000, 800);
    tab.show();
    auto* find = tab.findChild<SearchField*>("findInDocument");
    REQUIRE(find);
    auto status = [&] {
        for (auto* l : tab.findChildren<QLabel*>())
            if (l->text().contains(" of ") || l->text() == "No results")
                return l->text();
        return QString();
    };
    find->setFocus();
    QTest::keyClicks(find, "hello");
    QTest::keyClick(find, Qt::Key_Return);
    CHECK(QTest::qWaitFor([&] { return status() == "1 of 1"; }));
    find->findChild<QAction*>("matchCase")->trigger();  // the hits again, as written
    CHECK(QTest::qWaitFor([&] { return status() == "No results"; }));
    find->clear();
    QTest::keyClicks(find, "Hello");
    QTest::keyClick(find, Qt::Key_Return);
    CHECK(QTest::qWaitFor([&] { return status() == "1 of 1"; }));
    CHECK(env.config.get("/reader/findCaseSensitive", false).get<bool>());
    // the next PDF opens matching case too
    ReaderTab again(env.lib, pdf);
    CHECK(again.findChild<SearchField*>("findInDocument")->caseSensitive());
}

TEST_CASE("notes: finding in a note never changes the window's minimum size")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto paper = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    auto note = env.library.createNote(env.lib, "<p>Alpha beta alpha</p><p>gamma Alpha</p>", jstr(paper, "key"));
    auto att = env.library.importAttachment(env.lib, file, jstr(paper, "key"));
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
    std::set<int> widths, heights;
    auto record = [&] {
        QApplication::processEvents();
        widths.insert(window.minimumWidth());
        heights.insert(window.minimumHeight());
    };
    auto typeAndDelete = [&](NoteEditor* editor) {
        REQUIRE(editor);
        auto* edit = editor->findChild<NoteTextEdit*>();
        edit->setFocus();
        QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier);
        auto* find = editor->findChild<SearchField*>("findInNote");
        REQUIRE(find->isVisible());
        record();
        // found, then not, then nothing typed, again and again
        for (auto keys : {"al", "alx", "", "gamma", "q", "", "alpha beta", ""}) {
            find->clear();
            QTest::keyClicks(find, keys);
            record();
            QTest::keyClick(find, Qt::Key_Backspace);
            record();
        }
        QTest::keyClick(find, Qt::Key_Escape);
        record();
    };
    // beside a PDF
    window.openAttachment(env.lib, qs(jstr(att, "key")));
    QApplication::processEvents();
    auto* reader = qobject_cast<ReaderTab*>(window.findChild<QTabWidget*>("mainTabs")->currentWidget());
    REQUIRE(reader);
    reader->findChild<QTabWidget*>("readerContextTabs")->setCurrentIndex(1);
    widths.clear();
    heights.clear();
    record();
    typeAndDelete(reader->findChild<NoteEditor*>());
    CHECK(widths.size() == 1);
    CHECK(heights.size() == 1);
    // and in the library's item pane
    window.findChild<QTabWidget*>("mainTabs")->setCurrentIndex(0);
    window.findChild<LibraryView*>()->selectItem(qs(jstr(note, "key")));
    QApplication::processEvents();
    NoteEditor* inPane = nullptr;
    for (auto* e : window.findChild<LibraryView*>()->findChildren<NoteEditor*>())
        if (e->isVisible() && e->key() == qs(jstr(note, "key")))
            inPane = e;
    widths.clear();
    heights.clear();
    record();
    typeAndDelete(inPane);
    CHECK(widths.size() == 1);
    CHECK(heights.size() == 1);
}

TEST_CASE("notes: Ctrl+F finds in the note, in a bar at its bottom that Esc closes")
{
    app();
    Env env;
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "book"}, {"title", "Paper"}});
    auto note = env.library.createNote(env.lib, "<p>Alpha beta alpha</p><p>gamma Alpha</p>", jstr(parent, "key"));
    MainWindow window;
    window.resize(1200, 700);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    window.findChild<LibraryView*>()->selectItem(qs(jstr(note, "key")));
    QApplication::processEvents();
    NoteEditor* editor = nullptr;
    for (auto* e : window.findChildren<NoteEditor*>())
        if (e->isVisible() && e->key() == qs(jstr(note, "key")))
            editor = e;
    REQUIRE(editor);
    auto* edit = editor->findChild<NoteTextEdit*>();
    auto* bar = editor->findChild<QWidget*>("noteFindBar");
    auto* find = editor->findChild<SearchField*>("findInNote");
    auto* status = editor->findChild<QLabel*>("noteFindStatus");
    REQUIRE(bar);
    CHECK(!bar->isVisible());
    edit->setFocus();
    edit->moveCursor(QTextCursor::Start);
    QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier);  // the note's, not the library's search
    REQUIRE(bar->isVisible());
    CHECK(find->hasFocus());
    CHECK(window.findChild<SearchField*>("librarySearch")->text().isEmpty());
    CHECK(bar->geometry().top() > edit->geometry().bottom() - 1);  // under the note
    QTest::keyClicks(find, "alpha");
    CHECK(status->text() == "1 of 3");
    CHECK(edit->extraSelections().size() == 3);
    CHECK(edit->textCursor().selectedText() == "Alpha");
    QTest::keyClick(find, Qt::Key_Return);
    CHECK(status->text() == "2 of 3");
    QTest::keyClick(find, Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClick(find, Qt::Key_Return, Qt::ShiftModifier);  // round to the last
    CHECK(status->text() == "3 of 3");
    find->findChild<QAction*>("matchCase")->trigger();
    CHECK(status->text() == "1 of 1");
    CHECK(edit->textCursor().selectedText() == "alpha");
    find->findChild<QAction*>("matchCase")->trigger();
    // Esc: no bar, no marks, the keys back in the note; the word kept for Up
    QTest::keyClick(find, Qt::Key_Escape);
    CHECK(!bar->isVisible());
    CHECK(edit->extraSelections().isEmpty());
    CHECK(edit->hasFocus());
    QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier);
    QTest::keyClick(find, Qt::Key_Up);
    CHECK(find->text() == "alpha");
    // Esc in the note itself closes the bar as well
    edit->setFocus();
    QTest::keyClick(edit, Qt::Key_Escape);
    CHECK(!bar->isVisible());
    CHECK(edit->hasFocus());
}

TEST_CASE("reader: Ctrl+F in the note beside the PDF finds in the note, on the page in the PDF")
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    auto parent = env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper"}});
    env.library.createNote(env.lib, "<p>my notes on the paper</p>", jstr(parent, "key"));
    auto pdf = qs(jstr(env.library.importAttachment(env.lib, file, jstr(parent, "key")), "key"));
    env.config.set("/ui/itemPaneTab", "notes");
    MainWindow window;
    window.resize(1400, 800);
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    window.openAttachment(env.lib, pdf);
    QApplication::processEvents();
    auto* tab = qobject_cast<ReaderTab*>(window.findChild<QTabWidget*>("mainTabs")->currentWidget());
    REQUIRE(tab);
    auto* edit = tab->findChild<NoteTextEdit*>();
    auto* bar = tab->findChild<QWidget*>("noteFindBar");
    auto* pdfFind = tab->findChild<SearchField*>("findInDocument");
    REQUIRE(edit);
    REQUIRE(bar);
    edit->setFocus();
    QTest::keyClick(edit, Qt::Key_F, Qt::ControlModifier);
    CHECK(bar->isVisible());
    auto* noteFind = tab->findChild<SearchField*>("findInNote");
    CHECK(noteFind->hasFocus());
    QTest::keyClicks(noteFind, "notes");
    QTest::keyClick(noteFind, Qt::Key_F, Qt::ControlModifier);  // again: still the note's
    CHECK(noteFind->hasFocus());
    CHECK(noteFind->selectedText() == "notes");
    CHECK(!pdfFind->hasFocus());
    QTest::keyClick(noteFind, Qt::Key_Escape);
    CHECK(!bar->isVisible());
    tab->view()->setFocus();
    QTest::keyClick(tab->view(), Qt::Key_F, Qt::ControlModifier);
    CHECK(pdfFind->hasFocus());
    CHECK(!bar->isVisible());
}

TEST_CASE("library: sorting by a column keeps the columns' widths")
{
    app();
    Env env;
    for (int i = 0; i < 30; ++i)
        env.library.createItem(env.lib, Json{{"itemType", "journalArticle"}, {"title", "Paper " + std::to_string(i)},
                                             {"date", "202" + std::to_string(i % 7) + "-0" + std::to_string(1 + i % 9)}});
    MainWindow window;
    window.resize(1500, 800);
    window.show();
    QApplication::processEvents();
    QTreeView* items = nullptr;
    for (auto* t : window.findChild<LibraryView*>()->findChildren<QTreeView*>())
        if (qobject_cast<ItemsModel*>(t->model()))
            items = t;
    REQUIRE(items);
    auto* header = items->header();
    header->showSection(ItemsModel::Date);  // with columns hidden on either side of it
    header->showSection(ItemsModel::Notes);
    QApplication::processEvents();
    auto widths = [&] {
        std::vector<int> w;
        for (int c = 0; c < header->count(); ++c)
            w.push_back(header->isSectionHidden(c) ? -1 : header->sectionSize(c));
        return w;
    };
    auto total = [&] {
        int t = 0;
        for (int w : widths())
            t += std::max(0, w);
        return t;
    };
    auto before = widths();
    REQUIRE(total() == items->viewport()->width());
    // the header clicked, as by the mouse: Date, Date again (descending), Creator, Title
    for (int c : {ItemsModel::Date, ItemsModel::Date, ItemsModel::Creator, ItemsModel::Title}) {
        QPoint at(header->sectionViewportPosition(c) + header->sectionSize(c) / 2, header->height() / 2);
        QTest::mouseClick(header->viewport(), Qt::LeftButton, {}, at);
        QApplication::processEvents();
        CHECK(header->sortIndicatorSection() == c);
        CHECK(widths() == before);
        CHECK(total() == items->viewport()->width());  // the last column ends where the list does
    }
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

// The README's screenshot, of a library of well-known papers (nobody's own), one of the
// background pictures behind it, picked at random. Run again
// after changing the look (one command):
//   OURNOTES_SCREENSHOT=docs/images/screenshot.png QT_SCALE_FACTOR=1.25
//   ./build/tests/gui/test_gui -tc="docs: *"
TEST_CASE("docs: the README's screenshot" * doctest::skip(!std::getenv("OURNOTES_SCREENSHOT")))
{
    app();
    Env env;
    TempDir tmp;
    auto file = tmp.path / "paper.pdf";
    writeSamplePdf(file);
    applyAppearance(Appearance::Light);
    QApplication::setFont(QFont("Noto Sans", 10));
    // one of the pictures behind the item list, any (the tests have none built in)
    std::vector<fs::path> pictures;
    auto source = (fs::path(OURNOTES_TEST_DATA_DIR) / ".." / ".." / "..").lexically_normal();
    for (auto& e : fs::directory_iterator(source / "resources" / "bands"))
        if (e.path().extension() == ".jpg" || e.path().extension() == ".png")
            pictures.push_back(e.path());
    REQUIRE(!pictures.empty());
    auto behind = env.library.dataDir() / "backgrounds" / "bands";
    fs::create_directories(behind);
    auto picture = pictures[QRandomGenerator::global()->bounded(int(pictures.size()))];
    fs::copy_file(picture, behind / picture.filename());
    env.config.set("/ui/bandBackground", true);
    env.config.set("/connector/enabled", false);  // no word in the status bar of a running OurNotes's port
    env.config.set("/ui/itemPaneTab", "notes");
    auto& lib = env.library;
    auto collection = [&](const std::string& name, const std::string& parent = {}) {
        return jstr(lib.createCollection(env.lib, name, parent), "key");
    };
    auto ml = collection("Machine Learning");
    auto transformers = collection("Transformers", ml);
    auto generative = collection("Generative Models", ml);
    auto rl = collection("Reinforcement Learning");
    auto vision = collection("Computer Vision");
    auto group = collection("Reading Group");
    // type, title, authors, date, venue, added; filed in `in`, with a PDF or not
    auto paper = [&](const std::string& type, const std::string& title, std::vector<std::string> authors,
                     const std::string& date, const std::string& venue, const std::string& added,
                     std::vector<std::string> in, bool pdf, Json more = Json::object()) {
        Json creators = Json::array();
        for (auto& a : authors) {
            auto space = a.rfind(' ');
            creators.push_back(Json{{"creatorType", "author"}, {"firstName", a.substr(0, space)}, {"lastName", a.substr(space + 1)}});
        }
        Json data{{"itemType", type}, {"title", title}, {"creators", creators}, {"date", date}, {"dateAdded", added}};
        data[type == "conferencePaper" ? "proceedingsTitle"
             : type == "preprint"      ? "repository"
             : type == "book"          ? "publisher"
                                       : "publicationTitle"] = venue;
        for (auto& [k, v] : more.items())
            data[k] = v;
        auto key = jstr(lib.createItem(env.lib, data), "key");
        for (auto& c : in)
            lib.addToCollection(env.lib, c, {key});
        if (pdf)
            lib.importAttachment(env.lib, file, key);
        return key;
    };
    auto attention = paper(
        "conferencePaper", "Attention Is All You Need",
        {"Ashish Vaswani", "Noam Shazeer", "Niki Parmar", "Jakob Uszkoreit", "Llion Jones", "Aidan N. Gomez",
         "Łukasz Kaiser", "Illia Polosukhin"},
        "2017", "Advances in Neural Information Processing Systems", "2026-09-14T09:12:00Z", {transformers, group}, true,
        Json{{"conferenceName", "NeurIPS 2017"},
             {"volume", "30"},
             {"pages", "5998–6008"},
             {"url", "https://arxiv.org/abs/1706.03762"},
             {"abstractNote", "A sequence transduction model built on attention alone, with no recurrence and no "
                              "convolutions: faster to train, and better at translation."}});
    paper("conferencePaper", "Deep Residual Learning for Image Recognition",
          {"Kaiming He", "Xiangyu Zhang", "Shaoqing Ren", "Jian Sun"}, "2016",
          "IEEE Conference on Computer Vision and Pattern Recognition", "2026-08-02T16:40:00Z", {vision}, true);
    paper("preprint", "Auto-Encoding Variational Bayes", {"Diederik P. Kingma", "Max Welling"}, "2013-12-20", "arXiv",
          "2026-07-21T10:05:00Z", {generative}, true);
    auto ddpm = paper("conferencePaper", "Denoising Diffusion Probabilistic Models",
                      {"Jonathan Ho", "Ajay Jain", "Pieter Abbeel"}, "2020",
                      "Advances in Neural Information Processing Systems", "2026-09-30T20:18:00Z", {generative, group}, true);
    paper("conferencePaper", "BERT: Pre-training of Deep Bidirectional Transformers for Language Understanding",
          {"Jacob Devlin", "Ming-Wei Chang", "Kenton Lee", "Kristina Toutanova"}, "2019", "Proceedings of NAACL-HLT",
          "2026-09-15T11:30:00Z", {transformers}, true);
    paper("conferencePaper", "Adam: A Method for Stochastic Optimization", {"Diederik P. Kingma", "Jimmy Ba"}, "2015",
          "International Conference on Learning Representations", "2026-06-11T08:47:00Z", {ml}, true);
    paper("conferencePaper", "Generative Adversarial Nets",
          {"Ian Goodfellow", "Jean Pouget-Abadie", "Mehdi Mirza", "Bing Xu", "David Warde-Farley", "Sherjil Ozair",
           "Aaron Courville", "Yoshua Bengio"},
          "2014", "Advances in Neural Information Processing Systems", "2026-07-21T10:12:00Z", {generative}, true);
    paper("conferencePaper", "ImageNet Classification with Deep Convolutional Neural Networks",
          {"Alex Krizhevsky", "Ilya Sutskever", "Geoffrey E. Hinton"}, "2012",
          "Advances in Neural Information Processing Systems", "2026-08-02T16:31:00Z", {vision}, true);
    paper("preprint", "Playing Atari with Deep Reinforcement Learning",
          {"Volodymyr Mnih", "Koray Kavukcuoglu", "David Silver", "Alex Graves", "Ioannis Antonoglou", "Daan Wierstra",
           "Martin Riedmiller"},
          "2013-12-19", "arXiv", "2026-05-03T14:22:00Z", {rl}, true);
    auto ppo = paper("preprint", "Proximal Policy Optimization Algorithms",
                     {"John Schulman", "Filip Wolski", "Prafulla Dhariwal", "Alec Radford", "Oleg Klimov"}, "2017-07-20",
                     "arXiv", "2026-05-03T14:30:00Z", {rl}, true);
    paper("conferencePaper", "Learning Transferable Visual Models From Natural Language Supervision",
          {"Alec Radford", "Jong Wook Kim", "Chris Hallacy", "Aditya Ramesh", "Gabriel Goh", "Sandhini Agarwal"}, "2021",
          "International Conference on Machine Learning", "2026-10-06T19:02:00Z", {vision, transformers}, true);
    paper("conferencePaper", "An Image is Worth 16x16 Words: Transformers for Image Recognition at Scale",
          {"Alexey Dosovitskiy", "Lucas Beyer", "Alexander Kolesnikov", "Dirk Weissenborn"}, "2021",
          "International Conference on Learning Representations", "2026-10-06T19:10:00Z", {vision, transformers}, true);
    paper("journalArticle", "Dropout: A Simple Way to Prevent Neural Networks from Overfitting",
          {"Nitish Srivastava", "Geoffrey Hinton", "Alex Krizhevsky", "Ilya Sutskever", "Ruslan Salakhutdinov"}, "2014",
          "Journal of Machine Learning Research", "2026-06-11T08:55:00Z", {ml}, false);
    paper("conferencePaper", "Batch Normalization: Accelerating Deep Network Training by Reducing Internal Covariate Shift",
          {"Sergey Ioffe", "Christian Szegedy"}, "2015", "International Conference on Machine Learning",
          "2026-06-11T09:01:00Z", {ml}, true);
    paper("conferencePaper", "Neural Machine Translation by Jointly Learning to Align and Translate",
          {"Dzmitry Bahdanau", "Kyunghyun Cho", "Yoshua Bengio"}, "2015",
          "International Conference on Learning Representations", "2026-09-14T09:20:00Z", {transformers, group}, true);
    paper("journalArticle", "Mastering the game of Go with deep neural networks and tree search",
          {"David Silver", "Aja Huang", "Chris J. Maddison", "Arthur Guez"}, "2016", "Nature", "2026-05-04T21:45:00Z", {rl},
          true);
    paper("conferencePaper", "U-Net: Convolutional Networks for Biomedical Image Segmentation",
          {"Olaf Ronneberger", "Philipp Fischer", "Thomas Brox"}, "2015", "Medical Image Computing and Computer-Assisted Intervention",
          "2026-08-02T16:52:00Z", {vision}, true);
    auto gpt3 = paper("conferencePaper", "Language Models are Few-Shot Learners",
                      {"Tom B. Brown", "Benjamin Mann", "Nick Ryder", "Melanie Subbiah"}, "2020",
                      "Advances in Neural Information Processing Systems", "2026-09-16T13:37:00Z", {transformers}, true);
    paper("journalArticle", "Long Short-Term Memory", {"Sepp Hochreiter", "Jürgen Schmidhuber"}, "1997",
          "Neural Computation", "2026-06-12T10:15:00Z", {ml}, true);
    paper("journalArticle", "A Mathematical Theory of Communication", {"Claude E. Shannon"}, "1948",
          "Bell System Technical Journal", "2026-04-27T18:03:00Z", {group}, true);
    paper("book", "Deep Learning", {"Ian Goodfellow", "Yoshua Bengio", "Aaron Courville"}, "2016", "MIT Press",
          "2026-04-20T12:00:00Z", {ml}, false);
    paper("journalArticle", "Human-level control through deep reinforcement learning",
          {"Volodymyr Mnih", "Koray Kavukcuoglu", "David Silver"}, "2015", "Nature", "2026-05-04T21:50:00Z", {rl}, true);
    lib.createNote(env.lib, "<p>Compare with additive attention (Bahdanau et al.)</p>", attention);
    lib.createNote(env.lib, "<p>Noise schedule: linear, 1000 steps</p>", ddpm);
    lib.createNote(env.lib, "<p>Clipped objective, ε = 0.2</p>", ppo);
    lib.createNote(env.lib, "<p>In-context learning, no gradient updates</p>", gpt3);
    lib.createNote(env.lib,
                   "<h2>Scaled dot-product attention</h2>"
                   "<pre class=\"math\">$$\\mathrm{Attention}(Q,K,V)=\\mathrm{softmax}\\left(\\frac{QK^\\top}{\\sqrt{d_k}}\\right)V$$</pre>"
                   "<p>Scaling by <span class=\"math\">$1/\\sqrt{d_k}$</span> keeps the softmax out of its flat "
                   "regions for large <span class=\"math\">$d_k$</span>.</p>"
                   "<ul><li>8 heads, <span class=\"math\">$d_k=d_v=64$</span></li><li>Positional encodings: sines and "
                   "cosines</li></ul>",
                   attention);
    for (auto& [tag, keys] : std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"transformer", {attention, gpt3}}, {"attention", {attention}}, {"nlp", {attention, gpt3}},
             {"diffusion", {ddpm}}, {"to read", {gpt3, ppo}}, {"classic", {attention}}})
        lib.addTag(env.lib, keys, tag);

    MainWindow window;
    window.resize(1600, 940);
    window.show();
    for (auto& key : {attention, ddpm})
        for (auto& a : lib.children(env.lib, key))
            if (jstr(a, "itemType") == "attachment")
                window.openAttachment(env.lib, qs(jstr(a, "key")));
    window.findChild<QTabWidget*>("mainTabs")->setCurrentIndex(0);
    auto* view = window.findChild<LibraryView*>();
    auto* tree = view->findChild<CollectionTree*>();
    tree->selectScope({env.lib, ScopeKind::Library, {}});
    tree->expandAll();
    view->selectItem(qs(attention), env.lib);
    for (int i = 0; i < 10; ++i) {
        QApplication::processEvents();
        QTest::qWait(50);
    }
    window.grab().save(QString::fromLocal8Bit(std::getenv("OURNOTES_SCREENSHOT")));
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
