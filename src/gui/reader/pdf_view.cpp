#include "gui/reader/pdf_view.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegion>
#include <QScrollBar>
#include <QStyleHints>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVariantAnimation>

#include <cmath>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"
#include "gui/memory.h"
#include "gui/reader/comment_bubble.h"
#include "gui/reader/render_worker.h"
#include "gui/theme.h"

namespace on::gui {

namespace {

// The text box being typed in: keys it has no use for stay with it, instead of going on to
// the page (Ctrl+Shift+Space turned it, Alt+Left went a page back...).
class TextBoxEdit : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        QPlainTextEdit::keyPressEvent(e);
        e->accept();
    }
};

constexpr double kMargin = 12;
constexpr double kGap = 10;
constexpr double kBaseScale = 96.0 / 72.0;
constexpr double kNoteSize = 22;  // points
const double kZoomSteps[] = {0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0};

double r3(double v) { return std::round(v * 1000) / 1000; }

// The badge on an annotation that has a comment: a speech bubble in the annotation's
// colour, just outside its top-right corner, the same size at every zoom.
constexpr double kBadgeWidth = 16;
constexpr double kBadgeHeight = 13;

void paintCommentBadge(QPainter& p, const QRectF& r, const QColor& color)
{
    // the outline is the same for every badge; uniting paths is slow, so it is made once
    static const QPainterPath shape = [] {
        QRectF b(0, 0, kBadgeWidth, kBadgeHeight);
        QPainterPath bubble;
        bubble.addRoundedRect(b, 3, 3);
        QPainterPath tail;  // pointing down-left, at the annotation's corner
        tail.moveTo(b.left() + 2, b.bottom() - 2);
        tail.lineTo(b.left() - 3, b.bottom() + 4);
        tail.lineTo(b.left() + 7, b.bottom());
        tail.closeSubpath();
        return bubble.united(tail);
    }();
    p.setPen(QPen(color.darker(170), 1));
    p.setBrush(color);
    p.drawPath(shape.translated(r.topLeft()));
    // two lines of "text": dark on bright colours (yellow), light on darker ones (blue)
    double luminance = 0.299 * color.redF() + 0.587 * color.greenF() + 0.114 * color.blueF();
    QColor ink = luminance > 0.6 ? QColor(0, 0, 0, 160) : QColor(255, 255, 255, 230);
    p.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap));
    double y = r.center().y();
    p.drawLine(QPointF(r.left() + 4, y - 2), QPointF(r.right() - 4, y - 2));
    p.drawLine(QPointF(r.left() + 4, y + 2), QPointF(r.right() - 6, y + 2));
}

QRectF toQ(const pdf::Rect& r) { return QRectF(QPointF(r.x0, r.y0), QPointF(r.x1, r.y1)); }
pdf::Rect toP(const QRectF& r) { return {(float)r.left(), (float)r.top(), (float)r.right(), (float)r.bottom()}; }

double distanceToSegment(QPointF p, QPointF a, QPointF b)
{
    QPointF ab = b - a, ap = p - a;
    double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = len2 > 0 ? std::clamp((ap.x() * ab.x() + ap.y() * ab.y()) / len2, 0.0, 1.0) : 0;
    QPointF d = p - (a + t * ab);
    return std::hypot(d.x(), d.y());
}

} // namespace

// Floating buttons shown under a text selection (like Zotero's selection popup).
class SelectionPopup : public QFrame {
public:
    SelectionPopup(PdfView* view, QWidget* parent) : QFrame(parent)
    {
        setFrameShape(QFrame::StyledPanel);
        setAutoFillBackground(true);
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(4, 3, 4, 3);
        h->setSpacing(2);
        for (auto& c : schema::kAnnotationColors) {
            auto* b = new QToolButton(this);
            b->setIcon(colorIcon(c.hex, 16));
            b->setToolTip(QObject::tr("Highlight (%1)").arg(c.name));
            b->setAutoRaise(true);
            QObject::connect(b, &QToolButton::clicked, this, [this, hex = QString(c.hex)] { highlight(hex); });
            h->addWidget(b);
        }
        underline_ = addButton(h, icon("tool-underline"), QObject::tr("Underline"));
        copy_ = addButton(h, icon("copy"), QObject::tr("Copy"));
        reference_ = addButton(h, icon("type-link"), QObject::tr("Copy Reference (pasted into a note: the text, and a link here)"));
        note_ = addButton(h, icon("new-note"), QObject::tr("Add to Note"));
        Q_UNUSED(view);
    }
    QToolButton* underline_;
    QToolButton* copy_;
    QToolButton* reference_;
    QToolButton* note_;
    std::function<void(const QString&)> highlight;

private:
    QToolButton* addButton(QHBoxLayout* h, const QIcon& ic, const QString& tip)
    {
        auto* b = new QToolButton(this);
        b->setIcon(ic);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        h->addWidget(b);
        return b;
    }
};

// Where a link leads, small, under it (see PdfView::showLinkPreview): the pages as drawn, from
// where the link points on. With the mouse in it, the wheel scrolls it (with Shift, or a
// sideways wheel: across), on into the pages after and before; Ctrl+wheel zooms it, a drag
// moves it. Each time it is shown it begins afresh.
class LinkPreview : public QWidget {
public:
    explicit LinkPreview(QWidget* parent) : QWidget(parent)
    {
        setObjectName("linkPreview");
        setCursor(Qt::OpenHandCursor);
        hide();
    }

    std::function<QImage(int page, const QRectF& area, double scale)> draw;  // a page's part (points)
    std::function<QSizeF(int page)> pageSize;  // points
    int pages = 0;
    std::function<void(bool inside)> hovered;  // the mouse came in, or left

    void start(int page, QPointF at, QSize size, double scale)
    {
        page_ = page;
        at_ = at;
        scale_ = scale;
        tiles_.clear();
        resize(size);
        update();
    }
    QRectF area() const { return QRectF(at_, QSizeF(size()) / scale_); }  // on page(), points
    int page() const { return page_; }
    double scale() const { return scale_; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Dark));  // (between pages)
        double dpr = devicePixelRatioF();
        double y = -at_.y() * scale_;  // the top of page_, here
        int page = page_;
        for (; page < pages && y < height(); ++page) {
            QSizeF size = pageSize(page);
            QRectF onWidget(-at_.x() * scale_, y, size.width() * scale_, size.height() * scale_);
            QRectF shown = onWidget & QRectF(rect());
            if (!shown.isEmpty()) {
                auto& tile = tileFor(page, QRectF((shown.topLeft() - onWidget.topLeft()) / scale_, shown.size() / scale_));
                // pixel for pixel from a device pixel (see PdfView::paintEvent; the tile is whole ones)
                QPointF at = (onWidget.topLeft() + tile.area.topLeft() * scale_) * dpr;
                p.drawImage(QRectF(QPointF(std::round(at.x()), std::round(at.y())) / dpr, tile.area.size() * scale_), tile.image);
            }
            y += size.height() * scale_ + kGap;
        }
        // the tiles of the pages here kept, and one each side (wheeled on through many: not all)
        tiles_.erase(tiles_.begin(), tiles_.lower_bound(page_ - 1));
        tiles_.erase(tiles_.upper_bound(page), tiles_.end());
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
    void wheelEvent(QWheelEvent* e) override
    {
        if (e->modifiers() & Qt::ControlModifier) {
            if (double steps = e->angleDelta().y() / 120.0; steps != 0)
                zoom(std::pow(1.1, steps), e->position());
        } else {
            QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 3 : QPointF(e->pixelDelta());
            if ((e->modifiers() & Qt::ShiftModifier) && d.x() == 0)
                d = QPointF(d.y(), 0);
            scrollBy(-d);
        }
        e->accept();  // (not the page's)
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        dragFrom_ = e->position();
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton) {
            scrollBy(dragFrom_ - e->position());
            dragFrom_ = e->position();
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override { setCursor(Qt::OpenHandCursor); }
    void enterEvent(QEnterEvent*) override
    {
        if (hovered)
            hovered(true);
    }
    void leaveEvent(QEvent*) override
    {
        if (hovered)
            hovered(false);
    }
    void hideEvent(QHideEvent*) override { tiles_.clear(); }  // (drawn again when next shown)

private:
    static constexpr int kGap = 6;  // pixels between pages
    struct Tile {
        QRectF area;  // points
        QImage image;
    };

    double gap() const { return kGap / scale_; }  // points
    void scrollBy(QPointF pixels)
    {
        at_ += pixels / scale_;
        settle();
        update();
    }
    void zoom(double factor, QPointF pos)
    {
        double scale = std::clamp(scale_ * factor, std::min(0.4, scale_), std::max(4.0, scale_));  // (begun past these: no further)
        QPointF under = at_ + pos / scale_;  // (stays under the mouse)
        scale_ = scale;
        at_ = under - pos / scale_;
        tiles_.clear();
        settle();
        update();
    }
    // the top on the page it is in; not past the first page's top, the last one's foot, the sides
    void settle()
    {
        while (at_.y() < 0 && page_ > 0)
            at_.ry() += pageSize(--page_).height() + gap();
        while (page_ + 1 < pages && at_.y() >= pageSize(page_).height() + gap())
            at_.ry() -= pageSize(page_++).height() + gap();
        QSizeF shown = QSizeF(size()) / scale_, page = pageSize(page_);
        if (page_ + 1 >= pages)
            at_.setY(std::min(at_.y(), std::max(0.0, page.height() - shown.height())));
        at_.setY(std::max(0.0, at_.y()));
        at_.setX(std::clamp(at_.x(), 0.0, std::max(0.0, page.width() - shown.width())));
    }
    // a part of a page drawn, around what is to be shown (moving a little draws nothing again)
    const Tile& tileFor(int page, const QRectF& needed)
    {
        auto it = tiles_.find(page);
        // (where a page's edge shows, rounding can put needed a hair past area)
        if (it == tiles_.end() || !it->second.area.adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(needed)) {
            QRectF area = needed.adjusted(-needed.width() / 2, -needed.height(), needed.width() / 2, needed.height()) &
                          QRectF(QPointF(), pageSize(page));
            double px = scale_ * devicePixelRatioF();  // (to whole device pixels, as it is drawn)
            QRect pixels = QRectF(area.topLeft() * px, area.size() * px).toAlignedRect();
            area = QRectF(QPointF(pixels.topLeft()) / px, QSizeF(pixels.size()) / px);
            it = tiles_.insert_or_assign(page, Tile{area, draw(page, area, scale_)}).first;
        }
        return it->second;
    }

    int page_ = 0;
    QPointF at_;  // the top left, on page_ (points)
    double scale_ = 1;
    std::map<int, Tile> tiles_;
    QPointF dragFrom_;
};

