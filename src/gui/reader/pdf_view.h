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

class QPlainTextEdit;

namespace on::gui {

class CommentBubble;
class RenderWorker;
class SelectionPopup;

enum class ReaderTool { Select, Highlight, Underline, Note, Text, Area, Ink, Eraser };

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

    bool hasTextSelection() const { return selPage_ >= 0 && selFrom_ >= 0; }
    QString selectedText() const;
    void clearTextSelection();
    void copySelection();

    // Viewport position of a point in page space (points).
    QPointF pageToViewport(int page, QPointF pagePoint) const { return toView(page, pagePoint); }
    // Where the badge of an annotation with a comment is drawn (viewport coordinates);
    // empty when the annotation has none.
    QRectF commentBadgeRect(const QString& key) const;

    void find(const QString& text, bool backwards);
    void clearFind();
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
    void annotationChanged(const on::Json& data);
    void annotationDeleted(const QString& key);
    void annotationSelected(const QString& key);
    void findStatus(int current, int total);
    void thumbnailRendered(int page, const QImage& image);
    void addToNote(const QString& html);
    void toolChangeRequested(on::gui::ReaderTool tool);
    void libraryRequested();  // H: back to the library
    void panelToggleRequested(bool right);  // [ and ]: the panel on the left, on the right
    void contextTabRequested(int tab);      // I, N: Info (0) or Notes (1) beside the page
    void darkPagesToggleRequested();        // L: dark pages or the PDF's own colours

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

    std::vector<AnnView> anns_;
    QString selectedAnn_;

    int selPage_ = -1, selFrom_ = -1, selTo_ = -1, selAnchor_ = -1;
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

    QString findText_;
    std::vector<std::pair<int, std::vector<QRectF>>> hits_;
    int hitIndex_ = -1;

    SelectionPopup* popup_ = nullptr;
    QPlainTextEdit* textEditor_ = nullptr;
    QString editingKey_;
    CommentBubble* bubble_ = nullptr;
    QTimer hoverTimer_;    // the mouse rests on an annotation: show its comment
    QTimer unhoverTimer_;  // it left: hide the comment
    QString hoverKey_;
    QString pressedAnn_;  // a click on it edits its comment
    QPointF pressPos_;
    QPointF mousePos_{-1, -1};  // in the viewport, while the mouse is over it
};

} // namespace on::gui
