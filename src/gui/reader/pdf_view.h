#pragma once

#include <QAbstractScrollArea>
#include <QTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QPolygonF>

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/json.h"
#include "pdf/document.h"

class QLabel;
class QPlainTextEdit;
class QVariantAnimation;

namespace on::gui {

class CommentBubble;
class LinkPreview;
class RenderWorker;
class SelectionPopup;

enum class ReaderTool { Select, Highlight, Underline, Note, Text, Area, Ink, Eraser };

// Text selected on a page: where it is (the union of its lines, points from the page's top left).
struct PdfQuote {
    QString text;
    int page = -1;
    QRectF place;
};

// Continuous-scroll PDF view with an annotation overlay. Annotations are passed
// in as Zotero-style item data and edits are reported through signals; the
// owner (ReaderTab) persists them in the library.
class PdfView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PdfView(QWidget* parent = nullptr);
    ~PdfView() override;

    void open(const fs::path& path);  // throws on::Error
    pdf::Document* document() const { return doc_.get(); }
    int pageCount() const { return (int)pageSizes_.size(); }
    int currentPage() const;
    void goToPage(int page, double yPoints = -1);
    // A place on a page (points from its top left), as a link to it names: scrolled to, and
    // marked for a moment.
    void showPlace(int page, const QRectF& place);
    // Links followed in the PDF (and places shown for links in notes) keep where the view was:
    // goBack (Ctrl+Left, the mouse's back button) returns there, goForward (Ctrl+Right) to
    // where it went back from. A link followed after going back drops what was forward.
    bool canGoBack() const { return !back_.empty(); }
    bool canGoForward() const { return !forward_.empty(); }
    void goBack();
    void goForward();
    // Where a link in the PDF leads, shown small while the mouse rests on it: the preview,
    // and the part of the page in it (points); for tests.
    QWidget* linkPreview() const;
    QRectF linkPreviewArea() const;  // (on the page it shows)

    double zoom() const;  // 1.0 = 100 %
    void setZoom(double zoom, QPointF anchor = {-1, -1});
    void zoomIn();
    void zoomOut();
    void fitWidth();
    void fitPage();
    bool isFitWidth() const { return fit_ == Fit::Width; }

    // Where the view is, in the format of Zotero's .zotero-reader-state: pageIndex,
    // scale (percent, "page-width" or "page-fit"), and the viewport's top-left corner as
    // PDF coordinates of that page (top, left). Restoring needs a laid-out view.
    Json viewState() const;
    void restoreViewState(const Json& state);

    ReaderTool tool() const { return tool_; }
    void setTool(ReaderTool tool);
    QString color() const { return color_; }
    void setColor(const QString& color);
    void setEditable(bool editable);
    // Whether "Add to Note" is offered (a PDF filed under an item); on by default.
    void setNotesAvailable(bool available);

    void setAnnotations(const std::vector<Json>& annotations);
    void selectAnnotation(const QString& key, bool scrollTo);
    QString selectedAnnotation() const { return selectedAnn_; }
    void editTextAnnotation(const QString& key);
    // An annotation's comment over the page: shown, or edited in place (edit).
    void showComment(const QString& key, bool edit);
    // Saves a comment being edited and closes it (before the view goes away).
    void finishComment();
    CommentBubble* commentBubble() const { return bubble_; }
    QImage annotationImage(const Json& annotation, double scale);

    bool hasTextSelection() const { return selAnchorPage_ >= 0 && selFocusPage_ >= 0; }
    QString selectedText() const;
    PdfQuote selectedQuote() const;
    void clearTextSelection();
    void copySelection();

    // Viewport position of a point in page space (points).
    QPointF pageToViewport(int page, QPointF pagePoint) const { return toView(page, pagePoint); }
    // Where the badge of an annotation with a comment is drawn (viewport coordinates);
    // empty when the annotation has none.
    QRectF commentBadgeRect(const QString& key) const;

    void find(const QString& text, bool backwards);
    void clearFind();
    void setFindCaseSensitive(bool on);  // the next find searches again
    void requestThumbnails(double scale);

    // While hidden, the view drops its rendered pages after imagesMs and its render
    // thread and MuPDF caches after allMs (defaults 3 s and 20 s).
    void setReleaseDelays(int imagesMs, int allMs);
    int renderedPages() const { return (int)cache_.size(); }
    bool isRendering() const { return worker_ != nullptr; }