PdfView::PdfView(QWidget* parent) : QAbstractScrollArea(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    verticalScrollBar()->setSingleStep(40);
    horizontalScrollBar()->setSingleStep(40);
    popup_ = new SelectionPopup(this, viewport());
    popup_->hide();
    bubble_ = new CommentBubble(viewport());
    hoverTimer_.setSingleShot(true);
    hoverTimer_.setInterval(350);
    connect(&hoverTimer_, &QTimer::timeout, this, [this] {
        if (!hoverKey_.isEmpty() && commentKeyAt(mousePos_) == hoverKey_)
            showComment(hoverKey_, false);
    });
    linkPreview_ = new LinkPreview(viewport());
    linkPreview_->draw = [this](int page, const QRectF& area, double scale) { return drawnPart(page, area, scale); };
    linkPreview_->pageSize = [this](int page) { return pageSizes_[page]; };
    linkPreview_->hovered = [this](bool inside) {
        if (inside)
            previewLeaveTimer_.stop();  // (moved from the link into it: kept)
        else
            hideLinkPreview();
    };
    linkPreviewTimer_.setSingleShot(true);
    linkPreviewTimer_.setInterval(350);
    connect(&linkPreviewTimer_, &QTimer::timeout, this, &PdfView::showLinkPreview);
    previewLeaveTimer_.setSingleShot(true);
    previewLeaveTimer_.setInterval(300);  // (time to move the mouse from the link into it)
    connect(&previewLeaveTimer_, &QTimer::timeout, this, [this] {
        if (!linkPreview_->underMouse())
            hideLinkPreview();
    });
    unhoverTimer_.setSingleShot(true);
    unhoverTimer_.setInterval(250);
    connect(&unhoverTimer_, &QTimer::timeout, this, [this] {
        if (bubble_->isVisible() && !bubble_->isEditing() && !bubble_->underMouse() &&
            commentKeyAt(mousePos_) != bubble_->key())
            bubble_->finish(false);
    });
    connect(bubble_, &CommentBubble::left, &unhoverTimer_, qOverload<>(&QTimer::start));
    connect(bubble_, &CommentBubble::editRequested, this, [this](const QString& key) { showComment(key, true); });
    connect(bubble_, &CommentBubble::edited, this, [this](const QString& key, const QString& comment) {
        auto* a = findAnn(key);
        if (!a || a->external || !editable_)
            return;
        Json data = a->data;
        data["annotationComment"] = ss(comment);
        emit annotationChanged(data);
    });
    darkPages_ = darkPdfPages();
    tone_ = pageTone(darkPages_);
    if (auto* ctx = AppContext::tryGet()) {
        connect(ctx, &AppContext::appearanceChanged, this, [this] {
            setDarkPages(darkPdfPages());
            viewport()->update();  // (links tinted or not)
        });
        connect(ctx, &AppContext::pageToneChanged, this, [this] {
            tone_ = pageTone(darkPages_);
            viewport()->update();
        });
    }
    popup_->highlight = [this](const QString& c) { createFromSelection("highlight", c); };
    connect(popup_->underline_, &QToolButton::clicked, this, [this] { createFromSelection("underline", color_); });
    connect(popup_->copy_, &QToolButton::clicked, this, [this] {
        copySelection();
        hidePopup();
    });
    connect(popup_->reference_, &QToolButton::clicked, this, [this] {
        if (hasTextSelection())
            emit referenceCopied(selectedQuote());
        hidePopup();
    });
    connect(popup_->note_, &QToolButton::clicked, this, [this] {
        if (hasTextSelection())
            emit quoteToNote(selectedQuote());
        hidePopup();
    });
    placeFade_ = new QVariantAnimation(this);
    placeFade_->setDuration(2400);
    placeFade_->setStartValue(1.0);
    placeFade_->setKeyValueAt(0.5, 1.0);  // (shown a while, then fading)
    placeFade_->setEndValue(0.0);
    connect(placeFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        placeShown_ = v.toDouble();
        viewport()->update();
    });
    lastInkTime_.invalidate();
    releaseTimer_.setSingleShot(true);
    releaseTimer_.setInterval(20000);
    connect(&releaseTimer_, &QTimer::timeout, this, &PdfView::releaseMemory);
    // rendered pages are the bulk (megabytes each) and quick to render again
    imagesTimer_.setSingleShot(true);
    imagesTimer_.setInterval(3000);
    connect(&imagesTimer_, &QTimer::timeout, this, [this] {
        cache_.clear();
        ++generation_;
        if (worker_)
            worker_->setQueue({});  // nor those still to come (shown again, they are asked for)
        releaseFreedMemory();
    });
}

PdfView::~PdfView()
{
    worker_.reset();
    doc_.reset();
    releaseFreedMemory();
}

void PdfView::open(const fs::path& path)
{
    doc_ = pdf::Document::open(path);
    if (doc_->needsPassword())
        throw Error("This PDF is password-protected.");
    pageSizes_.clear();
    for (int i = 0; i < doc_->pageCount(); ++i) {
        auto s = doc_->pageSize(i);
        pageSizes_.push_back(QSizeF(s.width, s.height));
    }
    path_ = path;
    cache_.clear();
    links_.clear();
    linkGlyphs_.clear();
    pictureRects_.clear();
    startWorker();
    relayout(false);
}

void PdfView::startWorker()
{
    worker_ = std::make_unique<RenderWorker>(path_);
    connect(worker_.get(), &RenderWorker::rendered, this, &PdfView::onRendered);
}

void PdfView::setReleaseDelays(int imagesMs, int allMs)
{
    imagesTimer_.setInterval(imagesMs);
    releaseTimer_.setInterval(allMs);
}

void PdfView::releaseMemory()
{
    if (!worker_)
        return;
    worker_.reset();  // stops its thread and closes its copy of the document
    cache_.clear();
    ++generation_;
    if (doc_)
        doc_->releaseMemory();
    releaseFreedMemory();
}

void PdfView::showEvent(QShowEvent* e)
{
    QAbstractScrollArea::showEvent(e);
    releaseTimer_.stop();
    imagesTimer_.stop();
    if (worker_)
        requestRenders();  // (only the pages it has no image of, e.g. all after imagesTimer_)
    if (!worker_ && !path_.empty())
        // not for a tab shown only in passing, e.g. while several tabs are being closed
        QTimer::singleShot(50, this, [this] {
            if (isVisible() && !worker_) {
                startWorker();
                requestRenders();
                emit renderingRestarted();
            }
        });
}

void PdfView::hideEvent(QHideEvent* e)
{
    QAbstractScrollArea::hideEvent(e);
    hideLinkPreview();
    if (!e->spontaneous()) {  // another tab was chosen (not a minimised window)
        bubble_->finish(true);
        imagesTimer_.start();
        releaseTimer_.start();
    }
}

// --- geometry ------------------------------------------------------------------------------

QPointF PdfView::scrollOffset() const
{
    return QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

void PdfView::relayout(bool keepPosition)
{
    // remember what is at the top of the viewport
    int anchorPage = -1;
    double anchorY = 0;
    if (keepPosition && !pageRects_.empty()) {
        QPointF top = scrollOffset() + QPointF(0, 1);
        anchorPage = pageAt(top);
        anchorY = toPage(anchorPage, top).y();
    }
    double vpW = viewport()->width(), vpH = viewport()->height();
    double maxW = 0, maxH = 0;
    for (auto& s : pageSizes_) {
        maxW = std::max(maxW, s.width());
        maxH = std::max(maxH, s.height());
    }
    if (maxW > 0) {
        if (fit_ == Fit::Width)
            scale_ = std::max(0.1, (vpW - 2 * kMargin) / maxW);
        else if (fit_ == Fit::Page)
            scale_ = std::max(0.1, std::min((vpW - 2 * kMargin) / maxW, (vpH - 2 * kMargin) / maxH));
    }
    double contentW = maxW * scale_ + 2 * kMargin;
    double width = std::max(contentW, vpW);
    pageRects_.clear();
    double y = kMargin;
    for (auto& s : pageSizes_) {
        double w = s.width() * scale_, h = s.height() * scale_;
        pageRects_.push_back(QRectF((width - w) / 2, y, w, h));
        y += h + kGap;
    }
    contentSize_ = QSizeF(contentW, y - kGap + kMargin);
    // The scroll bars take the new size before the view is put back where it was; in
    // between, the old offset points elsewhere (dozens of pages away, deep into a long
    // document), and the renders asked for there dropped the pages in view: blank
    // pages while zooming. Only the final place counts.
    ++settling_;
    updateScrollBars();
    if (anchorPage >= 0)
        verticalScrollBar()->setValue(int(pageRects_[anchorPage].top() + anchorY * scale_ - 1));
    --settling_;
    if (settling_ == 0)
        scrolled();
    else
        viewport()->update();
}

void PdfView::updateScrollBars()
{
    auto vp = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, int(contentSize_.width() - vp.width())));
    horizontalScrollBar()->setPageStep(vp.width());
    verticalScrollBar()->setRange(0, std::max(0, int(contentSize_.height() - vp.height())));
    verticalScrollBar()->setPageStep(vp.height());
}

int PdfView::pageAt(QPointF content, bool nearest) const
{
    for (size_t i = 0; i < pageRects_.size(); ++i)
        if (content.y() <= pageRects_[i].bottom() + kGap / 2)
            return (nearest || pageRects_[i].contains(content)) ? (int)i : -1;
    return nearest && !pageRects_.empty() ? (int)pageRects_.size() - 1 : -1;
}

QPointF PdfView::toPage(int page, QPointF content) const
{
    return (content - pageRects_[page].topLeft()) / scale_;
}

QRectF PdfView::toView(int page, const QRectF& r) const
{
    QPointF origin = pageRects_[page].topLeft() - scrollOffset();
    return QRectF(origin + r.topLeft() * scale_, r.size() * scale_);
}

QPointF PdfView::toView(int page, QPointF p) const
{
    return pageRects_[page].topLeft() - scrollOffset() + p * scale_;
}

std::pair<int, int> PdfView::visiblePages(double margin) const
{
    double top = verticalScrollBar()->value() - margin;
    double bottom = verticalScrollBar()->value() + viewport()->height() + margin;
    int first = -1, last = -1;
    for (size_t i = 0; i < pageRects_.size(); ++i) {
        if (pageRects_[i].bottom() < top)
            continue;
        if (pageRects_[i].top() > bottom)
            break;
        if (first < 0)
            first = (int)i;
        last = (int)i;
    }
    return {first, last};
}

int PdfView::currentPage() const
{
    if (pageRects_.empty())
        return 0;
    double y = verticalScrollBar()->value() + viewport()->height() / 3.0;
    return std::max(0, pageAt(QPointF(0, y)));
}

void PdfView::goToPage(int page, double yPoints)
{
    if (page < 0 || page >= pageCount())
        return;
    double y = pageRects_[page].top() - kGap / 2;
    if (yPoints >= 0)
        y = pageRects_[page].top() + yPoints * scale_ - viewport()->height() / 4.0;
    verticalScrollBar()->setValue(int(y));
}

double PdfView::zoom() const { return scale_ / kBaseScale; }

void PdfView::setZoom(double zoom, QPointF anchor)
{
    zoom = std::clamp(zoom, 0.1, 8.0);
    QPointF content, pagePt;
    int page = -1;
    if (anchor.x() >= 0) {
        content = anchor + scrollOffset();
        page = pageAt(content);
        if (page >= 0)
            pagePt = toPage(page, content);
    }
    fit_ = Fit::None;
    scale_ = zoom * kBaseScale;
    ++generation_;
    ++settling_;  // see relayout
    relayout(page < 0);
    if (page >= 0) {
        QPointF newContent = pageRects_[page].topLeft() + pagePt * scale_;
        horizontalScrollBar()->setValue(int(newContent.x() - anchor.x()));
        verticalScrollBar()->setValue(int(newContent.y() - anchor.y()));
    }
    --settling_;
    scrolled();
    emit zoomChanged(this->zoom());
}

Json PdfView::viewState() const
{
    Json state = Json::object();
    if (!doc_ || pageRects_.empty())
        return state;
    int page = currentPage();
    // the corner may lie outside the page (in the margin, or on the page above); the
    // page's coordinate system extends there
    QPointF corner = toPage(page, scrollOffset());
    auto pdf = doc_->pageToPdf(page, pdf::Point{float(corner.x()), float(corner.y())});
    state["pageIndex"] = page;
    if (fit_ == Fit::Width)
        state["scale"] = "page-width";
    else if (fit_ == Fit::Page)
        state["scale"] = "page-fit";
    else
        state["scale"] = std::round(zoom() * 1000) / 10;
    // tenths of a point, so the view comes back to the pixel (Zotero reads either)
    state["top"] = std::round(double(pdf.y) * 10) / 10;
    state["left"] = std::round(double(pdf.x) * 10) / 10;
    state["scrollMode"] = 0;
    state["spreadMode"] = 0;
    return state;
}

void PdfView::restoreViewState(const Json& state)
{
    if (!doc_ || pageRects_.empty())
        return;
    auto scale = state.value("scale", Json());
    if (scale.is_number()) {
        fit_ = Fit::None;
        scale_ = std::clamp(scale.get<double>() / 100, 0.1, 8.0) * kBaseScale;
    } else if (scale == "page-fit") {
        fit_ = Fit::Page;
    } else if (scale.is_string()) {  // "page-width", or pdf.js's "auto"
        fit_ = Fit::Width;
    }
    ++generation_;
    relayout(false);
    emit zoomChanged(zoom());
    auto pageIndex = state.value("pageIndex", Json());
    int page = pageIndex.is_number_integer() ? std::clamp(pageIndex.get<int>(), 0, pageCount() - 1) : 0;
    auto top = state.value("top", Json()), left = state.value("left", Json());
    if (top.is_number() && left.is_number()) {
        auto p = doc_->pdfToPage(page, pdf::Point{left.get<float>(), top.get<float>()});
        QPointF corner = pageRects_[page].topLeft() + QPointF(p.x, p.y) * scale_;
        horizontalScrollBar()->setValue(int(std::lround(corner.x())));
        verticalScrollBar()->setValue(int(std::lround(corner.y())));
    } else {
        goToPage(page);
    }
}

void PdfView::zoomIn()
{
    double z = zoom();
    for (double s : kZoomSteps)
        if (s > z + 0.01) {
            setZoom(s);
            return;
        }
}

