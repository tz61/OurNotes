#include "gui/reader/comment_bubble.h"

#include <QApplication>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTextDocument>
#include <QVBoxLayout>

#include <cmath>
#include <memory>

#include "gui/note_editor.h"
#include "gui/note_html.h"
#include "gui/rich_text_images.h"

namespace on::gui {

namespace {
constexpr int kMaxWidth = 380;
constexpr int kMaxPreviewHeight = 300;
constexpr int kMaxEditHeight = 260;
} // namespace

CommentBubble::CommentBubble(QWidget* viewport) : QFrame(viewport)
{
    setObjectName("commentBubble");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(1, 1, 1, 1);
    text_ = new NoteTextEdit(this);
    text_->setFrameShape(QFrame::NoFrame);
    text_->setAcceptRichText(false);
    text_->setPlaceholderText(tr("Add a comment…  ($…$ for a formula)"));
    text_->document()->setDocumentMargin(6);
    v->addWidget(text_);
    text_->installEventFilter(this);
    text_->viewport()->installEventFilter(this);
    connect(text_->document(), &QTextDocument::contentsChanged, this, [this] {
        if (isVisible() && editing_)
            fit();
    });
    hide();
}

bool CommentBubble::isChanged() const { return editing_ && documentToComment(text_->document()) != original_; }

void CommentBubble::load(const QString& key, const QString& comment)
{
    key_ = key;
    original_ = comment;
    text_->setHtml(commentHtmlForEditor(comment));
    text_->prepareDocument();
    text_->document()->clearUndoRedoStacks();
}

void CommentBubble::preview(const QString& key, const QString& comment, const QRectF& anchor)
{
    if (editing_)
        return;
    load(key, comment);
    text_->setReadOnly(true);
    text_->setTextInteractionFlags(Qt::NoTextInteraction);
    text_->setFocusPolicy(Qt::NoFocus);  // a click on a comment only shown leaves the keys on the page
    text_->viewport()->setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Click to edit"));
    fit();
    place(anchor);
    show();
    raise();
}

void CommentBubble::edit(const QString& key, const QString& comment, const QRectF& anchor)
{
    if (editing_ && key == key_)
        return;
    load(key, comment);
    editing_ = true;
    text_->setReadOnly(false);
    text_->setTextInteractionFlags(Qt::TextEditorInteraction);
    text_->setFocusPolicy(Qt::StrongFocus);
    text_->viewport()->setCursor(Qt::IBeamCursor);
    setToolTip({});
    fit();
    place(anchor);
    show();
    raise();
    text_->setFocus(Qt::MouseFocusReason);
    text_->moveCursor(QTextCursor::End);
}

// As wide as its longest line (wrapped at kMaxWidth) and as tall as its text; while
// editing, the full width, growing with the text.
void CommentBubble::fit()
{
    auto* area = parentWidget();
    int maxWidth = std::max(120, std::min(kMaxWidth, area->width() - 16));
    int frame = 2 * (frameWidth() + 1);
    // measured on a copy: the editor keeps its document as wide as itself
    std::unique_ptr<QTextDocument> probe(text_->document()->clone());
    probe->setDefaultFont(text_->document()->defaultFont());
    probe->setDocumentMargin(text_->document()->documentMargin());
    FittedImageHandler::install(probe.get(), probe.get());
    probe->setTextWidth(maxWidth - frame);
    int width = maxWidth;
    if (!editing_)
        width = std::clamp(int(std::ceil(probe->idealWidth())) + frame + 1, 40, maxWidth);
    probe->setTextWidth(width - frame);
    int height = int(std::ceil(probe->size().height())) + frame;
    int most = std::min(editing_ ? kMaxEditHeight : kMaxPreviewHeight, area->height() - 16);
    text_->setVerticalScrollBarPolicy(height > most ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    resize(width, std::max(std::min(height, most), fontMetrics().height() + frame + 12));
    if (isVisible())
        place(anchor_);
}

void CommentBubble::place(const QRectF& anchor)
{
    anchor_ = anchor;
    QRect area = parentWidget()->rect().adjusted(6, 6, -6, -6);
    int x = std::clamp(int(anchor.left()), area.left(), std::max(area.left(), area.right() - width()));
    int below = int(std::ceil(anchor.bottom())) + 6, above = int(anchor.top()) - 6 - height();
    int y = below + height() <= area.bottom() || above < area.top() ? below : above;
    move(x, std::clamp(y, area.top(), std::max(area.top(), area.bottom() - height())));
}

void CommentBubble::finish(bool commit)
{
    if (key_.isEmpty() && !isVisible())
        return;
    QString key = key_;
    QString comment;
    bool changed = false;
    if (editing_) {
        text_->commitMathEdits(true);
        comment = documentToComment(text_->document());
        changed = comment != original_;
    }
    editing_ = false;
    key_.clear();
    hide();
    if (commit && changed)
        emit edited(key, comment);
}

bool CommentBubble::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == text_->viewport() && e->type() == QEvent::MouseButtonPress && !editing_ && !key_.isEmpty()) {
        emit editRequested(key_);
        return true;
    }
    if (watched == text_ && e->type() == QEvent::FocusOut && editing_) {
        auto reason = static_cast<QFocusEvent*>(e)->reason();
        // done when the user moves on, not for a menu, a formula dialog or another window
        if ((reason == Qt::MouseFocusReason || reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason) &&
            !QApplication::activeModalWidget())
            QMetaObject::invokeMethod(
                this,
                [this, key = key_] {
                    if (editing_ && key_ == key && !text_->hasFocus())
                        finish(true);
                },
                Qt::QueuedConnection);
    }
    return QFrame::eventFilter(watched, e);
}

void CommentBubble::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        finish(true);
        if (auto* view = parentWidget() ? parentWidget()->parentWidget() : nullptr)
            view->setFocus();  // back to the PDF
        return;
    }
    if (!editing_) {
        QFrame::keyPressEvent(e);  // a comment only shown: the keys are the page's
        return;
    }
    // what the comment being typed had no use for stays here: on the page, Ctrl+Space
    // turned it, and Ctrl+Shift+Backspace deleted the very annotation being commented
    e->accept();
}

void CommentBubble::leaveEvent(QEvent* e)
{
    QFrame::leaveEvent(e);
    if (!editing_)
        emit left();
}

} // namespace on::gui