    // Dark mode for the pages themselves (see darkPdfPages()); follows the theme.
    bool darkPages() const { return darkPages_; }
    void setDarkPages(bool dark);

signals:
    void darkPagesChanged(bool dark);
    void renderingRestarted();  // after being hidden for a while: ask for thumbnails again
    void pageChanged(int page);
    void zoomChanged(double zoom);
    void viewChanged();  // scrolled or zoomed
    void annotationCreated(const on::Json& data);
    void annotationsCreated(const std::vector<on::Json>& data);  // one per page of a selection across pages
    void annotationChanged(const on::Json& data);
    void annotationDeleted(const QString& key);
    void annotationSelected(const QString& key);
    void findStatus(int current, int total);
    void thumbnailRendered(int page, const QImage& image);
    void quoteToNote(const on::gui::PdfQuote& quote);       // the selection, "Add to Note"
    void referenceCopied(const on::gui::PdfQuote& quote);   // "Copy Reference": the selection's
    void annotationReferenceCopied(const QString& key);      // an annotation's
    void toolChangeRequested(on::gui::ReaderTool tool);
    void libraryRequested();  // H: back to the library
    void panelToggleRequested(bool right);  // [ and ]: the panel on the left, on the right
    void contextTabRequested(int tab);      // I, N: Info (0) or Notes (1) beside the page
    void darkPagesToggleRequested();        // L: dark pages or the PDF's own colours
    void paletteColorRequested(int index);  // Q, W, E: the first, second, third colour of one's own

protected:
    void paintEvent(QPaintEvent* e) override;
    void changeEvent(QEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool viewportEvent(QEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    enum class Fit { None, Width, Page };
    enum class Drag { None, Selecting, Area, Ink, Erasing, Moving, Resizing };

    struct AnnView {
        QString key;
        Json data;
        QString type;
        int page = -1;
        QColor color;
        std::vector<QRectF> rects;     // page space (points)
        std::vector<QRectF> nextRects;  // on the page after: text across two pages (Zotero's nextPageRects)
        QRectF nextBounds;
        std::vector<QPolygonF> paths;  // ink, page space
        QRectF bounds;
        double width = 2;
        double fontSize = 12;
        QString comment;
        bool badge = false;  // has a comment to show as a badge (see hasCommentBadge)
        bool external = false;
    };

    struct Cached {
        QImage image;
        double scale = 0;
    };

    // geometry
    void relayout(bool keepPosition = true);
    void updateScrollBars();
    QPointF scrollOffset() const;
    int pageAt(QPointF content, bool nearest = true) const;
    QPointF toPage(int page, QPointF content) const;
    QRectF toView(int page, const QRectF& pageRect) const;
    QPointF toView(int page, QPointF pagePoint) const;
    std::pair<int, int> visiblePages(double margin = 0) const;
    void requestRenders();
    void scrolled();
    int settling_ = 0;  // a layout or zoom is placing the view; scrolling meanwhile does not count
    QElapsedTimer fitKey_;  // since F was last pressed
    // A view not shown for a while gives back its rendered pages, render thread and
    // MuPDF caches; they come back when it is shown again.
    void releaseMemory();
    void startWorker();
    void onRendered(int page, double scale, quint64 generation, bool thumbnail, bool dark, const QImage& image);

    // annotations
    AnnView makeView(const Json& data) const;
    const AnnView* annotationAt(int page, QPointF pt) const;
    bool hasCommentBadge(const AnnView& a) const;
    QRectF commentBadge(const AnnView& a, const QRectF& bounds) const;  // bounds in page space
    const AnnView* commentBadgeAt(QPointF viewPos) const;
    AnnView* findAnn(const QString& key);
    Json positionFromRects(int page, const std::vector<QRectF>& rects) const;
    Json baseAnnotation(const QString& type, int page, int charOffset, double top) const;
    void createFromSelection(const QString& type, const QString& color);
    void createInk();
    void eraseAt(int page, QPointF pt);
    void commitMoved(Drag drag);
    // `resized`, when valid, is the annotation's new box (page space).
    Json movedData(const AnnView& a, QPointF delta, const QRectF& resized = QRectF()) const;
    // Resizing boxes (area and text annotations) by any edge or corner.
    enum Edge { EdgeLeft = 1, EdgeTop = 2, EdgeRight = 4, EdgeBottom = 8 };
    bool resizable(const AnnView& a) const;
    int edgesAt(const AnnView& a, QPointF viewPos) const;
    const AnnView* edgeAt(int page, QPointF viewPos, int* edges) const;
    QRectF resizedBounds(const AnnView& a) const;  // the box resized by the current drag
    void deleteSelectedAnnotation();
    void setAnnotationColor(const QString& key, const QString& color);
    void closeTextEditor(bool commit);
    QString commentKeyAt(QPointF viewPos) const;  // an annotation with a comment to show there
    void placeComment();

    // painting
    void paintAnnotations(QPainter& p, int page);
    void paintTextSelection(QPainter& p, int page);
    void paintLinks(QPainter& p, int page, const QImage& image, double imageScale, const QRectF& onView);
    void showPopup();
    void hidePopup();
    void updateCursor(QPointF viewPos);

    std::unique_ptr<pdf::Document> doc_;
    std::unique_ptr<RenderWorker> worker_;
    fs::path path_;
    QTimer releaseTimer_;  // hidden for a while: everything
    QTimer imagesTimer_;   // hidden for a moment: the rendered pages
    std::vector<QSizeF> pageSizes_;
    std::vector<QRectF> pageRects_;  // content coordinates (device-independent pixels)
    QSizeF contentSize_;
    double scale_ = 96.0 / 72.0;     // DIP per point
    Fit fit_ = Fit::Width;
    quint64 generation_ = 0;
    bool darkPages_ = false;
    int tone_ = 0;  // the page colour (pageTone) for darkPages_
    std::map<int, Cached> cache_;
    int lastPage_ = -1;

    ReaderTool tool_ = ReaderTool::Select;
    QString color_ = "#ffd400";
    bool editable_ = true;
    bool notesAvailable_ = true;

    std::vector<AnnView> anns_;
    QString selectedAnn_;

    // the text selected: from where the drag began (anchor: page, character) to where the
    // mouse is (focus), on one page or across pages; no focus yet: nothing selected
    int selAnchorPage_ = -1, selAnchor_ = -1, selFocusPage_ = -1, selFocus_ = -1;
    struct SelSpan {  // its characters [from, to] on a page
        int page, from, to;
    };
    std::vector<SelSpan> selectionSpans() const;  // in reading order, page by page
    static QString joinedText(QString before, const QString& after);  // the text of two pages, read on
    Drag drag_ = Drag::None;
    int dragPage_ = -1;
    QPointF dragStart_;  // page space
    QPointF dragNow_;    // page space
    int resizeEdges_ = 0;
    QPolygonF ink_;
    std::set<QString> erased_;
    std::map<QString, std::vector<QPolygonF>> erasedPaths_;
    QElapsedTimer tripleClick_;
    QString lastInkKey_;
    QElapsedTimer lastInkTime_;
    QString pressedLink_;
    int pressedLinkPage_ = -1;
    double pressedLinkY_ = 0;
    std::map<int, std::vector<pdf::Link>> links_;

    struct Place {  // where the view is: its top left corner, on a page (points from its top left)
        int page = -1;
        QPointF at;
    };
    Place place() const;
    void goTo(const Place& place);
    void remember();  // where the view is, before a link moves it (see goBack)
    void followLink(int page, double y);
    std::vector<Place> back_, forward_;

    // the link to a page under a point of the viewport: its page and index there (-1s: none)
    std::pair<int, int> pageLinkAt(QPointF pos);
    void hoverLink(int page, int index);
    void showLinkPreview();
    void hideLinkPreview();
    QImage drawnPart(int page, const QRectF& area, double scale);  // as the page is drawn (dark, its colour)
    LinkPreview* linkPreview_ = nullptr;
    QTimer linkPreviewTimer_;
    QTimer previewLeaveTimer_;  // the mouse left the link: hidden, unless it went into the preview
    int previewPage_ = -1, previewIndex_ = -1;  // the link the mouse rests on

    // where pictures are drawn on a page (page points), which the page colour leaves as they
    // are; one over most of the page (a scan) is its paper
    const std::vector<QRectF>& pictureRects(int page);
    std::map<int, std::vector<QRectF>> pictureRects_;

    int placePage_ = -1;  // a place shown (showPlace), fading
    QRectF place_;
    double placeShown_ = 0;
    QVariantAnimation* placeFade_ = nullptr;

    QString findText_;
    bool findCaseSensitive_ = false;
    std::vector<std::pair<int, std::vector<QRectF>>> hits_;
    int hitIndex_ = -1;

    SelectionPopup* popup_ = nullptr;
    QPlainTextEdit* textEditor_ = nullptr;
    QString editingKey_;
    CommentBubble* bubble_ = nullptr;
    QTimer hoverTimer_;    // the mouse rests on an annotation: show its comment
    QTimer unhoverTimer_;  // it left: hide the comment
    QString hoverKey_;
    QString pressedAnn_;  // a click on it (selected already, or on its badge) edits its comment
    QPointF pressPos_;
    QPointF mousePos_{-1, -1};  // in the viewport, while the mouse is over it
};

} // namespace on::gui