void PdfView::zoomOut()
{
    double z = zoom();
    for (int i = (int)std::size(kZoomSteps) - 1; i >= 0; --i)
        if (kZoomSteps[i] < z - 0.01) {
            setZoom(kZoomSteps[i]);
            return;
        }
}

void PdfView::fitWidth()
{
    fit_ = Fit::Width;
    ++generation_;
    relayout();
    emit zoomChanged(zoom());
}

void PdfView::fitPage()
{
    fit_ = Fit::Page;
    ++generation_;
    relayout();
    emit zoomChanged(zoom());
}

void PdfView::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    double old = scale_;
    relayout();
    if (std::abs(old - scale_) > 1e-6) {
        ++generation_;
        emit zoomChanged(zoom());
    }
    placeComment();
}

void PdfView::scrollContentsBy(int, int)
{
    if (settling_ == 0)
        scrolled();
}

// After the view moved (or its layout changed): what follows the pages, the renders
// for where it is now, and the page it shows.
void PdfView::scrolled()
{
    hidePopup();
    hideLinkPreview();
    placeComment();
    if (textEditor_ && !editingKey_.isEmpty())
        if (auto* a = findAnn(editingKey_))
            textEditor_->setGeometry(toView(a->page, a->rects.empty() ? a->bounds : a->rects[0]).toRect());
    requestRenders();
    viewport()->update();
    int page = currentPage();
    if (page != lastPage_) {
        lastPage_ = page;
        emit pageChanged(page);
    }
    emit viewChanged();
}

// --- rendering ------------------------------------------------------------------------------

void PdfView::requestRenders()
{
    if (!worker_)
        return;
    double dpr = devicePixelRatioF();
    renderDpr_ = dpr;
    double want = scale_ * dpr;
    auto [first, last] = visiblePages(viewport()->height() * 0.75);
    if (first < 0)
        return;
    double centerY = verticalScrollBar()->value() + viewport()->height() / 2.0;
    std::vector<int> pages;
    for (int i = first; i <= last; ++i) {
        auto it = cache_.find(i);
        if (it == cache_.end() || std::abs(it->second.scale - want) > 1e-3)
            pages.push_back(i);
    }
    std::sort(pages.begin(), pages.end(), [&](int a, int b) {
        return std::abs(pageRects_[a].center().y() - centerY) < std::abs(pageRects_[b].center().y() - centerY);
    });
    std::deque<RenderWorker::Request> q;
    for (int p : pages)
        q.push_back({p, want, generation_, false, darkPages_});
    worker_->setQueue(std::move(q));
    // drop images far from the viewport
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->first < first - 4 || it->first > last + 4)
            it = cache_.erase(it);
        else
            ++it;
    }
}

void PdfView::onRendered(int page, double scale, quint64 generation, bool thumbnail, bool dark, const QImage& image)
{
    if (dark != darkPages_)
        return;  // rendered before dark mode was switched
    if (thumbnail) {
        emit thumbnailRendered(page, image);
        return;
    }
    double want = scale_ * devicePixelRatioF();
    auto it = cache_.find(page);
    bool exact = std::abs(scale - want) < 1e-3;
    if (!exact && it != cache_.end() && generation < generation_)
        return;  // keep the existing image until the right one arrives
    QImage img = image;
    img.setDevicePixelRatio(devicePixelRatioF());
    cache_[page] = {img, scale, {}, false};
    auto [first, last] = visiblePages();
    if (page >= first && page <= last)
        viewport()->update(pageRects_[page].translated(-scrollOffset()).toAlignedRect());
}

void PdfView::requestThumbnails(double scale, const std::vector<bool>& have)
{
    if (!worker_)
        return;
    std::deque<RenderWorker::Request> q;
    for (int i = 0; i < pageCount(); ++i)
        if (i >= (int)have.size() || !have[i])
            q.push_back({i, scale, 0, true, darkPages_});
    worker_->addRequests(q);
}

QImage PdfView::annotationImage(const Json& data, double scale)
{
    auto v = makeView(data);
    if (!doc_ || v.page < 0 || v.rects.empty())
        return {};
    try {
        auto img = doc_->renderClip(v.page, (float)scale, toP(v.rects[0]));
        return QImage(img.pixels.data(), img.width, img.height, img.stride, QImage::Format_RGB888).copy();
    } catch (const std::exception&) {
        return {};
    }
}

void PdfView::setDarkPages(bool dark)
{
    if (dark == darkPages_)
        return;
    darkPages_ = dark;
    tone_ = pageTone(dark);  // the one remembered for these pages
    ++generation_;
    cache_.clear();
    requestRenders();
    viewport()->update();
    emit darkPagesChanged(dark);
}

void PdfView::changeEvent(QEvent* e)
{
    QAbstractScrollArea::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        setDarkPages(darkPdfPages());
}

// --- painting --------------------------------------------------------------------------------

void PdfView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    QColor bg = palette().color(QPalette::Window).lightness() > 128 ? QColor(0xd9, 0xd9, 0xdc) : QColor(0x2a, 0x2a, 0x2e);
    p.fillRect(viewport()->rect(), bg);
    auto [first, last] = visiblePages();
    if (first < 0)
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    double dpr = devicePixelRatioF();
    // moved to a screen of another pixel density (nothing else asks for pages then): the
    // pages at this one, instead of the others' stretched until the next scroll
    if (worker_ && dpr != renderDpr_)
        requestRenders();
    for (int i = first; i <= last; ++i) {
        QRectF r = pageRects_[i].translated(-scrollOffset());
        p.fillRect(r.adjusted(1, 2, 2, 3), QColor(0, 0, 0, 45));
        p.fillRect(r, darkPages_ ? darkPageColor() : QColor(Qt::white));
        auto it = cache_.find(i);
        if (it != cache_.end() && std::abs(it->second.scale - scale_ * dpr) < 1e-3) {
            // pixel for pixel from a device pixel: stretched into the page's rect, which
            // is a pixel larger or smaller and often between pixels (at 150 % always),
            // every glyph was resampled and blurred
            QPointF at(std::round(r.x() * dpr) / dpr, std::round(r.y() * dpr) / dpr);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(at, it->second.image);
            paintLinks(p, i, it->second, QRectF(at, QSizeF(it->second.image.size()) / dpr));
            p.setRenderHint(QPainter::SmoothPixmapTransform);
        } else if (it != cache_.end()) {
            p.drawImage(r, it->second.image);  // another zoom's, until this one's arrives
            paintLinks(p, i, it->second, r);  // (its links as marked meanwhile)
        }
        if (tone_ > 0) {  // the page colour: white (or black) paper towards gray, not the text on
            // it (no lighter, or darker, than the gray) nor the pictures
            p.save();
            QRegion paper(r.toAlignedRect());
            for (auto& picture : pictureRects(i))
                paper -= QRegion(toView(i, picture).toAlignedRect());
            p.setClipRegion(paper);
            p.setCompositionMode(darkPages_ ? QPainter::CompositionMode_Lighten : QPainter::CompositionMode_Darken);
            p.fillRect(r, pageToneColor(darkPages_, tone_));
            p.restore();
        }
        paintAnnotations(p, i);
        paintTextSelection(p, i);
        if (placePage_ == i && placeShown_ > 0) {  // a place a link named (showPlace)
            QColor c = palette().color(QPalette::Highlight);
            c.setAlphaF(0.4 * placeShown_);
            p.fillRect(toView(i, place_.adjusted(-2, -2, 2, 2)), c);
        }
        // find hits
        for (int h = 0; h < (int)hits_.size(); ++h) {
            if (hits_[h].first != i)
                continue;
            QColor c = h == hitIndex_ ? QColor(255, 140, 0, 110) : QColor(255, 220, 0, 90);
            for (auto& rect : hits_[h].second)
                p.fillRect(toView(i, rect), c);
        }
        // tool previews
        if (dragPage_ == i && drag_ == Drag::Area) {
            QRectF area = QRectF(dragStart_, dragNow_).normalized();
            p.setPen(QPen(QColor(color_), 2, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(toView(i, area));
        }
        if (dragPage_ == i && drag_ == Drag::Ink && ink_.size() > 1) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor(color_), 2 * scale_, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPolygonF poly;
            for (auto& pt : ink_)
                poly << toView(i, pt);
            p.drawPolyline(poly);
        }
    }
}

void PdfView::paintAnnotations(QPainter& p, int page)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPointF delta;
    bool resizing = drag_ == Drag::Resizing && dragPage_ == page;
    if (drag_ == Drag::Moving && dragPage_ == page)
        delta = dragNow_ - dragStart_;
    for (auto& a : anns_) {
        bool onNext = a.page == page - 1 && !a.nextRects.empty();  // (its rest, on this page)
        if (a.page != page && !onNext)
            continue;
        bool selected = a.key == selectedAnn_;
        QPointF d = selected ? delta : QPointF();
        QColor c = a.color;
        const auto& paths = erasedPaths_.count(a.key) ? erasedPaths_[a.key] : a.paths;  // (not copied each paint)
        if (a.type == "highlight" || a.type == "underline") {
            for (auto& r : onNext ? a.nextRects : a.rects) {
                QRectF v = toView(page, r);
                if (a.type == "highlight") {
                    QColor fill = c;
                    fill.setAlphaF(0.42);
                    // multiply darkens the paper under the text; on dark pages, screen lightens it
                    p.setCompositionMode(darkPages_ ? QPainter::CompositionMode_Screen
                                                    : QPainter::CompositionMode_Multiply);
                    p.fillRect(v, fill);
                    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                } else {
                    double th = std::max(1.5, v.height() * 0.09);
                    p.fillRect(QRectF(v.left(), v.bottom() - th, v.width(), th), c);
                }
            }
        } else if (a.type == "note") {
            QRectF v = toView(page, a.bounds.translated(d));
            QPainterPath path;
            double fold = v.width() * 0.3;
            path.moveTo(v.topLeft());
            path.lineTo(v.topRight());
            path.lineTo(v.right(), v.bottom() - fold);
            path.lineTo(v.right() - fold, v.bottom());
            path.lineTo(v.bottomLeft());
            path.closeSubpath();
            p.setPen(QPen(c.darker(140), 1));
            p.setBrush(c);
            p.drawPath(path);
            QPainterPath corner;
            corner.moveTo(v.right(), v.bottom() - fold);
            corner.lineTo(v.right() - fold, v.bottom() - fold);
            corner.lineTo(v.right() - fold, v.bottom());
            corner.closeSubpath();
            p.setBrush(c.darker(125));
            p.drawPath(corner);
        } else if (a.type == "text") {
            if (a.key == editingKey_)
                continue;
            QRectF r = selected && resizing ? resizedBounds(a) : a.bounds.translated(d);
            QRectF v = toView(page, r);
            QFont f = font();
            f.setPixelSize(std::max(4, int(a.fontSize * scale_)));
            p.setFont(f);
            p.setPen(c);
            p.drawText(v, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, a.comment);
        } else if (a.type == "image") {
            QRectF r = selected && resizing ? resizedBounds(a) : a.bounds.translated(d);
            p.setPen(QPen(c, std::max(1.5, 2 * scale_ / kBaseScale)));
            p.setBrush(Qt::NoBrush);
            p.drawRect(toView(page, r));
        } else if (a.type == "ink") {
            p.setPen(QPen(c, std::max(1.0, a.width * scale_), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (auto& path : paths) {
                QPolygonF poly;
                for (auto& pt : path)
                    poly << toView(page, pt + d);
                p.drawPolyline(poly);
            }
        }
        if (selected) {
            QRectF b = onNext ? a.nextBounds : resizing ? resizedBounds(a) : a.bounds.translated(d);
            QRectF v = toView(page, b).adjusted(-3, -3, 3, 3);
            p.setPen(QPen(palette().color(QPalette::Highlight), 1.5, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(v);
            if (resizable(a)) {
                // handles on the corners and edges, each of which can be dragged
                p.setPen(QPen(palette().color(QPalette::Base), 1));
                p.setBrush(palette().color(QPalette::Highlight));
                QPointF c = v.center();
                for (QPointF h : {v.topLeft(), QPointF(c.x(), v.top()), v.topRight(), QPointF(v.right(), c.y()),
                                  v.bottomRight(), QPointF(c.x(), v.bottom()), v.bottomLeft(), QPointF(v.left(), c.y())})
                    p.drawRect(QRectF(h - QPointF(4, 4), QSizeF(8, 8)));
            }
        }
    }
    for (auto& a : anns_)
        if (a.page == page && hasCommentBadge(a)) {
            bool selected = a.key == selectedAnn_;
            QRectF b = selected && resizing ? resizedBounds(a) : a.bounds.translated(selected ? delta : QPointF());
            paintCommentBadge(p, commentBadge(a, b), a.color);
        }
    p.restore();
}

namespace {

// Whether the ink in a part of a page is coloured (most of what is not paper: a link's own
// colour), or black or gray (on dark pages: white).
bool colouredInk(const QImage& part, bool darkPages)
{
    int ink = 0, coloured = 0;
    for (int y = 0; y < part.height(); ++y) {
        auto* line = reinterpret_cast<const QRgb*>(part.constScanLine(y));
        for (int x = 0; x < part.width(); ++x) {
            int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            int most = std::max({r, g, b}), least = std::min({r, g, b});
            if (darkPages ? most < 70 : least > 185)
                continue;  // (paper, and the edges of letters)
            ++ink;
            if (most - least > 60)
                ++coloured;
        }
    }
    return ink > 0 && coloured * 2 > ink;
}

} // namespace

// The text of the links on a page marked, and bold: a PDF does not always mark its links (a
// reference's number, a figure's) apart from its text. Its part of the page (`cached`'s image,
// drawn over `onView`; another zoom's too, stretched, while the zoom changes): black or gray
// ink turned blue (on dark pages: the light ink light blue), the paper as it is; ink of a
// colour of its own kept. Drawn again a pixel to the right, the darker (lighter) of the two
// kept: thicker strokes. Not over pictures. The parts made once an image (each paint, at
// every step of a scroll, was milliseconds a page).
void PdfView::paintLinks(QPainter& p, int page, Cached& cached, const QRectF& onView)
{
    if (!highlightPdfLinks())
        return;
    const QImage& image = cached.image;
    double imageScale = cached.scale;
    if (image.isNull() || imageScale <= 0)
        return;
    if (!cached.linksMarked) {
        if (!links_.count(page))
            links_[page] = doc_->links(page);
        QColor ink = darkPages_ ? QColor(150, 190, 255) : QColor(20, 100, 235);
        auto& glyphs = linkGlyphs(page);
        for (size_t i = 0; i < links_[page].size() && i < glyphs.size(); ++i) {
            QRectF area = toQ(links_[page][i].rect);
            bool onPicture = false;
            for (auto& picture : pictureRects(page))
                if (auto common = picture & area; common.width() * common.height() > 0.3 * area.width() * area.height())
                    onPicture = true;
            // its letters, whole (not those its box cuts, nor the next line's), and only they
            QRectF letters;
            double height = 0;
            for (auto& g : glyphs[i]) {
                letters = letters.isNull() ? g : letters.united(g);
                height += g.height();
            }
            QRect pixels = QRectF(letters.topLeft() * imageScale, letters.size() * imageScale).toAlignedRect() & image.rect();
            if (onPicture || pixels.isEmpty())
                continue;
            QImage part = image.copy(pixels).convertToFormat(QImage::Format_RGB32);
            if (!colouredInk(part, darkPages_)) {  // (screened: black ink blue, white paper white)
                QPainter q(&part);
                q.setCompositionMode(darkPages_ ? QPainter::CompositionMode_Multiply : QPainter::CompositionMode_Screen);
                q.fillRect(part.rect(), ink);
            }
            cached.links.push_back({pixels, std::move(part), glyphs[i], height / glyphs[i].size()});
        }
        cached.linksMarked = true;
    }
    if (cached.links.empty())
        return;
    double dpr = devicePixelRatioF(), across = onView.width() / image.width();  // (view units a pixel)
    auto onScreen = [&](const QRectF& r) {  // page points: where the image has them drawn
        return QRectF(onView.topLeft() + r.topLeft() * imageScale * across, r.size() * imageScale * across);
    };
    p.save();
    for (auto& link : cached.links) {
        QPainterPath clip;
        for (auto& g : link.glyphs)
            clip.addRect(onScreen(g));
        QRectF to(onView.topLeft() + QPointF(link.pixels.topLeft()) * across, QSizeF(link.pixels.size()) * across);
        p.setClipPath(clip);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawImage(to, link.part);
        // bold: as much thicker as the letters are high (a pixel or more), the darker (lighter) kept
        p.setCompositionMode(darkPages_ ? QPainter::CompositionMode_Lighten : QPainter::CompositionMode_Darken);
        int bold = std::clamp(int(std::lround(link.height * imageScale * across * dpr * 0.04)), 1, 4);
        for (int k = 1; k <= bold; ++k)
            p.drawImage(to.translated(k / dpr, 0), link.part);
    }
    p.restore();
}

const std::vector<std::vector<QRectF>>& PdfView::linkGlyphs(int page)
{
    auto it = linkGlyphs_.find(page);
    if (it != linkGlyphs_.end())
        return it->second;
    auto& text = doc_->text(page);
    std::vector<std::vector<QRectF>> all;
    for (auto& link : links_[page]) {
        QRectF area = toQ(link.rect);
        std::vector<QRectF> glyphs;
        for (auto& c : text.chars)
            if (c.c != ' ' && area.contains(toQ(c.box).center()))
                glyphs.push_back(toQ(c.box));
        if (glyphs.empty())  // (no text: the box)
            glyphs.push_back(area);
        all.push_back(std::move(glyphs));
    }
    return linkGlyphs_[page] = std::move(all);
}

void PdfView::paintTextSelection(QPainter& p, int page)
{
    QColor c = palette().color(QPalette::Highlight);
    c.setAlphaF(0.35);
    for (auto& span : selectionSpans())
        if (span.page == page)
            for (auto& r : doc_->text(page).rectsOf(span.from, span.to))
                p.fillRect(toView(page, toQ(r)), c);
}

// --- annotations -------------------------------------------------------------------------------

void PdfView::setAnnotations(const std::vector<Json>& annotations)
{
    anns_.clear();
    for (auto& a : annotations) {
        auto v = makeView(a);
        if (v.page >= 0 && v.page < pageCount())
            anns_.push_back(std::move(v));
    }
    if (!selectedAnn_.isEmpty() && !findAnn(selectedAnn_))
        selectedAnn_.clear();
    placeComment();
    viewport()->update();
}

PdfView::AnnView PdfView::makeView(const Json& data) const
{
    AnnView v;
    v.key = jqs(data, "key");
    v.data = data;
    v.type = jqs(data, "annotationType");
    v.color = QColor(jqs(data, "annotationColor"));
    if (!v.color.isValid())
        v.color = QColor("#ffd400");
    v.comment = jqs(data, "annotationComment");
    // a sticky note's or a text box's comment is what it shows; the others get a badge
    v.badge = v.type != "note" && v.type != "text" && !v.comment.trimmed().isEmpty();
    v.external = jbool(data, "annotationIsExternal");
    auto pos = data.value("annotationPosition", Json::object());
    if (pos.is_string()) {
        try {
            pos = Json::parse(pos.get<std::string>());
        } catch (...) {
            pos = Json::object();
        }
    }
    v.page = (int)jint(pos, "pageIndex", -1);
    if (!doc_ || v.page < 0 || v.page >= doc_->pageCount())
        return v;
    v.width = pos.value("width", 2.0);
    v.fontSize = pos.value("fontSize", 12.0);
    for (auto& r : pos.value("rects", Json::array())) {
        if (!r.is_array() || r.size() != 4)
            continue;
        auto pr = doc_->pdfToPage(v.page, pdf::Rect{r[0].get<float>(), r[1].get<float>(), r[2].get<float>(),
                                                     r[3].get<float>()});
        v.rects.push_back(toQ(pr));
        v.bounds = v.bounds.isNull() ? toQ(pr) : v.bounds.united(toQ(pr));
    }
    if (v.page + 1 < doc_->pageCount())  // (the rest of text across two pages)
        for (auto& r : pos.value("nextPageRects", Json::array())) {
            if (!r.is_array() || r.size() != 4)
                continue;
            auto pr = doc_->pdfToPage(v.page + 1, pdf::Rect{r[0].get<float>(), r[1].get<float>(), r[2].get<float>(),
                                                             r[3].get<float>()});
            v.nextRects.push_back(toQ(pr));
            v.nextBounds = v.nextBounds.isNull() ? toQ(pr) : v.nextBounds.united(toQ(pr));
        }
    for (auto& path : pos.value("paths", Json::array())) {
        QPolygonF poly;
        for (size_t i = 0; i + 1 < path.size(); i += 2) {
            auto pt = doc_->pdfToPage(v.page, pdf::Point{path[i].get<float>(), path[i + 1].get<float>()});
            poly << QPointF(pt.x, pt.y);
        }
        if (!poly.isEmpty()) {
            v.paths.push_back(poly);
            auto br = poly.boundingRect().adjusted(-v.width, -v.width, v.width, v.width);
            v.bounds = v.bounds.isNull() ? br : v.bounds.united(br);
        }
    }
    if (v.type == "note" && !v.rects.empty())
        v.bounds = QRectF(v.rects[0].topLeft(), QSizeF(kNoteSize, kNoteSize));
    return v;
}

PdfView::AnnView* PdfView::findAnn(const QString& key)
{
    for (auto& a : anns_)
        if (a.key == key)
            return &a;
    return nullptr;
}

const PdfView::AnnView* PdfView::annotationAt(int page, QPointF pt) const
{
    // later annotations are drawn on top: search backwards; prefer point annotations
    const AnnView* found = nullptr;
    for (auto it = anns_.rbegin(); it != anns_.rend(); ++it) {
        auto& a = *it;
        if (a.page == page - 1 && !found)  // (the rest of text across two pages, on this page)
            for (auto& r : a.nextRects)
                if (r.contains(pt))
                    found = &a;
        if (a.page != page)
            continue;
        if (a.type == "note" || a.type == "text" || a.type == "image") {
            QRectF b = a.bounds.adjusted(-2, -2, 2, 2);
            if (a.type == "image" && !b.contains(pt))
                continue;
            if (b.contains(pt))
                return &a;
        } else if (a.type == "ink") {
            for (auto& path : a.paths)
                for (int i = 1; i < path.size(); ++i)
                    if (distanceToSegment(pt, path[i - 1], path[i]) < a.width + 3)
                        return &a;
        } else if (!found) {
            for (auto& r : a.rects)
                if (r.contains(pt))
                    found = &a;
        }
    }
    return found;
}

// worked out when the annotation is loaded: this is asked on every paint and mouse move
bool PdfView::hasCommentBadge(const AnnView& a) const { return a.badge; }

QRectF PdfView::commentBadge(const AnnView& a, const QRectF& bounds) const
{
    QRectF v = toView(a.page, bounds);
    return QRectF(v.right() + 3, v.top() - kBadgeHeight - 1, kBadgeWidth, kBadgeHeight);
}

const PdfView::AnnView* PdfView::commentBadgeAt(QPointF viewPos) const
{
    auto [first, last] = visiblePages(kBadgeHeight);
    for (auto it = anns_.rbegin(); it != anns_.rend(); ++it)
        if (it->page >= first && it->page <= last && hasCommentBadge(*it) &&
            commentBadge(*it, it->bounds).adjusted(-3, -2, 2, 4).contains(viewPos))
            return &*it;
    return nullptr;
}

QRectF PdfView::commentBadgeRect(const QString& key) const
{
    for (auto& a : anns_)
        if (a.key == key)
            return hasCommentBadge(a) ? commentBadge(a, a.bounds) : QRectF();
    return {};
}

void PdfView::selectAnnotation(const QString& key, bool scrollTo)
{
    selectedAnn_ = key;
    if (scrollTo)
        if (auto* a = findAnn(key)) {
            QRectF v = toView(a->page, a->bounds);
            if (!viewport()->rect().contains(v.toRect()))
                goToPage(a->page, a->bounds.top());
        }
    viewport()->update();
}

Json PdfView::positionFromRects(int page, const std::vector<QRectF>& rects) const
{
    Json arr = Json::array();
    for (auto& r : rects) {
        auto pr = doc_->pageToPdf(page, toP(r));
        arr.push_back({r3(pr.x0), r3(pr.y0), r3(pr.x1), r3(pr.y1)});
    }
    return Json{{"pageIndex", page}, {"rects", arr}};
}

Json PdfView::baseAnnotation(const QString& type, int page, int charOffset, double top) const
{
    return Json{{"key", newKey()},
                {"itemType", "annotation"},
                {"annotationType", ss(type)},
                {"annotationText", ""},
                {"annotationComment", ""},
                {"annotationColor", ss(color_)},
                {"annotationPageLabel", doc_->pageLabel(page)},
                {"annotationSortIndex", pdf::sortIndex(page, charOffset, (float)top)},
                {"tags", Json::array()}};
}

void PdfView::createFromSelection(const QString& type, const QString& color)
{
    if (!hasTextSelection() || !editable_)
        return;
    // text across two pages: one annotation, the rest on the next page (as Zotero has it,
    // nextPageRects); across more, one for each two
    std::vector<Json> made;
    auto spans = selectionSpans();
    auto rectsOf = [&](const SelSpan& span) {
        std::vector<QRectF> rects;
        for (auto& r : doc_->text(span.page).rectsOf(span.from, span.to))
            rects.push_back(toQ(r));
        return rects;
    };
    for (size_t i = 0; i < spans.size(); ++i) {
        auto& span = spans[i];
        auto rects = rectsOf(span);
        if (rects.empty())
            continue;
        double top = rects.front().top();
        for (auto& r : rects)
            top = std::min(top, r.top());
        auto data = baseAnnotation(type, span.page, span.from, top);
        data["annotationColor"] = ss(color);
        auto text = qs(doc_->text(span.page).textOf(span.from, span.to));
        auto position = positionFromRects(span.page, rects);
        if (i + 1 < spans.size() && spans[i + 1].page == span.page + 1) {
            auto& next = spans[++i];
            if (auto more = rectsOf(next); !more.empty()) {
                position["nextPageRects"] = positionFromRects(next.page, more)["rects"];
                text = joinedText(text, qs(doc_->text(next.page).textOf(next.from, next.to)));
            }
        }
        data["annotationText"] = ss(text);
        data["annotationPosition"] = position;
        made.push_back(data);
    }
    clearTextSelection();
    hidePopup();
    if (made.size() == 1)
        emit annotationCreated(made[0]);
    else if (made.size() > 1)
        emit annotationsCreated(made);
}

void PdfView::createInk()
{
    if (ink_.size() < 2 || dragPage_ < 0)
        return;
    Json path = Json::array();
    for (auto& pt : ink_) {
        auto pp = doc_->pageToPdf(dragPage_, pdf::Point{(float)pt.x(), (float)pt.y()});
        path.push_back(r3(pp.x));
        path.push_back(r3(pp.y));
    }
    // strokes drawn in quick succession on the same page belong to one annotation
    if (auto* last = findAnn(lastInkKey_);
        last && last->page == dragPage_ && lastInkTime_.isValid() && lastInkTime_.elapsed() < 2500 &&
        last->color == QColor(color_)) {
        Json data = last->data;
        data["annotationPosition"]["paths"].push_back(path);
        lastInkTime_.restart();
        emit annotationChanged(data);
        return;
    }
    auto bounds = ink_.boundingRect();
    auto data = baseAnnotation("ink", dragPage_, 0, bounds.top());
    data["annotationPosition"] = Json{{"pageIndex", dragPage_}, {"width", 2}, {"paths", Json::array({path})}};
    lastInkKey_ = qs(jstr(data, "key"));
    lastInkTime_.restart();
    emit annotationCreated(data);
}

void PdfView::eraseAt(int page, QPointF pt)
{
    double radius = 6;
    for (auto& a : anns_) {
        if (a.page != page || a.type != "ink" || a.external)
            continue;
        auto& paths = erasedPaths_.count(a.key) ? erasedPaths_[a.key] : (erasedPaths_[a.key] = a.paths);
        std::vector<QPolygonF> kept;
        bool changed = false;
        for (auto& path : paths) {
            QPolygonF cur;
            for (auto& p : path) {
                if (std::hypot(p.x() - pt.x(), p.y() - pt.y()) < radius + a.width) {
                    changed = true;
                    if (cur.size() > 1)
                        kept.push_back(cur);
                    cur.clear();
                } else {
                    cur << p;
                }
            }
            if (cur.size() > 1)
                kept.push_back(cur);
        }
        if (changed) {
            paths = kept;
            erased_.insert(a.key);
        } else if (!erased_.count(a.key)) {
            erasedPaths_.erase(a.key);
        }
    }
    viewport()->update();
}

bool PdfView::resizable(const AnnView& a) const
{
    return (a.type == "image" || a.type == "text") && !a.external && editable_;
}

// Which edges of a box are under the pointer (a corner is two edges); 0 when the
// pointer is inside the box or away from it.
int PdfView::edgesAt(const AnnView& a, QPointF pos) const
{
    if (!resizable(a))
        return 0;
    constexpr double t = 6;  // px around the line
    QRectF v = toView(a.page, a.bounds);
    if (!v.adjusted(-t, -t, t, t).contains(pos))
        return 0;
    double dl = std::abs(pos.x() - v.left()), dr = std::abs(pos.x() - v.right());
    double dt = std::abs(pos.y() - v.top()), db = std::abs(pos.y() - v.bottom());
    int edges = 0;
    if (std::min(dl, dr) <= t)
        edges |= dl <= dr ? EdgeLeft : EdgeRight;
    if (std::min(dt, db) <= t)
        edges |= dt <= db ? EdgeTop : EdgeBottom;
    return edges;
}

// The box whose edge is under the pointer: the selected annotation's first, then area
// annotations (whose frame is visible) from the top.
const PdfView::AnnView* PdfView::edgeAt(int page, QPointF pos, int* edges) const
{
    if (auto* a = const_cast<PdfView*>(this)->findAnn(selectedAnn_); a && a->page == page)
        if ((*edges = edgesAt(*a, pos)))
            return a;
    for (auto it = anns_.rbegin(); it != anns_.rend(); ++it)
        if (it->page == page && it->type == "image")
            if ((*edges = edgesAt(*it, pos)))
                return &*it;
    *edges = 0;
    return nullptr;
}

QRectF PdfView::resizedBounds(const AnnView& a) const
{
    QRectF r = a.bounds;
    QPointF d = dragNow_ - dragStart_;
    // the dragged edges move; they never cross the opposite ones
    double minW = a.type == "text" ? 20 : 4, minH = a.type == "text" ? 10 : 4;
    if (resizeEdges_ & EdgeLeft)
        r.setLeft(std::min(r.left() + d.x(), r.right() - minW));
    if (resizeEdges_ & EdgeRight)
        r.setRight(std::max(r.right() + d.x(), r.left() + minW));
    if (resizeEdges_ & EdgeTop)
        r.setTop(std::min(r.top() + d.y(), r.bottom() - minH));
    if (resizeEdges_ & EdgeBottom)
        r.setBottom(std::max(r.bottom() + d.y(), r.top() + minH));
    return r;
}

// A tool's cursor: its icon in the toolbar, the hot spot where it acts (the marker's tip,
// the start of the underline and of the pen's line, the note's middle, the corner of the
// text box and of the area, the eraser's edge).
static QCursor toolCursor(ReaderTool tool, qreal dpr)
{
    switch (tool) {
    case ReaderTool::Highlight:
        return iconCursor("tool-highlight", {3, 21}, dpr);
    case ReaderTool::Underline:
        return iconCursor("tool-underline", {5, 20}, dpr);
    case ReaderTool::Note:
        return iconCursor("tool-note", {12, 12}, dpr);
    case ReaderTool::Text:
        return iconCursor("tool-text", {5, 5}, dpr);
    case ReaderTool::Area:
        return iconCursor("tool-area", {4, 4}, dpr);
    case ReaderTool::Ink:
        return iconCursor("tool-ink", {3, 19}, dpr);
    case ReaderTool::Eraser:
        return iconCursor("tool-eraser", {5, 19}, dpr);
    default:
        return Qt::ArrowCursor;
    }
}

static Qt::CursorShape resizeCursor(int edges)
{
    bool h = edges & (1 | 4), v = edges & (2 | 8);
    if (h && v)
        return ((edges & 1) != 0) == ((edges & 2) != 0) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    return h ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}

Json PdfView::movedData(const AnnView& a, QPointF delta, const QRectF& resized) const
{
    Json data = a.data;
    auto& pos = data["annotationPosition"];
    if (a.type == "ink") {
        Json paths = Json::array();
        for (auto& path : a.paths) {
            Json arr = Json::array();
            for (auto& pt : path) {
                auto pp = doc_->pageToPdf(a.page, pdf::Point{float(pt.x() + delta.x()), float(pt.y() + delta.y())});
                arr.push_back(r3(pp.x));
                arr.push_back(r3(pp.y));
            }
            paths.push_back(arr);
        }
        pos["paths"] = paths;
    } else {
        QRectF r = a.type == "note" ? a.bounds : (a.rects.empty() ? a.bounds : a.rects[0]);
        r = resized.isValid() ? resized : r.translated(delta);
        auto rects = positionFromRects(a.page, {r})["rects"];
        pos["rects"] = rects;
    }
    double top = resized.isValid() ? resized.top() : a.bounds.top() + delta.y();
    data["annotationSortIndex"] = pdf::sortIndex(a.page, 0, (float)top);
    return data;
}

// Saves a move or resize that has just ended (drag_ is already reset).
void PdfView::commitMoved(Drag drag)
{
    auto* a = findAnn(selectedAnn_);
    if (!a)
        return;
    if (drag == Drag::Resizing) {
        QRectF r = resizedBounds(*a);
        QRectF b = a->bounds;
        if (std::max({std::abs(r.left() - b.left()), std::abs(r.top() - b.top()), std::abs(r.right() - b.right()),
                      std::abs(r.bottom() - b.bottom())}) >= 0.5)
            emit annotationChanged(movedData(*a, {}, r));
        return;
    }
    QPointF delta = dragNow_ - dragStart_;
    if (std::hypot(delta.x(), delta.y()) >= 0.5)
        emit annotationChanged(movedData(*a, delta));
}

void PdfView::deleteSelectedAnnotation()
{
    auto* a = findAnn(selectedAnn_);
    if (!a || a->external || !editable_)
        return;
    auto key = selectedAnn_;
    selectedAnn_.clear();
    emit annotationDeleted(key);
}

void PdfView::setAnnotationColor(const QString& key, const QString& color)
{
    auto* a = findAnn(key);
    if (!a || a->external || !editable_)
        return;
    Json data = a->data;
    data["annotationColor"] = ss(color);
    emit annotationChanged(data);
}

QString PdfView::commentKeyAt(QPointF viewPos) const
{
    if (pageRects_.empty() || !viewport()->rect().contains(viewPos.toPoint()))
        return {};
    const AnnView* a = commentBadgeAt(viewPos);
    if (!a) {
        QPointF content = viewPos + scrollOffset();
        int page = pageAt(content, false);
        if (page >= 0)
            a = annotationAt(page, toPage(page, content));
    }
    // a text box shows its text on the page already
    return a && !a->comment.isEmpty() && a->type != "text" ? a->key : QString();
}

void PdfView::showComment(const QString& key, bool edit)
{
    auto* a = findAnn(key);
    if (!a || a->type == "text")
        return;
    if (a->external || !editable_)
        edit = false;
    if (!edit && a->comment.isEmpty())
        return;
    if (bubble_->isVisible() && bubble_->key() != key)
        bubble_->finish(true);
    QRectF anchor = toView(a->page, a->bounds);
    hoverTimer_.stop();
    unhoverTimer_.stop();
    if (edit)
        bubble_->edit(key, a->comment, anchor);
    else
        bubble_->preview(key, a->comment, anchor);
}

void PdfView::finishComment() { bubble_->finish(true); }

void PdfView::placeComment()
{
    if (!bubble_->isVisible())
        return;
    auto* a = findAnn(bubble_->key());
    if (!a) {
        bubble_->finish(false);  // deleted meanwhile
        return;
    }
    QRectF anchor = toView(a->page, a->bounds);
    if (!bubble_->isEditing() && !anchor.intersects(viewport()->rect()))
        bubble_->finish(false);
    else
        bubble_->place(anchor);
}

void PdfView::editTextAnnotation(const QString& key)
{
    auto* a = findAnn(key);
    if (!a || a->type != "text" || a->external || !editable_)
        return;
    closeTextEditor(true);
    editingKey_ = key;
    textEditor_ = new TextBoxEdit(viewport());
    textEditor_->setPlainText(a->comment);
    QFont f = font();
    f.setPixelSize(std::max(6, int(a->fontSize * scale_)));
    textEditor_->setFont(f);
    textEditor_->setFrameShape(QFrame::Box);
    textEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    textEditor_->setStyleSheet(QString("QPlainTextEdit { color: %1; background: rgba(255,255,255,200); border: 1px "
                                       "dashed %1; }")
                                   .arg(a->color.name()));
    textEditor_->document()->setDocumentMargin(0);
    textEditor_->setGeometry(toView(a->page, a->rects.empty() ? a->bounds : a->rects[0]).toRect());
    textEditor_->show();
    textEditor_->setFocus();
    textEditor_->installEventFilter(this);
    connect(textEditor_, &QPlainTextEdit::textChanged, this, [this] {
        // grow the box while typing
        auto* a = findAnn(editingKey_);
        if (!a || !textEditor_)
            return;
        int needed = int(textEditor_->document()->size().height() * QFontMetrics(textEditor_->font()).lineSpacing()) + 6;
        if (needed > textEditor_->height())
            textEditor_->resize(textEditor_->width(), needed);
    });
    viewport()->update();
}

void PdfView::closeTextEditor(bool commit)
{
    if (!textEditor_)
        return;
    auto* editor = textEditor_;
    textEditor_ = nullptr;
    auto key = editingKey_;
    editingKey_.clear();
    QString text = editor->toPlainText();
    QSize size = editor->size();
    editor->deleteLater();
    if (!commit)
        return;
    auto* a = findAnn(key);
    if (!a)
        return;
    if (text.trimmed().isEmpty() && a->comment.isEmpty()) {
        emit annotationDeleted(key);
        return;
    }
    Json data = a->data;
    data["annotationComment"] = ss(text);
    // keep the box tall enough for the text
    QRectF r = a->rects.empty() ? a->bounds : a->rects[0];
    double h = size.height() / scale_;
    if (h > r.height() + 1) {
        r.setHeight(h);
        data["annotationPosition"]["rects"] = positionFromRects(a->page, {r})["rects"];
    }
    if (text != a->comment || h > (a->rects.empty() ? 0 : a->rects[0].height()) + 1)
        emit annotationChanged(data);
    viewport()->update();
}

// --- selection --------------------------------------------------------------------------------

std::vector<PdfView::SelSpan> PdfView::selectionSpans() const
{
    if (!hasTextSelection())
        return {};
    auto first = std::make_pair(selAnchorPage_, selAnchor_), last = std::make_pair(selFocusPage_, selFocus_);
    if (last < first)
        std::swap(first, last);
    std::vector<SelSpan> spans;
    for (int page = first.first; page <= last.first; ++page) {
        int count = (int)doc_->text(page).chars.size();
        int from = page == first.first ? first.second : 0, to = page == last.first ? last.second : count - 1;
        if (count > 0 && from <= to)
            spans.push_back({page, from, to});
    }
    return spans;
}

QString PdfView::joinedText(QString before, const QString& after)
{
    if (before.endsWith('-') && !after.isEmpty() && after[0].isLower())
        before.chop(1);  // (a word broken at the foot of the page)
    else if (!before.isEmpty() && !after.isEmpty())
        before += ' ';
    return before + after;
}

QString PdfView::selectedText() const
{
    QString out;
    for (auto& span : selectionSpans())
        out = joinedText(out, qs(doc_->text(span.page).textOf(span.from, span.to)));
    return out;
}

PdfQuote PdfView::selectedQuote() const
{
    PdfQuote quote;
    auto spans = selectionSpans();
    if (spans.empty())
        return quote;
    quote.text = selectedText();
    quote.page = spans[0].page;  // (where it begins)
    for (auto& r : doc_->text(spans[0].page).rectsOf(spans[0].from, spans[0].to))
        quote.place = quote.place.isNull() ? toQ(r) : quote.place.united(toQ(r));
    return quote;
}

void PdfView::showPlace(int page, const QRectF& place)
{
    if (page < 0 || page >= pageCount())
        return;
    remember();  // (Ctrl+Left: back to where it was)
    goToPage(page, place.isNull() ? -1 : place.top());
    if (place.isNull())
        return;
    placePage_ = page;
    place_ = place;
    placeFade_->stop();
    placeFade_->start();
}

PdfView::Place PdfView::place() const
{
    if (pageRects_.empty())
        return {};
    auto corner = scrollOffset();
    int page = pageAt(corner, true);
    return {page, toPage(page, corner)};
}

void PdfView::goTo(const Place& place)
{
    if (place.page < 0 || place.page >= (int)pageRects_.size())
        return;
    QPointF corner = pageRects_[place.page].topLeft() + place.at * scale_;  // (at this zoom)
    horizontalScrollBar()->setValue(int(std::lround(corner.x())));
    verticalScrollBar()->setValue(int(std::lround(corner.y())));
}

void PdfView::remember()
{
    if (pageRects_.empty())
        return;
    back_.push_back(place());
    if (back_.size() > 200)
        back_.erase(back_.begin());
    forward_.clear();  // (a new way from here: what was forward of it is gone)
}

void PdfView::followLink(int page, double y)
{
    remember();
    goToPage(page, std::isfinite(y) ? y : -1);
}

void PdfView::goBack()
{
    if (back_.empty())
        return;
    forward_.push_back(place());
    auto to = back_.back();
    back_.pop_back();
    goTo(to);
}

void PdfView::goForward()
{
    if (forward_.empty())
        return;
    back_.push_back(place());
    auto to = forward_.back();
    forward_.pop_back();
    goTo(to);
}

QWidget* PdfView::linkPreview() const { return linkPreview_; }

const std::vector<QRectF>& PdfView::pictureRects(int page)
{
    auto it = pictureRects_.find(page);
    if (it != pictureRects_.end())
        return it->second;
    std::vector<QRectF> rects;
    try {
        double area = pageSizes_[page].width() * pageSizes_[page].height();
        for (auto& r : doc_->imageRects(page))
            if (double(r.width()) * r.height() <= 0.75 * area)
                rects.push_back(toQ(r));
    } catch (const std::exception&) {
        // (none known: the page colour over all of it)
    }
    return pictureRects_[page] = std::move(rects);
}

std::pair<int, int> PdfView::pageLinkAt(QPointF pos)
{
    if (pageRects_.empty() || tool_ != ReaderTool::Select || pos.x() < 0)
        return {-1, -1};
    QPointF content = pos + scrollOffset();
    int page = pageAt(content, false);
    if (page < 0)
        return {-1, -1};
    QPointF pt = toPage(page, content);
    if (!links_.count(page))
        links_[page] = doc_->links(page);
    auto& links = links_[page];
    for (int i = 0; i < (int)links.size(); ++i)
        if (links[i].page >= 0 && toQ(links[i].rect).contains(pt))
            return {page, i};
    return {-1, -1};
}

void PdfView::hoverLink(int page, int index)
{
    if (page == previewPage_ && index == previewIndex_) {
        previewLeaveTimer_.stop();  // (back on it)
        return;
    }
    if (page < 0 && linkPreview_->isVisible()) {  // off the link: a moment to get into the preview
        previewLeaveTimer_.start();
        return;
    }
    hideLinkPreview();
    previewPage_ = page;
    previewIndex_ = index;
    if (page >= 0)
        linkPreviewTimer_.start();
}

void PdfView::hideLinkPreview()
{
    linkPreviewTimer_.stop();
    previewLeaveTimer_.stop();
    linkPreview_->hide();
    previewPage_ = previewIndex_ = -1;
}

QRectF PdfView::linkPreviewArea() const { return linkPreview_->area(); }

QImage PdfView::drawnPart(int page, const QRectF& area, double scale)
{
    double dpr = devicePixelRatioF();
    QImage image;
    try {
        auto img = doc_->renderClip(page, float(scale * dpr), toP(area));
        image = QImage(img.pixels.data(), img.width, img.height, img.stride, QImage::Format_RGB888).copy();
    } catch (const std::exception&) {
        image = QImage(QSize(1, 1), QImage::Format_RGB888);
        image.fill(Qt::white);
    }
    if (darkPages_) {  // as the page is drawn
        std::vector<pdf::Rect> keep;
        for (auto r : doc_->imageRects(page))
            keep.push_back({float(r.x0 - area.left()), float(r.y0 - area.top()), float(r.x1 - area.left()),
                            float(r.y1 - area.top())});
        darkenPage(image, keep, scale * dpr, doc_->pageSize(page));
    }
    if (tone_ > 0) {  // (as the page is drawn, see paintEvent)
        QPainter p(&image);
        QRegion paper(image.rect());
        for (auto& picture : pictureRects(page))
            paper -= QRegion(QRectF((picture.topLeft() - area.topLeft()) * scale * dpr, picture.size() * scale * dpr)
                                 .toAlignedRect());
        p.setClipRegion(paper);
        p.setCompositionMode(darkPages_ ? QPainter::CompositionMode_Lighten : QPainter::CompositionMode_Darken);
        p.fillRect(image.rect(), pageToneColor(darkPages_, tone_));
    }
    return image;
}

void PdfView::showLinkPreview()
{
    if (previewPage_ < 0 || !links_.count(previewPage_) || previewIndex_ >= (int)links_[previewPage_].size())
        return;
    auto link = links_[previewPage_][previewIndex_];
    int target = link.page;
    if (target < 0 || target >= pageCount())
        return;
    QSizeF size = pageSizes_[target];
    double x = std::isfinite(link.x) ? link.x : 0, y = std::isfinite(link.y) ? std::max<double>(0, link.y) : 0;
    // as wide as the text there: the line at the place (nearest it across), and the lines
    // under it beside that one (a reference in one column of two: that column)
    double left = 0, right = size.width();
    auto& text = doc_->text(target);
    const pdf::TextLine* first = nullptr;
    double best = 1e9;
    for (auto& line : text.lines) {
        if (line.vertical || line.box.y1 < y - 2 || line.box.y0 > y + 40)
            continue;
        double across = x < line.box.x0 ? line.box.x0 - x : x > line.box.x1 ? x - line.box.x1 : 0;
        double score = 2 * across + std::max(0.0, line.box.y0 - y);
        if (score < best) {
            best = score;
            first = &line;
        }
    }
    if (first) {
        left = first->box.x0;
        right = first->box.x1;
        for (int pass = 0; pass < 2; ++pass)
            for (auto& line : text.lines)
                if (!line.vertical && line.box.y1 >= y && line.box.y0 <= y + 200 && line.box.x0 < right &&
                    line.box.x1 > left) {
                    left = std::min<double>(left, line.box.x0);
                    right = std::max<double>(right, line.box.x1);
                }
        left = std::max(0.0, left - 8);
        right = std::min(size.width(), right + 8);
    }
    // at the page's zoom, unless that is wider than a preview
    double widest = std::clamp(viewport()->width() * 0.6, 260.0, 600.0);
    double scale = std::min(scale_, widest / std::max(1.0, right - left));
    double top = std::clamp(y - 8, 0.0, std::max(0.0, size.height() - 40));
    double height = std::min(size.height() - top, std::clamp(viewport()->height() * 0.3, 120.0, 240.0) / scale);
    linkPreview_->pages = pageCount();
    linkPreview_->start(target, {left, top}, QSize(int(std::ceil((right - left) * scale)), int(std::ceil(height * scale))), scale);
    // under the link (over it, near the bottom), from the mouse on, inside the view
    QRectF at = toView(previewPage_, toQ(link.rect));
    int w = linkPreview_->width(), h = linkPreview_->height();
    int px = std::clamp(int(mousePos_.x()) - 24, 4, std::max(4, viewport()->width() - w - 4));
    int py = int(at.bottom()) + 6;
    if (py + h > viewport()->height() - 4)
        py = std::max(4, int(at.top()) - 6 - h);
    linkPreview_->move(px, py);
    linkPreview_->show();
    linkPreview_->raise();
}

void PdfView::clearTextSelection()
{
    selAnchorPage_ = selAnchor_ = selFocusPage_ = selFocus_ = -1;
    viewport()->update();
}

void PdfView::copySelection()
{
    if (hasTextSelection()) {
        QApplication::clipboard()->setText(selectedText());
    } else if (auto* a = findAnn(selectedAnn_)) {
        auto text = jqs(a->data, "annotationText");
        QApplication::clipboard()->setText(text.isEmpty() ? a->comment : text);
    }
}

void PdfView::showPopup()
{
    auto spans = selectionSpans();
    if (spans.empty() || !editable_)
        return;
    auto firstRects = doc_->text(spans.front().page).rectsOf(spans.front().from, spans.front().to);
    auto lastRects = doc_->text(spans.back().page).rectsOf(spans.back().from, spans.back().to);
    if (firstRects.empty() || lastRects.empty())
        return;
    QRectF last = toView(spans.back().page, toQ(lastRects.back()));
    popup_->adjustSize();
    QPoint pos(int(last.center().x() - popup_->width() / 2), int(last.bottom() + 6));
    pos.setX(std::clamp(pos.x(), 2, std::max(2, viewport()->width() - popup_->width() - 2)));
    if (pos.y() + popup_->height() > viewport()->height())
        pos.setY(int(toView(spans.front().page, toQ(firstRects.front())).top() - popup_->height() - 6));
    popup_->move(pos);
    popup_->show();
    popup_->raise();
}

void PdfView::hidePopup() { popup_->hide(); }

// --- mouse --------------------------------------------------------------------------------------

void PdfView::setTool(ReaderTool tool)
{
    tool_ = tool;
    if (tool != ReaderTool::Select && tool != ReaderTool::Highlight && tool != ReaderTool::Underline)
        clearTextSelection();
    hidePopup();
    updateCursor(viewport()->mapFromGlobal(QCursor::pos()));
}

void PdfView::setColor(const QString& color) { color_ = color; }

void PdfView::setEditable(bool editable)
{
    editable_ = editable;
    if (!editable)
        setTool(ReaderTool::Select);
}

void PdfView::setNotesAvailable(bool available)
{
    notesAvailable_ = available;
    popup_->note_->setVisible(available);
}

void PdfView::updateCursor(QPointF pos)
{
    if (pageRects_.empty())
        return;
    if (commentBadgeAt(pos)) {
        viewport()->setCursor(Qt::PointingHandCursor);
        return;
    }
    QPointF content = pos + scrollOffset();
    int page = pageAt(content, false);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (page >= 0) {
        QPointF pt = toPage(page, content);
        switch (tool_) {
        case ReaderTool::Select: {
            int edges = 0;
            if (edgeAt(page, pos, &edges)) {
                shape = resizeCursor(edges);
                break;
            }
            if (auto* a = annotationAt(page, pt); a && a->type != "highlight" && a->type != "underline") {
                shape = a->external || !editable_ ? Qt::PointingHandCursor : Qt::SizeAllCursor;
                break;
            }
            if (!links_.count(page))
                links_[page] = doc_->links(page);
            for (auto& l : links_[page])
                if (toQ(l.rect).contains(pt)) {
                    shape = Qt::PointingHandCursor;
                    break;
                }
            if (shape == Qt::ArrowCursor && doc_->text(page).hitTest({(float)pt.x(), (float)pt.y()}, false) >= 0)
                shape = Qt::IBeamCursor;
            break;
        }
        default:
            viewport()->setCursor(toolCursor(tool_, viewport()->devicePixelRatioF()));
            return;
        }
    }
    viewport()->setCursor(shape);
}

void PdfView::mousePressEvent(QMouseEvent* e)
{
    hideLinkPreview();
    if (e->button() == Qt::BackButton || e->button() == Qt::ForwardButton) {  // (a mouse's side buttons)
        e->button() == Qt::BackButton ? goBack() : goForward();
        return;
    }
    if (e->button() != Qt::LeftButton || pageRects_.empty()) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    setFocus();
    hidePopup();
    closeTextEditor(true);
    bubble_->finish(true);
    hoverTimer_.stop();
    pressedAnn_.clear();
    pressPos_ = e->position();
    if (auto* a = commentBadgeAt(e->position())) {
        // with any tool, the badge selects its annotation (and its comment opens)
        QString key = a->key;
        pressedAnn_ = key;
        selectedAnn_ = key;
        clearTextSelection();
        emit annotationSelected(key);
        viewport()->update();
        return;
    }
    QPointF content = e->position() + scrollOffset();
    int page = pageAt(content, false);
    if (page < 0) {
        clearTextSelection();
        selectedAnn_.clear();
        viewport()->update();
        return;
    }
    QPointF pt = toPage(page, content);
    dragPage_ = page;
    dragStart_ = dragNow_ = pt;
    bool triple = tripleClick_.isValid() && tripleClick_.elapsed() < QApplication::styleHints()->mouseDoubleClickInterval();
    tripleClick_.invalidate();

    switch (tool_) {
    case ReaderTool::Select:
    case ReaderTool::Highlight:
    case ReaderTool::Underline: {
        if (tool_ == ReaderTool::Select) {
            int edges = 0;
            if (auto* a = edgeAt(page, e->position(), &edges)) {
                // an edge or corner of a box: resize from there
                selectedAnn_ = a->key;
                clearTextSelection();
                emit annotationSelected(a->key);
                drag_ = Drag::Resizing;
                resizeEdges_ = edges;
                viewport()->update();
                return;
            }
            if (auto* a = annotationAt(page, pt)) {
                // the first click selects it (Delete deletes it); a click on it selected (not a
                // drag) edits its comment
                pressedAnn_ = selectedAnn_ == a->key ? a->key : QString();
                selectedAnn_ = a->key;
                clearTextSelection();
                emit annotationSelected(a->key);
                if (!a->external && editable_ && a->type != "highlight" && a->type != "underline")
                    drag_ = Drag::Moving;
                viewport()->update();
                return;
            }
            if (!links_.count(page))
                links_[page] = doc_->links(page);
            pressedLink_.clear();
            for (auto& l : links_[page])
                if (toQ(l.rect).contains(pt)) {
                    pressedLink_ = qs(l.uri);
                    pressedLinkPage_ = l.page;
                    pressedLinkY_ = l.y;
                    break;
                }
            if (!selectedAnn_.isEmpty()) {
                selectedAnn_.clear();
                emit annotationSelected({});
            }
        }
        auto& text = doc_->text(page);
        int idx = text.hitTest({(float)pt.x(), (float)pt.y()}, false);
        if (triple && selAnchorPage_ == page && idx >= 0) {
            auto [a, b] = text.lineAt(idx);
            selAnchorPage_ = selFocusPage_ = page;
            selAnchor_ = a;
            selFocus_ = b;
            viewport()->update();
            return;
        }
        clearTextSelection();
        // off the text (a margin, between columns) a drag still selects, from the
        // nearest character, with every tool
        if (idx < 0)
            idx = text.hitTest({(float)pt.x(), (float)pt.y()}, true);
        if (idx >= 0) {
            selAnchorPage_ = page;
            selAnchor_ = idx;
            selFocusPage_ = selFocus_ = -1;  // nothing selected until the mouse moves
            drag_ = Drag::Selecting;
        }
        viewport()->update();
        return;
    }
    case ReaderTool::Note: {
        if (!editable_)
            return;
        QRectF r(pt - QPointF(kNoteSize / 2, kNoteSize / 2), QSizeF(kNoteSize, kNoteSize));
        int offset = doc_->text(page).hitTest({(float)pt.x(), (float)pt.y()}, true);
        auto data = baseAnnotation("note", page, std::max(0, offset), r.top());
        data["annotationPosition"] = positionFromRects(page, {r});
        emit annotationCreated(data);
        emit annotationSelected(qs(jstr(data, "key")));
        selectedAnn_ = qs(jstr(data, "key"));
        return;
    }
    case ReaderTool::Text: {
        if (!editable_)
            return;
        double fontSize = 14;
        QRectF r(pt, QSizeF(220, fontSize * 1.6));
        int offset = doc_->text(page).hitTest({(float)pt.x(), (float)pt.y()}, true);
        auto data = baseAnnotation("text", page, std::max(0, offset), r.top());
        auto pos = positionFromRects(page, {r});
        pos["fontSize"] = fontSize;
        pos["rotation"] = 0;
        data["annotationPosition"] = pos;
        emit annotationCreated(data);
        editTextAnnotation(qs(jstr(data, "key")));
        return;
    }
    case ReaderTool::Area:
        if (editable_)
            drag_ = Drag::Area;
        return;
    case ReaderTool::Ink:
        if (editable_) {
            drag_ = Drag::Ink;
            ink_.clear();
            ink_ << pt;
        }
        return;
    case ReaderTool::Eraser:
        if (editable_) {
            drag_ = Drag::Erasing;
            erased_.clear();
            erasedPaths_.clear();
            eraseAt(page, pt);
        }
        return;
    }
}

void PdfView::mouseMoveEvent(QMouseEvent* e)
{
    mousePos_ = e->position();
    if (drag_ == Drag::None && e->buttons() == Qt::NoButton && !bubble_->isEditing()) {
        // resting on an annotation shows its comment
        auto key = commentKeyAt(e->position());
        if (!key.isEmpty()) {
            unhoverTimer_.stop();
            if (!(bubble_->isVisible() && bubble_->key() == key) && hoverKey_ != key) {
                hoverKey_ = key;
                hoverTimer_.start();
            }
        } else {
            hoverKey_.clear();
            hoverTimer_.stop();
            if (bubble_->isVisible())
                unhoverTimer_.start();
        }
    }
    if (drag_ == Drag::None) {
        if (e->buttons() == Qt::NoButton) {
            auto [page, index] = pageLinkAt(e->position());
            hoverLink(page, index);
        }
        updateCursor(e->position());
        QAbstractScrollArea::mouseMoveEvent(e);
        return;
    }
    QPointF content = e->position() + scrollOffset();
    // autoscroll while dragging past the edges
    if (e->position().y() < 0)
        verticalScrollBar()->setValue(verticalScrollBar()->value() - 20);
    else if (e->position().y() > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + 20);
    QPointF pt = toPage(dragPage_, content);
    dragNow_ = pt;
    switch (drag_) {
    case Drag::Selecting: {
        // on to the page the mouse is over (a selection across pages): its character nearest
        // the mouse; on a page without text, the last (or first) of the page before (after) it
        int page = pageAt(content, true), idx = -1, step = page < selAnchorPage_ ? 1 : -1;
        for (; page >= 0 && page < pageCount(); page += step) {
            auto& text = doc_->text(page);
            if (page == pageAt(content, true)) {
                QPointF at = toPage(page, content);
                idx = text.hitTest({(float)at.x(), (float)at.y()}, true);
            } else if (!text.chars.empty()) {
                idx = step < 0 ? (int)text.chars.size() - 1 : 0;
            }
            if (idx >= 0 || page == selAnchorPage_)
                break;
        }
        if (idx >= 0 && (page != selAnchorPage_ || idx != selAnchor_ || selFocusPage_ >= 0 ||
                         std::hypot(pt.x() - dragStart_.x(), pt.y() - dragStart_.y()) > 2)) {
            selFocusPage_ = page;
            selFocus_ = idx;
        }
        break;
    }
    case Drag::Ink:
        if (ink_.isEmpty() || std::hypot(ink_.last().x() - pt.x(), ink_.last().y() - pt.y()) > 0.6)
            ink_ << pt;
        break;
    case Drag::Erasing:
        eraseAt(dragPage_, pt);
        break;
    default:
        break;
    }
    viewport()->update();
}

void PdfView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(e);
        return;
    }
    auto drag = drag_;
    drag_ = Drag::None;
    switch (drag) {
    case Drag::Selecting:
        if (hasTextSelection()) {
            if (tool_ == ReaderTool::Highlight)
                createFromSelection("highlight", color_);
            else if (tool_ == ReaderTool::Underline)
                createFromSelection("underline", color_);
            else
                showPopup();
        } else if (!pressedLink_.isEmpty()) {
            if (pressedLinkPage_ >= 0)
                followLink(pressedLinkPage_, pressedLinkY_);
            else if (pressedLink_.startsWith("http") || pressedLink_.startsWith("mailto:"))
                QDesktopServices::openUrl(QUrl(pressedLink_));
        }
        break;
    case Drag::Area: {
        QRectF area = QRectF(dragStart_, dragNow_).normalized();
        if (area.width() > 3 && area.height() > 3) {
            auto data = baseAnnotation("image", dragPage_, 0, area.top());
            data["annotationPosition"] = positionFromRects(dragPage_, {area});
            emit annotationCreated(data);
        }
        break;
    }
    case Drag::Ink:
        createInk();
        ink_.clear();
        break;
    case Drag::Erasing:
        for (auto& key : erased_) {
            auto* a = findAnn(key);
            if (!a)
                continue;
            auto& paths = erasedPaths_[key];
            if (paths.empty()) {
                emit annotationDeleted(key);
                continue;
            }
            AnnView copy = *a;
            copy.paths = paths;
            emit annotationChanged(movedData(copy, {}, {}));
        }
        erased_.clear();
        erasedPaths_.clear();
        break;
    case Drag::Moving:
    case Drag::Resizing:
        commitMoved(drag);
        break;
    case Drag::None:
        if (!pressedLink_.isEmpty() && tool_ == ReaderTool::Select) {
            if (pressedLinkPage_ >= 0)
                followLink(pressedLinkPage_, pressedLinkY_);
            else if (pressedLink_.startsWith("http") || pressedLink_.startsWith("mailto:"))
                QDesktopServices::openUrl(QUrl(pressedLink_));
        }
        break;
    }
    pressedLink_.clear();
    dragPage_ = -1;
    if (!pressedAnn_.isEmpty() && (e->position() - pressPos_).manhattanLength() < QApplication::startDragDistance())
        showComment(pressedAnn_, true);
    pressedAnn_.clear();
    viewport()->update();
}

void PdfView::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (bubble_->isEditing() && !bubble_->isChanged())
        bubble_->finish(false);  // opened by the first click of this double click
    if (e->button() != Qt::LeftButton || pageRects_.empty() || commentBadgeAt(e->position()))
        return;
    QPointF content = e->position() + scrollOffset();
    int page = pageAt(content, false);
    if (page < 0)
        return;
    QPointF pt = toPage(page, content);
    if (tool_ == ReaderTool::Select)
        if (auto* a = annotationAt(page, pt); a && a->type == "text") {
            editTextAnnotation(a->key);
            return;
        }
    if (tool_ != ReaderTool::Select && tool_ != ReaderTool::Highlight && tool_ != ReaderTool::Underline)
        return;
    auto& text = doc_->text(page);
    int idx = text.hitTest({(float)pt.x(), (float)pt.y()}, false);
    if (idx < 0)
        return;
    auto [a, b] = text.wordAt(idx);
    selAnchorPage_ = selFocusPage_ = page;
    selAnchor_ = a;
    selFocus_ = b;
    drag_ = Drag::None;
    tripleClick_.start();
    viewport()->update();
    if (tool_ == ReaderTool::Highlight)
        createFromSelection("highlight", color_);
    else if (tool_ == ReaderTool::Underline)
        createFromSelection("underline", color_);
    else
        showPopup();
}

void PdfView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        double steps = e->angleDelta().y() / 120.0;
        if (steps != 0)
            setZoom(zoom() * std::pow(1.1, steps), e->position());
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void PdfView::keyPressEvent(QKeyEvent* e)
{
    // (not Shift or Ctrl alone: held down for the link preview's wheel, across or zooming)
    static const std::set<int> modifiers{Qt::Key_Shift, Qt::Key_Control, Qt::Key_Meta, Qt::Key_Alt, Qt::Key_AltGr};
    if (!modifiers.count(e->key()))
        hideLinkPreview();
    // back to where a link was followed from, and forward again (a browser's Alt+Left/Right)
    if ((e->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier &&
        (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right)) {
        e->key() == Qt::Key_Left ? goBack() : goForward();
        return;
    }
    // 1 … 8: the tools, in the toolbar's order. Only the page itself takes them: in a
    // comment, a text box or any other field the digit is typed (and never gets here).
    if ((e->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier && e->key() >= Qt::Key_1 && e->key() <= Qt::Key_8) {
        static const ReaderTool tools[] = {ReaderTool::Select, ReaderTool::Highlight, ReaderTool::Underline,
                                           ReaderTool::Note,   ReaderTool::Text,      ReaderTool::Area,
                                           ReaderTool::Ink,    ReaderTool::Eraser};
        auto tool = tools[e->key() - Qt::Key_1];
        if (editable_ || tool == ReaderTool::Select)
            emit toolChangeRequested(tool);
        return;
    }
    if (e->modifiers() == Qt::NoModifier) {  // likewise only here
        switch (e->key()) {
        case Qt::Key_H:
            emit libraryRequested();
            return;
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            emit panelToggleRequested(e->key() == Qt::Key_BracketRight);
            return;
        case Qt::Key_I:
        case Qt::Key_N:
            emit contextTabRequested(e->key() == Qt::Key_N ? 1 : 0);
            return;
        case Qt::Key_L:
            emit darkPagesToggleRequested();
            return;
        case Qt::Key_Q:
        case Qt::Key_W:
        case Qt::Key_E:
            if (editable_)
                emit paletteColorRequested(e->key() == Qt::Key_Q ? 0 : e->key() == Qt::Key_W ? 1 : 2);
            return;
        default:
            break;
        }
    }
    // F: fit the width; F again within 10 s, the whole page, and so on. After a while
    // the first F is the width again.
    if (e->key() == Qt::Key_F && e->modifiers() == Qt::NoModifier) {
        bool again = fitKey_.isValid() && fitKey_.elapsed() < 10000;
        if (again && fit_ == Fit::Width)
            fitPage();
        else
            fitWidth();
        fitKey_.start();
        return;
    }
    if (e->matches(QKeySequence::Copy)) {
        copySelection();
        return;
    }
    switch (e->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelectedAnnotation();
        return;
    case Qt::Key_Escape:
        clearTextSelection();
        hidePopup();
        bubble_->finish(true);
        selectedAnn_.clear();
        if (tool_ != ReaderTool::Select)
            emit toolChangeRequested(ReaderTool::Select);
        viewport()->update();
        return;
    case Qt::Key_Home:
        verticalScrollBar()->setValue(0);
        return;
    case Qt::Key_End:
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        return;
    case Qt::Key_Space:
        // Space and Shift+Space page; with Ctrl, Alt or Meta it is no page key (Ctrl+Space
        // switches input methods: it turned the page when none was there to take it)
        if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            break;
        verticalScrollBar()->triggerAction(e->modifiers() & Qt::ShiftModifier ? QAbstractSlider::SliderPageStepSub
                                                                              : QAbstractSlider::SliderPageStepAdd);
        return;
    case Qt::Key_Left:
        if (horizontalScrollBar()->maximum() == 0) {
            goToPage(currentPage() - 1);
            return;
        }
        break;
    case Qt::Key_Right:
        if (horizontalScrollBar()->maximum() == 0) {
            goToPage(currentPage() + 1);
            return;
        }
        break;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

void PdfView::contextMenuEvent(QContextMenuEvent* e)
{
    QPointF content = QPointF(e->pos()) + scrollOffset();
    int page = pageAt(content, false);
    QMenu menu(this);
    if (page >= 0) {
        QPointF pt = toPage(page, content);
        if (auto* a = annotationAt(page, pt)) {
            selectedAnn_ = a->key;
            emit annotationSelected(a->key);
            viewport()->update();
            auto key = a->key;
            bool can = editable_ && !a->external;
            auto* colors = menu.addMenu(tr("Colour"));
            colors->setEnabled(can);
            for (auto& c : schema::kAnnotationColors)
                colors->addAction(colorIcon(c.hex), tr(c.name), this,
                                  [this, key, hex = QString(c.hex)] { setAnnotationColor(key, hex); });
            if (a->type == "text")
                menu.addAction(tr("Edit Text"), this, [this, key] { editTextAnnotation(key); })->setEnabled(can);
            menu.addAction(icon("copy"), tr("Copy"), this, &PdfView::copySelection);
            menu.addAction(icon("type-link"), tr("Copy Reference"), this, [this, key] { emit annotationReferenceCopied(key); });
            menu.addSeparator();
            menu.addAction(icon("trash"), tr("Delete"), this, &PdfView::deleteSelectedAnnotation)->setEnabled(can);
            menu.exec(e->globalPos());
            return;
        }
    }
    if (hasTextSelection()) {
        menu.addAction(icon("copy"), tr("Copy"), this, &PdfView::copySelection);
        menu.addAction(icon("type-link"), tr("Copy Reference"), this, [this] { emit referenceCopied(selectedQuote()); });
        menu.addAction(icon("tool-highlight"), tr("Highlight"), this, [this] { createFromSelection("highlight", color_); })
            ->setEnabled(editable_);
        menu.addAction(icon("tool-underline"), tr("Underline"), this, [this] { createFromSelection("underline", color_); })
            ->setEnabled(editable_);
        if (notesAvailable_)
            menu.addAction(icon("new-note"), tr("Add to Note"), this, [this] { emit quoteToNote(selectedQuote()); });
        menu.addSeparator();
    }
    auto* links = menu.addAction(tr("Highlight Links"), this, [](bool on) { setHighlightPdfLinks(on); });
    links->setCheckable(true);
    links->setChecked(highlightPdfLinks());
    if (canGoBack() || canGoForward()) {
        auto shortcut = [](Qt::Key key) { return "\t" + QKeySequence(Qt::CTRL | key).toString(QKeySequence::NativeText); };
        menu.addAction(icon("prev"), tr("Back") + shortcut(Qt::Key_Left), this, &PdfView::goBack)->setEnabled(canGoBack());
        menu.addAction(icon("next"), tr("Forward") + shortcut(Qt::Key_Right), this, &PdfView::goForward)
            ->setEnabled(canGoForward());
        menu.addSeparator();
    }
    menu.addAction(icon("zoom-in"), tr("Zoom In"), this, &PdfView::zoomIn);
    menu.addAction(icon("zoom-out"), tr("Zoom Out"), this, &PdfView::zoomOut);
    menu.addAction(icon("fit-width"), tr("Fit Width"), this, &PdfView::fitWidth);
    menu.addAction(icon("fit-page"), tr("Fit Page"), this, &PdfView::fitPage);
    menu.exec(e->globalPos());
}

bool PdfView::viewportEvent(QEvent* e)
{
    // a touchpad pinch on macOS and Wayland (X11 sends Ctrl+wheel instead, see wheelEvent)
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            setZoom(zoom() * (1 + g->value()), g->position());
            g->accept();
            return true;
        }
    }
    if (e->type() == QEvent::ToolTip && !pageRects_.empty()) {
        QToolTip::hideText();  // comments show in their bubble
        return true;
    }
    if (e->type() == QEvent::Leave) {
        hideLinkPreview();
        mousePos_ = {-1, -1};
        hoverKey_.clear();
        hoverTimer_.stop();
        if (bubble_->isVisible() && !bubble_->isEditing())
            unhoverTimer_.start();
    }
    return QAbstractScrollArea::viewportEvent(e);
}

bool PdfView::eventFilter(QObject* watched, QEvent* e)
{
    if (textEditor_ && watched == textEditor_) {
        if (e->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(e);
            bool ctrlEnter = (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
                             (ke->modifiers() & Qt::ControlModifier);
            if (ke->key() == Qt::Key_Escape || ctrlEnter) {
                QMetaObject::invokeMethod(this, [this] { closeTextEditor(true); }, Qt::QueuedConnection);
                setFocus();
                return true;
            }
        } else if (e->type() == QEvent::FocusOut) {
            QMetaObject::invokeMethod(this, [this] { closeTextEditor(true); }, Qt::QueuedConnection);
        }
    }
    return QAbstractScrollArea::eventFilter(watched, e);
}

// --- find --------------------------------------------------------------------------------------

void PdfView::setFindCaseSensitive(bool on)
{
    if (on == findCaseSensitive_)
        return;
    findCaseSensitive_ = on;
    findText_.clear();
}

void PdfView::find(const QString& text, bool backwards)
{
    if (!doc_ || text.trimmed().isEmpty()) {
        clearFind();
        return;
    }
    if (text != findText_) {
        findText_ = text;
        hits_.clear();
        hitIndex_ = -1;
        auto needle = ss(text);
        for (int i = 0; i < pageCount(); ++i)
            for (auto& hit : doc_->search(i, needle, 512, findCaseSensitive_)) {
                std::vector<QRectF> rects;
                for (auto& r : hit)
                    rects.push_back(toQ(r));
                hits_.push_back({i, rects});
            }
        // continue from the current page: the step below lands on the first hit at or
        // after it (forwards) or the last hit before it (backwards)
        int cur = currentPage();
        hitIndex_ = backwards ? 0 : -1;
        for (int h = 0; h < (int)hits_.size(); ++h)
            if (hits_[h].first >= cur) {
                hitIndex_ = backwards ? h : h - 1;
                break;
            }
    }
    if (hits_.empty()) {
        emit findStatus(0, 0);
        viewport()->update();
        return;
    }
    int n = (int)hits_.size();
    hitIndex_ = ((hitIndex_ + (backwards ? -1 : 1)) % n + n) % n;
    auto& hit = hits_[hitIndex_];
    QRectF v = toView(hit.first, hit.second.front());
    if (!viewport()->rect().adjusted(0, 40, 0, -40).contains(v.toRect()))
        goToPage(hit.first, hit.second.front().top());
    emit findStatus(hitIndex_ + 1, n);
    viewport()->update();
}

void PdfView::clearFind()
{
    findText_.clear();
    hits_.clear();
    hitIndex_ = -1;
    viewport()->update();
}

} // namespace on::gui
