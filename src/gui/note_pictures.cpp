// Pictures in notes, as in Feishu or Slack: shown small unless made larger; a click on one
// picks it (an outline and four corners, no text cursor), with buttons above it; a click
// beside one (or on its left or right edge) puts the cursor there, where pictures pasted go.
// Pictures alone on a line are a row, all as high; making one larger or smaller makes the
// row so, as wide as the text at most. A caption is the paragraph under a line of pictures.
// (In a table's cell, one line when saved, pictures stay where they are put.)

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QDropEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>
#include <QToolButton>

#include <algorithm>
#include <optional>

#include "gui/icons.h"
#include "gui/note_editor.h"
#include "gui/note_html.h"
#include "gui/rich_text_images.h"

namespace on::gui {

namespace {

constexpr qreal kCorner = 8;               // a corner's square (taken a pixel larger, no more:
constexpr qreal kCornerHit = kCorner / 2 + 1;  // beside it the cursor goes there)
constexpr qreal kEdge = 10;                // a picture's left and right edge (at most a quarter
                                           // of it): a click there puts the cursor beside it
constexpr qreal kLeastHeight = 24;         // the smallest a picture is made
constexpr QSizeF kSmall = kDefaultPictureBox;  // the sizes of the buttons: a picture within
constexpr QSizeF kMedium(540, 360);           // it, a row as high
constexpr QSizeF kLarge(720, 480);

// The buttons over a picture, in groups with a line between them (on one row).
const std::vector<std::vector<const char*>>& barGroups()
{
    static const std::vector<std::vector<const char*>> groups = {
        {"pictureSmall", "pictureMedium", "pictureLarge", "pictureFit", "pictureOriginal"},
        {"pictureCaption"},
        {"pictureLeft", "pictureCenter", "pictureRight"},
        {"pictureJoin", "pictureOwnLine"},
        {"pictureOpen", "pictureDelete"},
    };
    return groups;
}

QTextImageFormat formatAt(QTextDocument* doc, int pos)
{
    QTextCursor c(doc);
    c.setPosition(pos);
    c.setPosition(pos + 1, QTextCursor::KeepAnchor);
    return c.charFormat().toImageFormat();
}

bool isPictureAt(QTextDocument* doc, int pos)
{
    return pos >= 0 && pos < doc->characterCount() - 1 && doc->characterAt(pos) == QChar::ObjectReplacementCharacter &&
           isPicture(formatAt(doc, pos));
}

// Whether a key types text, as an editor takes it (QInputControl::isAcceptableInput): what
// is printable (with Alt or AltGr too), a formatting mark, private use, an emoji's two
// halves; not a shortcut with Ctrl.
bool typesText(const QKeyEvent* e)
{
    if (e->text().isEmpty())
        return false;
    QChar c = e->text().at(0);
    if (c.category() == QChar::Other_Format)
        return true;
    auto mods = e->modifiers();  // (as Qt has them: Ctrl and the keypad's key types its text)
    if (mods == Qt::ControlModifier || mods == (Qt::ControlModifier | Qt::ShiftModifier))
        return false;
    return c.isPrint() || c.category() == QChar::Other_PrivateUse ||
           (c.isHighSurrogate() && e->text().size() > 1 && e->text().at(1).isLowSurrogate());
}

// Whether two blocks are in the same place: one cell of a table, or no table. Lines are
// joined only so: a selection from one cell into another, or out of a table, takes whole
// cells, or the table.
bool sameCell(const QTextBlock& a, const QTextBlock& b)
{
    if (!a.isValid() || !b.isValid())
        return false;
    QTextCursor ca(a), cb(b);
    if (ca.currentFrame() != cb.currentFrame())
        return false;
    auto* table = ca.currentTable();
    return !table || table->cellAt(ca) == table->cellAt(cb);
}

bool inTable(const QTextBlock& block) { return QTextCursor(block).currentTable() != nullptr; }

bool isCaption(const QTextBlock& block) { return block.isValid() && block.blockFormat().hasProperty(kCaptionProperty); }

// The caption under a line of pictures, if it has one.
QTextBlock captionOf(const QTextBlock& block)
{
    auto next = block.next();
    return isCaption(next) && sameCell(block, next) ? next : QTextBlock();
}

// The line of pictures above one (past its caption), if there is one.
QTextBlock rowAbove(const QTextBlock& block)
{
    auto prev = block.previous();
    if (isCaption(prev) && sameCell(prev, block))
        prev = prev.previous();
    return sameCell(prev, block) && !pictureRow(prev).empty() ? prev : QTextBlock();
}

// As wide for its height as the picture is (not as its size, rounded, was).
qreal aspectOf(QTextDocument* doc, const QTextImageFormat& f)
{
    auto natural = naturalPictureSize(doc, f);
    auto size = natural.isEmpty() ? pictureSize(doc, f) : natural;
    return size.width() / std::max<qreal>(1, size.height());
}

// The format of a picture `height` high (one not here to measure as it was).
QTextImageFormat sized(QTextDocument* doc, QTextImageFormat f, qreal height)
{
    if (!pictureKnown(doc, f))
        return f;
    f.setWidth(std::round(height * aspectOf(doc, f)));
    f.setHeight(std::round(height));
    return f;
}

// The height of a line of pictures, by the first of them there is to measure; 0 if none
// (not downloaded yet: their height is not known).
qreal knownRowHeight(QTextDocument* doc, const QTextBlock& block)
{
    for (int p : pictureRow(block))
        if (auto f = formatAt(doc, p); pictureKnown(doc, f))
            return pictureSize(doc, f).height();
    return 0;
}

// The pictures of a row made as large as they are shown, and saved: all as high as the row.
// (One joining a row would else keep its own size, and be that again alone.)
void evenRow(QTextDocument* doc, const QTextBlock& block)
{
    auto row = pictureRow(block);
    if (row.size() < 2)
        return;
    qreal height = rowHeight(doc, block);
    QTextCursor c(doc);
    for (int p : row) {
        auto f = formatAt(doc, p);
        auto even = sized(doc, f, height);
        if (even == f)
            continue;
        c.setPosition(p);
        c.setPosition(p + 1, QTextCursor::KeepAnchor);
        c.setCharFormat(even);
    }
}

// The block a line of text after pictures gets: as theirs, but left and no caption.
QTextBlockFormat textAfter(QTextBlockFormat f)
{
    f.clearProperty(QTextFormat::BlockAlignment);
    f.clearProperty(kCaptionProperty);
    f.clearProperty(kHeadingMarkersProperty);
    f.setHeadingLevel(0);
    return f;
}

// An empty block gone, its neighbours as they were (one before it takes nothing of it); only
// within its cell of a table, or outside tables (the only line of a cell stays).
void removeEmptyBlock(QTextBlock block)
{
    if (!block.isValid() || block.length() > 1)
        return;
    QTextCursor c(block);
    if (sameCell(block.previous(), block)) {
        c.setPosition(block.position() - 1);
        c.setPosition(block.position(), QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else if (sameCell(block.next(), block)) {
        auto next = block.next().blockFormat();
        c.deleteChar();  // the next block into this one: as it was
        c.setBlockFormat(next);
    }
}

// Pictures put where `c` is, on a line of their own (a row): a line with text is split
// around them, an empty one is theirs, in a row they join it. `c` ends after the last.
void insertPictureLine(QTextCursor& c, const QList<QTextImageFormat>& pictures)
{
    auto block = c.block();
    if (inTable(block)) {  // (a cell is one line when saved: there, where the cursor is)
        for (auto& f : pictures)
            c.insertImage(f);
        return;
    }
    auto original = block.blockFormat();
    auto originalChars = block.charFormat();
    bool text = block.length() > 1 && pictureRow(block).empty();
    if (text && c.atBlockStart()) {
        c.insertBlock(original, originalChars);
        c.movePosition(QTextCursor::PreviousBlock);
        c.setBlockFormat(textAfter(original));
    } else if (text) {
        c.insertBlock(textAfter(original), QTextCharFormat());
    } else if (block.length() <= 1) {  // (a paragraph: no heading, no caption)
        auto f = textAfter(original);
        if (original.hasProperty(QTextFormat::BlockAlignment))
            f.setAlignment(original.alignment());
        c.setBlockFormat(f);
    }
    for (auto& f : pictures)
        c.insertImage(f);
    if (text && !c.atBlockEnd()) {
        int end = c.position();
        c.insertBlock(original, originalChars);
        c.setPosition(end);
    }
}

} // namespace

void NoteTextEdit::setUpPictures()
{
    caretWidth_ = cursorWidth();
    pictureBarTimer_.setSingleShot(true);
    pictureBarTimer_.setInterval(0);
    connect(&pictureBarTimer_, &QTimer::timeout, this, &NoteTextEdit::placePictureBar);
    auto later = [this] { pictureBarTimer_.start(); };
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, later);
    connect(document(), &QTextDocument::contentsChanged, this, later);
    // the cursor moved (beside a picture, say): none picked any more; an input method's
    // text composed at pictures no longer where it was moved to (written, dropped, undone)
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        if (!composeTo_.isNull() && textCursor().block() != composeTo_.block())
            endComposing();
        if (picking_)
            return;
        unpickPicture();
        updateCaret();
    });
    connect(this, &QTextEdit::selectionChanged, this, [this] {
        if (!picking_)
            updateCaret();
    });
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        if (!picking_)
            updateCaret();  // (its line a line of pictures now, or no more)
    });
    caretBlink_.setInterval(std::max(100, QApplication::cursorFlashTime() / 2));
    connect(&caretBlink_, &QTimer::timeout, this, [this] {
        caretShown_ = !caretShown_;
        viewport()->update(staleCaret().united(rowCaretRect()).adjusted(-2, -2, 2, 2));
    });

    pictureBar_ = new QFrame(viewport());
    pictureBar_->setObjectName("pictureBar");
    pictureBar_->setStyleSheet("QFrame#pictureBar { background: palette(base); border: 1px solid palette(mid); "
                               "border-radius: 6px; } QToolButton { padding: 2px 5px; }");
    pictureBar_->setCursor(Qt::ArrowCursor);
    auto button = [&](const QString& name, const QString& text, const QString& tip, const QString& iconName,
                      std::function<void()> act) {
        auto* b = new QToolButton(pictureBar_);
        b->setObjectName(name);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);  // the note keeps the keys (and the picture picked)
        b->setToolTip(tip);
        if (iconName.isEmpty())
            b->setText(text);
        else
            b->setIcon(icon(iconName));
        connect(b, &QToolButton::clicked, this, std::move(act));
    };
    for (int i = 0; i + 1 < int(barGroups().size()); ++i) {  // (placed by placePictureBar)
        auto* l = new QFrame(pictureBar_);
        l->setObjectName(QString("pictureLine%1").arg(i + 1));
        l->setFrameShape(QFrame::VLine);
        l->setFrameShadow(QFrame::Sunken);
    }
    button("pictureSmall", "S", tr("Small"), {}, [this] { resizePicture(PictureSize::Small); });
    button("pictureMedium", "M", tr("Medium"), {}, [this] { resizePicture(PictureSize::Medium); });
    button("pictureLarge", "L", tr("Large"), {}, [this] { resizePicture(PictureSize::Large); });
    button("pictureFit", {}, tr("As wide as the note"), "fit-width", [this] { resizePicture(PictureSize::FitWidth); });
    button("pictureOriginal", "1:1", tr("As large as it is"), {}, [this] { resizePicture(PictureSize::Original); });
    button("pictureCaption", tr("Caption"), tr("Add a caption under it"), {}, [this] { addCaption(); });
    button("pictureLeft", {}, tr("Left"), "align-left", [this] { alignPictures(Qt::AlignLeft); });
    button("pictureCenter", {}, tr("Centre"), "align-center", [this] { alignPictures(Qt::AlignHCenter); });
    button("pictureRight", {}, tr("Right"), "align-right", [this] { alignPictures(Qt::AlignRight); });
    button("pictureJoin", {}, tr("Beside the pictures above (or drag it there)"), "picture-join", [this] { joinPreviousRow(); });
    button("pictureOwnLine", {}, tr("On a line of its own"), "picture-own-line", [this] { pictureOwnLine(); });
    button("pictureOpen", {}, tr("Open it (or double-click)"), "open", [this] {
        if (int p = pickedPicture(); p >= 0 && openPicture)
            openPicture(formatAt(document(), p).name());
    });
    button("pictureDelete", {}, tr("Delete"), "trash", [this] { deletePicture(); });
    pictureBar_->hide();
}

int NoteTextEdit::pickedPicture() const
{
    if (picked_.isNull() || picked_.selectionEnd() - picked_.selectionStart() != 1)
        return -1;
    int p = picked_.selectionStart();
    return isPictureAt(document(), p) ? p : -1;
}

void NoteTextEdit::pickPicture(int pos)
{
    if (!isPictureAt(document(), pos)) {
        unpickPicture();
        return;
    }
    picked_ = QTextCursor(document());
    picked_.setPosition(pos);
    picked_.setPosition(pos + 1, QTextCursor::KeepAnchor);
    ++picking_;
    QTextCursor caret(document());  // before it, not shown
    caret.setPosition(pos);
    setTextCursor(caret);
    --picking_;
    updateCaret();
    viewport()->update();
    pictureBarTimer_.start();
}

void NoteTextEdit::unpickPicture()
{
    if (picked_.isNull())
        return;
    picked_ = QTextCursor();
    updateCaret();
    viewport()->update();
    pictureBarTimer_.start();
}

void NoteTextEdit::updateCaret()
{
    auto c = textCursor();
    rowCaret_ = picked_.isNull() && !isReadOnly() && !pictureRow(c.block()).empty();
    int width = !picked_.isNull() || rowCaret_ || dropAt_ >= 0 ? 0 : caretWidth_;
    if (cursorWidth() != width)
        setCursorWidth(width);
    caretShown_ = true;  // (moved: shown at once)
    if (rowCaret_ && hasFocus() && QApplication::cursorFlashTime() > 0)
        caretBlink_.start();
    else
        caretBlink_.stop();
    if (!drawnCaret_.isNull())
        viewport()->update(staleCaret().adjusted(-2, -2, 2, 2));
    if (rowCaret_)
        viewport()->update(rowCaretRect().adjusted(-2, -2, 2, 2));
}

QRect NoteTextEdit::staleCaret() const
{
    return drawnCaret_.translated(caretScroll_ - QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value()));
}

QRect NoteTextEdit::rowCaretRect() const
{
    // where Qt has it, but clear of the pictures beside it, and as high as text, standing on
    // the line as they do
    auto c = textCursor();
    int pos = c.position();
    auto line = cursorRect(c);
    QRectF before = pos > c.block().position() && isPictureAt(document(), pos - 1) ? pictureRect(pos - 1) : QRectF();
    QRectF after = pos < c.block().position() + c.block().length() - 1 && isPictureAt(document(), pos) ? pictureRect(pos) : QRectF();
    qreal x = line.left();
    for (auto& r : {before, after})  // (on its left or right: right to left, the one before is right)
        if (!r.isNull() && x > r.left() - 3 && x < r.right() + 2)
            x = x - r.left() < r.right() - x ? r.left() - 3 : r.right() + 2;
    // (a rect's bottom is under its last row of pixels)
    int foot = !after.isNull() ? qRound(after.bottom()) - 1 : !before.isNull() ? qRound(before.bottom()) - 1 : line.bottom();
    int height = std::min(line.height(), QFontMetrics(document()->defaultFont()).height());
    return QRect(qRound(x), foot - height + 1, std::max(1, caretWidth_), height);
}

QRectF NoteTextEdit::pictureRect(int pos) const
{
    auto r = images_->pictureRect(document(), pos);
    return r.translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
}

int NoteTextEdit::pictureAt(const QPoint& viewportPos, int* edge) const
{
    if (edge)
        *edge = 0;
    // the pictures of the line there drawn under the point (and of the line being composed in:
    // while something is composed, Qt's position for a point in it is not the note's)
    auto there = cursorForPosition(viewportPos).block();
    auto composing = textCursor().block();
    bool preedit = composing.layout() && !composing.layout()->preeditAreaText().isEmpty();
    for (auto block : {there, composing}) {
        if (block != there && (!preedit || block == there))
            continue;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            if (!isPicture(frag.charFormat()))
                continue;
            for (int at = frag.position(); at < frag.position() + frag.length(); ++at) {
                auto r = pictureRect(at);
                if (!r.contains(viewportPos))
                    continue;
                if (edge) {
                    qreal zone = std::min<qreal>(kEdge, r.width() / 4);
                    *edge = viewportPos.x() < r.left() + zone ? -1 : viewportPos.x() > r.right() - zone ? 1 : 0;
                }
                return at;
            }
        }
    }
    return -1;
}

int NoteTextEdit::pictureCornerAt(const QPoint& viewportPos) const
{
    int p = pickedPicture();
    if (p < 0 || isReadOnly())
        return -1;
    auto r = pictureRect(p);
    QPointF corners[] = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
    for (int i = 0; i < 4; ++i)
        if (std::abs(viewportPos.x() - corners[i].x()) <= kCornerHit && std::abs(viewportPos.y() - corners[i].y()) <= kCornerHit)
            return i;
    return -1;
}

void NoteTextEdit::placePictureBar()
{
    updateCaret();  // (after the edit, an undo or a scroll: the cursor as it is now)
    int p = pickedPicture();
    if (picked_.isNull() == false && p < 0)  // (taken away: an undo, say)
        unpickPicture();
    if (p < 0 || !hasFocus() || resizing_.corner >= 0) {
        pictureBar_->hide();
        return;
    }
    auto block = document()->findBlock(p);
    auto row = pictureRow(block);
    bool editable = !isReadOnly(), inRow = !row.empty(), table = inTable(block);
    bool canJoin = rowAbove(block).isValid();
    auto shown = [&](const QString& name) {
        if (name == "pictureOpen")
            return true;
        if (!editable)
            return false;
        if (name == "pictureOriginal")
            return row.size() <= 1;
        // one in a line of text has the line's alignment; in a list's item or a table's cell
        // there is no paragraph of their own under them, nor a line of its own
        if (name == "pictureCaption")
            return inRow && !table && !block.textList();
        if (name == "pictureLeft" || name == "pictureCenter" || name == "pictureRight")
            return inRow && !table;
        if (name == "pictureJoin")
            return canJoin;
        if (name == "pictureOwnLine")
            return row.size() != 1 && !table;
        return true;
    };
    auto r = pictureRect(p);
    auto view = viewport()->rect();
    if (!r.intersects(view)) {
        pictureBar_->hide();
        return;
    }
    // the groups in rows as wide as the note, a line between two on a row
    const int margin = 4, gap = 1, lineWidth = 9;
    int most = std::max(80, view.width() - 8), height = 0;
    for (auto& group : barGroups())
        for (auto* name : group)
            height = std::max(height, pictureBar_->findChild<QToolButton*>(name)->sizeHint().height());
    int x = margin, y = 2, width = 0, lines = 0;
    bool rowStarted = false;
    for (auto& group : barGroups()) {
        std::vector<QToolButton*> buttons;
        int groupWidth = -gap;
        for (auto* name : group) {
            auto* b = pictureBar_->findChild<QToolButton*>(name);
            b->setVisible(shown(name));
            if (shown(name)) {
                buttons.push_back(b);
                groupWidth += b->sizeHint().width() + gap;
            }
        }
        if (buttons.empty())
            continue;
        if (rowStarted && x + lineWidth + groupWidth + margin > most) {  // on the next row
            x = margin;
            y += height + 2;
            rowStarted = false;
        }
        if (rowStarted) {
            auto* line = pictureBar_->findChild<QFrame*>(QString("pictureLine%1").arg(++lines));
            line->setGeometry(x + lineWidth / 2 - 1, y + 3, 2, height - 6);
            line->show();
            x += lineWidth;
        }
        for (auto* b : buttons) {
            b->setGeometry(x, y, b->sizeHint().width(), height);
            x += b->sizeHint().width() + gap;
        }
        width = std::max(width, x - gap + margin);
        rowStarted = true;
    }
    for (int i = lines + 1; i < int(barGroups().size()); ++i)
        pictureBar_->findChild<QFrame*>(QString("pictureLine%1").arg(i))->hide();
    pictureBar_->resize(width, y + height + 2);
    QSize size = pictureBar_->size();
    int left = std::clamp(int(r.left()), 4, std::max(4, view.width() - size.width() - 4));
    int top = int(r.top()) - size.height() - 8;  // above it; else under it; else over it
    if (top < 4)
        top = int(r.bottom()) + 8;
    if (top + size.height() > view.height() - 4)
        top = std::max(4, int(r.top()) + 4);
    pictureBar_->move(left, top);
    pictureBar_->show();
    pictureBar_->raise();
}

void NoteTextEdit::paintEvent(QPaintEvent* e)
{
    QTextEdit::paintEvent(e);
    QPainter p(viewport());
    // the cursor in a line of pictures (see updateCaret); where something dragged would go
    drawnCaret_ = QRect();
    if (rowCaret_ && caretShown_ && hasFocus() && !isReadOnly() && dropAt_ < 0) {
        drawnCaret_ = rowCaretRect();
        caretScroll_ = QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        p.fillRect(drawnCaret_, palette().color(QPalette::Text));
    }
    if (dropAt_ >= 0 && dropLine_ != 0) {  // on a line of its own: a bar across, under or above
        auto block = document()->findBlock(dropBlock_);
        auto r = document()->documentLayout()->blockBoundingRect(block).translated(-horizontalScrollBar()->value(),
                                                                                  -verticalScrollBar()->value());
        qreal y = dropLine_ < 0 ? r.top() : r.bottom();
        p.fillRect(QRectF(r.left(), y - 1, std::max<qreal>(40, viewport()->width() - 2 * r.left()), 2),
                   palette().color(QPalette::Highlight));
    } else if (dropAt_ >= 0) {
        QTextCursor at(document());
        at.setPosition(std::min(dropAt_, document()->characterCount() - 1));
        auto r = cursorRect(at);
        p.fillRect(QRect(r.left() - 1, r.top(), 2, r.height()), palette().color(QPalette::Highlight));
    }
    // an empty caption: what goes there
    auto cursor = textCursor();
    if (hasFocus() && !isReadOnly() && isCaption(cursor.block()) && cursor.block().length() <= 1) {
        auto at = cursorRect(cursor);
        QFont f = document()->defaultFont();
        if (f.pointSizeF() > 0)
            f.setPointSizeF(f.pointSizeF() * 0.85);
        else if (f.pixelSize() > 0)
            f.setPixelSize(std::max(1, qRound(f.pixelSize() * 0.85)));
        p.setFont(f);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRectF(at.right() + 2, at.top(), 400, at.height()), Qt::AlignLeft | Qt::AlignVCenter, tr("Add a caption"));
    }
    int picked = pickedPicture();
    if (picked < 0 || !hasFocus())
        return;
    auto r = pictureRect(picked);
    auto accent = palette().color(QPalette::Highlight);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(accent, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(-1, -1, 1, 1));
    if (isReadOnly())
        return;
    p.setPen(QPen(accent, 1.5));
    p.setBrush(palette().color(QPalette::Base));
    for (auto c : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()})
        p.drawRect(QRectF(c - QPointF(kCorner / 2, kCorner / 2), QSizeF(kCorner, kCorner)));
}

void NoteTextEdit::resizeEvent(QResizeEvent* e)
{
    QTextEdit::resizeEvent(e);
    pictureBarTimer_.start();
}

void NoteTextEdit::focusInEvent(QFocusEvent* e)
{
    QTextEdit::focusInEvent(e);
    updateCaret();
    viewport()->update();
    pictureBarTimer_.start();
}

// --- the mouse --------------------------------------------------------------------------------

bool NoteTextEdit::pictureMousePress(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    auto pos = e->position().toPoint();
    bool shift = e->modifiers() & Qt::ShiftModifier;
    int corner = shift ? -1 : pictureCornerAt(pos);
    int edge = 0, hit = corner >= 0 ? -1 : pictureAt(pos, &edge);
    if (corner < 0 && (hit < 0 || shift || (edge != 0 && !isReadOnly()))) {
        // beside pictures, on one's edge (Qt puts the cursor on the side of its half there),
        // or a selection to there: as in text (Qt writes what is composed itself, after
        // reading the point)
        unpickPicture();  // (the cursor may go where it is already, before the one picked)
        return false;
    }
    // ours: what is composed written first (the picture followed: it moves what is after it)
    QTextCursor at(document());
    at.setPosition(corner >= 0 ? pickedPicture() : hit);
    finishComposing();
    if (corner >= 0) {
        resizing_.corner = corner;
        resizing_.at = pickedPicture();
        resizing_.start = pos;
        resizing_.rect = pictureRect(pickedPicture());
        resizing_.height = resizing_.rect.height();
        pictureBar_->hide();
        return true;
    }
    hit = at.position();
    if (!isPictureAt(document(), hit))
        return true;
    setFocus(Qt::MouseFocusReason);
    pickPicture(hit);
    pressedPicture_ = pickedPicture();  // (picking it renders a formula being typed: it may have moved)
    pressedAt_ = pos;
    return true;
}

bool NoteTextEdit::pictureMouseMove(QMouseEvent* e)
{
    auto pos = e->position().toPoint();
    if (resizing_.corner >= 0) {
        // as large as the corner is dragged, as wide as it is high as it was
        int p = pickedPicture();
        if (p < 0)
            return true;
        auto& r = resizing_.rect;
        bool right = resizing_.corner == 1 || resizing_.corner == 3, bottom = resizing_.corner >= 2;
        qreal aspect = r.width() / std::max<qreal>(1, r.height());
        qreal byX = (r.width() + (right ? 1 : -1) * (pos.x() - resizing_.start.x())) / aspect;
        qreal byY = r.height() + (bottom ? 1 : -1) * (pos.y() - resizing_.start.y());
        qreal height = std::abs(byX - r.height()) > std::abs(byY - r.height()) ? byX : byY;
        qreal most = maxPictureHeight(document(), p);  // (less than kLeastHeight for a very wide one)
        resizing_.height = std::clamp(height, std::min(kLeastHeight, most), most);
        images_->setResizing(document(), p, resizing_.height);
        auto block = document()->findBlock(p);
        document()->markContentsDirty(block.position(), block.length());
        viewport()->update();
        return true;
    }
    if (pressedPicture_ >= 0 && (e->buttons() & Qt::LeftButton)) {
        if ((pos - pressedAt_).manhattanLength() >= QApplication::startDragDistance() && !isReadOnly())
            startPictureDrag();
        return true;
    }
    if (e->buttons() != Qt::NoButton)
        return false;
    if (int c = pictureCornerAt(pos); c >= 0) {
        viewport()->setCursor(c == 0 || c == 3 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        return true;
    }
    if (int edge = 0; pictureAt(pos, &edge) >= 0) {  // (its edges: the cursor goes beside it)
        viewport()->setCursor(edge != 0 && !isReadOnly() ? Qt::IBeamCursor : Qt::ArrowCursor);
        viewport()->setToolTip({});
        return true;
    }
    return false;
}

bool NoteTextEdit::pictureMouseRelease(QMouseEvent*)
{
    if (resizing_.corner >= 0) {
        int p = pickedPicture();
        resizing_.corner = -1;
        images_->setResizing(document(), -1, 0);
        if (int at = std::min(resizing_.at, document()->characterCount() - 1); at >= 0) {  // (shown as stored again)
            auto block = document()->findBlock(at);
            document()->markContentsDirty(block.position(), block.length());
        }
        if (p >= 0) {
            if (std::abs(resizing_.height - resizing_.rect.height()) >= 0.5)  // (else nothing to undo)
                setPictureHeight(p, resizing_.height);
            pickPicture(p);
        }
        return true;
    }
    if (pressedPicture_ >= 0) {
        pressedPicture_ = -1;
        return true;
    }
    return false;
}

void NoteTextEdit::mouseDoubleClickEvent(QMouseEvent* e)
{
    int edge = 0;
    int p = pictureAt(e->position().toPoint(), &edge);
    if (p >= 0 && (edge == 0 || isReadOnly())) {
        QTextCursor at(document());  // (what is composed written first: it moves what is after it)
        at.setPosition(p);
        finishComposing();
        if (openPicture && isPictureAt(document(), at.position()))
            openPicture(formatAt(document(), at.position()).name());
        return;
    }
    if (p >= 0) {  // on its edge: as one click there (the cursor beside it)
        unpickPicture();
        QMouseEvent press(QEvent::MouseButtonPress, e->position(), e->scenePosition(), e->globalPosition(), e->button(),
                          e->buttons(), e->modifiers(), e->pointingDevice());
        QTextEdit::mousePressEvent(&press);
        return;
    }
    // beside pictures, in their line: the cursor stays there (a picture is no word to select)
    if (!pictureRow(cursorForPosition(e->position().toPoint()).block()).empty())
        return;
    QTextEdit::mouseDoubleClickEvent(e);
}

// Dragged within the note: moved there (beside another picture, it joins its row);
// dragged out of it: copied there.
void NoteTextEdit::startPictureDrag()
{
    int p = std::exchange(pressedPicture_, -1);
    if (!isPictureAt(document(), p))
        return;
    QTextCursor one(document());
    one.setPosition(p);
    one.setPosition(p + 1, QTextCursor::KeepAnchor);
    ++picking_;
    auto caret = textCursor();
    setTextCursor(one);
    auto* mime = createMimeDataFromSelection();
    setTextCursor(caret);
    --picking_;
    dragged_ = one;
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    auto image = document()->resource(QTextDocument::ImageResource, QUrl(formatAt(document(), p).name())).value<QImage>();
    if (!image.isNull())
        drag->setPixmap(QPixmap::fromImage(image.scaled(160, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    dragged_ = QTextCursor();
    if (dropAt_ >= 0) {  // (let go elsewhere, or not at all)
        dropAt_ = -1;
        dropLine_ = 0;
        updateCaret();
        viewport()->update();
    }
}

void NoteTextEdit::dragEnterEvent(QDragEnterEvent* e)
{
    dropPictures_ = picturesOnly(e->mimeData());  // (once: a note's copy is read to know)
    QTextEdit::dragEnterEvent(e);
}

void NoteTextEdit::dragMoveEvent(QDragMoveEvent* e)
{
    QTextEdit::dragMoveEvent(e);
    int at = e->isAccepted() ? cursorForPosition(e->position().toPoint()).position() : -1;
    // where it goes, as it is put there: pictures into a caption on a line under it; text at
    // pictures on a line of its own (above them from their line's start, else under them)
    int line = 0, block = -1;
    if (at >= 0) {
        auto there = document()->findBlock(at);
        if (!dragged_.isNull() && there == captionOf(document()->findBlock(dragged_.selectionStart()))) {
            at = -1;  // (its own picture's caption: dropped there, it stays where it is)
        } else if (dropPictures_ && isCaption(there)) {
            line = 1;
            block = there.position();
        } else if (!dropPictures_ && !pictureRow(there).empty() && !inTable(there)) {
            bool above = at == there.position();
            auto caption = captionOf(there);
            line = above ? -1 : 1;
            block = (above || !caption.isValid() ? there : caption).position();
        }
    }
    if (at == dropAt_ && line == dropLine_ && block == dropBlock_)
        return;
    dropAt_ = at;
    dropLine_ = line;
    dropBlock_ = block;
    updateCaret();  // (the text's own marker hidden: it is no wider than the cursor)
    viewport()->update();
}

void NoteTextEdit::dragLeaveEvent(QDragLeaveEvent* e)
{
    QTextEdit::dragLeaveEvent(e);
    dropAt_ = -1;
    dropLine_ = 0;
    updateCaret();
    viewport()->update();
}

void NoteTextEdit::dropEvent(QDropEvent* e)
{
    dropAt_ = -1;
    dropLine_ = 0;
    if (!dragged_.isNull() && e->source() == this) {
        QDragLeaveEvent leave;  // (the text's own drop: its marker where it would go gone)
        QTextEdit::dragLeaveEvent(&leave);
        int to = cursorForPosition(e->position().toPoint()).position();
        movePicture(dragged_.selectionStart(), to);
        e->setDropAction(Qt::MoveAction);
        e->accept();
    } else {
        unpickPicture();  // (let go right before the one picked: there, not after it)
        QTextEdit::dropEvent(e);
    }
    updateCaret();
    viewport()->update();
}

void NoteTextEdit::finishComposing()
{
    auto block = textCursor().block();
    if (!block.layout() || block.layout()->preeditAreaText().isEmpty())
        return;
    QGuiApplication::inputMethod()->commit();
    block = textCursor().block();
    if (block.layout() && !block.layout()->preeditAreaText().isEmpty()) {  // (no input method to write it:
        block.layout()->setPreeditArea(-1, QString());                     // dropped, as Qt does)
        block.layout()->clearFormats();
        document()->markContentsDirty(block.position(), block.length());
    }
    endComposing();
}

void NoteTextEdit::endComposing()
{
    composeTo_ = QTextCursor();
    composeLine_ = QTextCursor();
    composeSteps_ = -1;
    composeShift_ = 0;
}

// --- the keys -----------------------------------------------------------------------------------

bool NoteTextEdit::resizeKey(QKeyEvent* e)
{
    if (resizing_.corner < 0)
        return false;
    if (e->key() == Qt::Key_Escape) {  // the drag given up: as it was
        resizing_.corner = -1;
        images_->setResizing(document(), -1, 0);
        if (int at = std::min(resizing_.at, document()->characterCount() - 1); at >= 0) {
            auto block = document()->findBlock(at);
            document()->markContentsDirty(block.position(), block.length());
        }
        if (int p = pickedPicture(); p >= 0)
            pickPicture(p);  // (its buttons again)
        viewport()->update();
    }
    return true;  // (other keys wait: they would change what is being resized)
}

bool NoteTextEdit::pictureKey(QKeyEvent* e)
{
    int p = pickedPicture();
    if (p < 0)
        return false;
    auto block = document()->findBlock(p);
    bool inRow = !pictureRow(block).empty(), table = inTable(block);
    auto mods = e->modifiers() & ~Qt::KeypadModifier;
    auto caretAt = [this](int pos) {
        QTextCursor c(document());
        c.setPosition(pos);
        setTextCursor(c);
    };
    if (e == QKeySequence::Copy || e == QKeySequence::Cut) {
        copyPicture(e == QKeySequence::Cut && !isReadOnly());
        return true;
    }
    if (e == QKeySequence::Undo || e == QKeySequence::Redo)
        return false;  // (Alt+Backspace is Undo on some keyboards)
    // (with Ctrl or Alt too: the picture, not a word and the line before it)
    bool erase = e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete;
    if (isReadOnly())
        return erase;
    if (erase) {
        deletePicture(e->key() == Qt::Key_Backspace);
        return true;
    }
    if (typesText(e)) {  // on a line under its row (after it, in a line of text); what is
                         // pasted goes there too (insertFromMimeData), pictures beside it
        typeAside();
        return false;  // (typed there)
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {  // (with any modifier)
        if (!inRow || table) {  // in a line of text or a cell: the line breaks after it, as usual
            caretAt(p + 1);
            return false;
        }
        setTextCursor(newLineAfter(block));
        return true;
    }
    if ((e->key() == Qt::Key_Tab && mods == Qt::NoModifier) || (e->key() == Qt::Key_Backtab && mods == Qt::ShiftModifier))
        return inRow && !table;  // (in a cell: to the next one, as usual)
    if (mods != Qt::NoModifier)  // (Shift and an arrow: a selection from it, as in text)
        return false;
    switch (e->key()) {  // no longer picked: the cursor beside it, or where the key takes it
    case Qt::Key_Left:
        unpickPicture();  // (before it, where the cursor is already: it does not move)
        caretAt(p);
        return true;
    case Qt::Key_Right:
    case Qt::Key_Escape:
        caretAt(p + 1);
        return true;
    case Qt::Key_Home:
    case Qt::Key_End:
        unpickPicture();  // (from before it, as usual: the start or end of the line shown)
        return false;
    default:
        return false;  // (Up, Down, Page Up, Page Down: from before it, as usual)
    }
}

// The cursor in a line of pictures, beside them: what is typed goes on a line of its own
// (above them from the line's start, else under them); Enter breaks the line there (at its
// start a line above, at its end one under, past their caption); Backspace and Delete pick
// the picture before or after it (at the line's start or end: an empty line next to it
// goes, pictures there are picked, text is not joined to them); Tab does nothing. (In a
// table's cell, the cell's line: as in text.)
bool NoteTextEdit::rowKey(QKeyEvent* e)
{
    if (isReadOnly() || pickedPicture() >= 0)
        return false;
    auto c = textCursor();
    auto block = c.block();
    if (c.hasSelection() || pictureRow(block).empty() || inTable(block))
        return false;
    if (typesText(e)) {
        typeAside();
        return false;  // (typed there)
    }
    if (e == QKeySequence::Cut || e == QKeySequence::Copy || e == QKeySequence::Paste || e == QKeySequence::Undo ||
        e == QKeySequence::Redo)
        return false;
    bool atStart = c.atBlockStart(), atEnd = c.atBlockEnd();
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:  // (Shift+Enter too: a line break among them would end the row)
        if (atEnd) {
            setTextCursor(newLineAfter(block));
        } else if (atStart) {  // a line above; the cursor stays at the start of the pictures
            QTextCursor back(document());
            back.setPosition(newLineBefore(block).position() + 1);
            setTextCursor(back);
        } else {  // the row in two (its caption under the second)
            c.beginEditBlock();
            c.insertBlock(block.blockFormat(), block.charFormat());
            c.endEditBlock();
            setTextCursor(c);
        }
        return true;
    case Qt::Key_Backspace:
        if (!atStart) {
            pickPicture(c.position() - 1);
        } else if (auto prev = block.previous(); sameCell(prev, block)) {
            if (prev.length() <= 1) {
                c.beginEditBlock();
                removeEmptyBlock(prev);
                c.endEditBlock();
            } else if (auto above = pictureRow(prev); !above.empty()) {
                pickPicture(above.back());
            } else {
                QTextCursor end(document());
                end.setPosition(prev.position() + prev.length() - 1);
                setTextCursor(end);
            }
        }
        return true;
    case Qt::Key_Delete:
        if (!atEnd) {
            pickPicture(c.position());
        } else if (auto next = block.next(); sameCell(next, block)) {
            if (next.length() <= 1) {
                c.beginEditBlock();
                removeEmptyBlock(next);
                c.endEditBlock();
            } else if (auto below = pictureRow(next); !below.empty()) {
                pickPicture(below.front());
            }
        }
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return true;
    default:
        return false;
    }
}

void NoteTextEdit::typeAside()
{
    // the line made as its own step (laid out, so the view can follow), the text typed joined
    // to it: one step to undo (ended by keyPressEvent)
    if (caretForText()) {
        typingStep_ = textCursor();
        typingStep_.joinPreviousEditBlock();
    }
}

bool NoteTextEdit::caretForText()
{
    auto c = textCursor();
    if (c.hasSelection())
        return false;
    // a format set for what is typed next (bold, say: not the format there) goes with it;
    // read now (a cursor moved by an edit forgets it)
    QTextCursor there(document());
    there.setPosition(c.position());
    std::optional<QTextCharFormat> pending;
    if (c.charFormat() != there.charFormat())
        pending = textFormatOf(c.charFormat());
    int lines = document()->blockCount();
    QTextCursor to;
    if (int p = pickedPicture(); p >= 0 && c.position() == p) {
        auto block = document()->findBlock(p);
        if (pictureRow(block).empty() || inTable(block)) {  // in a line of text, or a cell: after it
            to = QTextCursor(document());
            to.setPosition(p + 1);
        } else {
            to = emptyLineAt(block, false);
        }
    } else if (auto block = c.block(); !pictureRow(block).empty() && !inTable(block)) {
        to = emptyLineAt(block, c.atBlockStart());
    } else {
        return false;
    }
    bool made = document()->blockCount() > lines;
    if (pending)
        to.setCharFormat(*pending);
    setTextCursor(to);
    return made;
}

void NoteTextEdit::inputMethodEvent(QInputMethodEvent* e)
{
    if (resizing_.corner >= 0)
        return;  // (held while a corner is dragged, as keys are: resizeKey)
    bool composing = !e->preeditString().isEmpty(), writing = !e->commitString().isEmpty();
    // what is composed no longer shown where it was moved to (the note opened again, an undo):
    // that composition over
    if (!composeTo_.isNull() && (textCursor().block() != composeTo_.block() || !textCursor().block().layout() ||
                                 textCursor().block().layout()->preeditAreaText().isEmpty()))
        endComposing();
    if (!isReadOnly() && (composing || writing) && composeTo_.isNull()) {
        int from = textCursor().position();  // (a number: a cursor would move with the line made)
        bool made = caretForText();  // (at pictures: on a line of its own)
        if (made || textCursor().position() != from) {
            composeTo_ = textCursor();
            // (moved within its line, past a picture there: what it replaces is that far back)
            composeShift_ = made ? 0 : textCursor().position() - from;
            if (made) {
                composeLine_ = textCursor();
                composeSteps_ = document()->availableUndoSteps();
            }
        }
    }
    // the line made for it, the last step still: what is written joins that step
    bool ours = !composeLine_.isNull() && document()->availableUndoSteps() == composeSteps_;
    QTextCursor step;
    if (writing && ours) {
        step = textCursor();
        step.joinPreviousEditBlock();
    }
    // text it replaces before the cursor (as the input method had it): where that is from the
    // cursor now, and not past the start of the line it was moved to (joined to the pictures)
    int start = e->replacementStart(), length = e->replacementLength();
    if (!composeTo_.isNull() && (start != 0 || length != 0)) {
        start -= composeShift_;
        int inLine = textCursor().positionInBlock();
        if (start < -inLine) {
            length = std::max(0, length - (-inLine - start));
            start = -inLine;
        }
        QInputMethodEvent within(e->preeditString(), e->attributes());
        within.setCommitString(e->commitString(), start, length);
        QTextEdit::inputMethodEvent(&within);
    } else {
        QTextEdit::inputMethodEvent(e);
    }
    if (!step.isNull()) {
        step.endEditBlock();
        ensureCursorVisible();  // (laid out only now: its line as high as what was written)
    }
    if (!writing && !composing && ours && textCursor().block() == composeLine_.block() && composeLine_.block().length() <= 1) {
        // given up: the line made for it gone (its own step, the last), the cursor back
        undo();
        document()->clearUndoRedoStacks(QTextDocument::RedoStack);  // (nothing to redo: it was never asked for)
    }
    if (writing || !composing)
        endComposing();
}

// Next to pictures and in captions: Enter ends a caption (a paragraph after it, not another
// caption); Backspace and Delete pick a picture rather than join a line with its row.
bool NoteTextEdit::captionKey(QKeyEvent* e)
{
    auto mods = e->modifiers() & ~Qt::KeypadModifier;
    // Backspace and Delete with Ctrl, Alt or Shift too (a word deleted would be the line's
    // end, joining the line to the pictures); Enter with any but Shift (a line break in it)
    bool erase = (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) && !e->matches(QKeySequence::Undo) &&
                 !e->matches(QKeySequence::Redo) && !e->matches(QKeySequence::Cut);
    bool enter = (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(mods & Qt::ShiftModifier);
    if (isReadOnly() || (mods != Qt::NoModifier && !erase && !enter))
        return false;
    auto c = textCursor();
    if (c.hasSelection())
        return false;
    auto block = c.block();
    if (enter && isCaption(block)) {
        // a paragraph after it: what is after the cursor, as a paragraph's text; at its start,
        // under the whole caption (else it would go, and its text be a paragraph)
        c.beginEditBlock();
        bool atStart = c.atBlockStart() && block.length() > 1;
        if (atStart)
            c.movePosition(QTextCursor::EndOfBlock);
        c.insertBlock(textAfter(block.blockFormat()), QTextCharFormat());
        if (!atStart)
            setCaption(c.block(), false);
        c.endEditBlock();
        setTextCursor(c);
        return true;
    }
    if (erase && e->key() == Qt::Key_Backspace && c.atBlockStart() && sameCell(block.previous(), block)) {
        // at the start of a caption, or of a line under pictures
        auto row = pictureRow(block.previous());
        if (row.empty())
            return false;
        if (block.length() <= 1) {  // an empty line (or caption) under them: it goes
            c.beginEditBlock();
            removeEmptyBlock(block);
            c.endEditBlock();
        }
        if (hasFocus())
            pickPicture(row.back());
        return true;
    }
    if (erase && e->key() == Qt::Key_Delete && c.atBlockEnd() && sameCell(block.next(), block) && pictureRow(block).empty()) {
        auto row = pictureRow(block.next());
        if (row.empty())
            return false;
        if (block.length() <= 1) {  // an empty line above them: it goes
            c.beginEditBlock();
            auto rowFormat = block.next().blockFormat();
            c.deleteChar();
            c.setBlockFormat(rowFormat);
            c.endEditBlock();
            row = pictureRow(textCursor().block());
        }
        if (hasFocus() && !row.empty())
            pickPicture(row.front());
        return true;
    }
    return false;
}

void NoteTextEdit::copyPicture(bool cut)
{
    int p = pickedPicture();
    if (p < 0)
        return;
    QTextCursor one(document());
    one.setPosition(p);
    one.setPosition(p + 1, QTextCursor::KeepAnchor);
    ++picking_;
    setTextCursor(one);
    copy();
    QTextCursor back(document());
    back.setPosition(p);
    setTextCursor(back);
    --picking_;
    if (cut)
        deletePicture(false, true);  // (its caption stays, as text: only the picture is taken)
}

// --- what the buttons do ---------------------------------------------------------------------

QTextCursor NoteTextEdit::newLineAfter(const QTextBlock& row)
{
    auto caption = captionOf(row);
    auto after = caption.isValid() ? caption : row;
    auto step = textCursor();  // (undone: the cursor back where it was, not at the caption's end)
    step.beginEditBlock();
    QTextCursor c(document());
    c.setPosition(after.position() + after.length() - 1);
    c.insertBlock(textAfter(row.blockFormat()), QTextCharFormat());
    step.endEditBlock();
    return c;
}

QTextCursor NoteTextEdit::newLineBefore(const QTextBlock& row)
{
    auto format = row.blockFormat();
    auto step = textCursor();  // (undone: the cursor back where it was)
    step.beginEditBlock();
    QTextCursor c(document());
    c.setPosition(row.position());
    c.insertBlock(format, row.charFormat());  // (the pictures a line down)
    c.movePosition(QTextCursor::PreviousBlock);
    c.setBlockFormat(textAfter(format));
    c.setBlockCharFormat(QTextCharFormat());
    step.endEditBlock();
    return c;
}

QTextCursor NoteTextEdit::emptyLineAt(const QTextBlock& row, bool above)
{
    auto caption = captionOf(row);
    auto next = above ? row.previous() : (caption.isValid() ? caption : row).next();
    if (sameCell(next, row) && next.length() <= 1 && !isCaption(next))
        return QTextCursor(next);
    return above ? newLineBefore(row) : newLineAfter(row);
}

void NoteTextEdit::setPictureHeight(int pos, qreal height)
{
    auto block = document()->findBlock(pos);
    auto row = pictureRow(block);
    if (row.empty())  // one in a line of text
        row = {pos};
    QTextCursor c(document());
    c.setPosition(pos);
    c.beginEditBlock();  // (undone: the cursor back here, not at the note's start)
    for (int p : row) {
        c.setPosition(p);
        c.setPosition(p + 1, QTextCursor::KeepAnchor);
        c.setCharFormat(sized(document(), formatAt(document(), p), height));
    }
    c.endEditBlock();
}

void NoteTextEdit::resizePicture(PictureSize size)
{
    int p = pickedPicture();
    if (p < 0 || isReadOnly())
        return;
    auto f = formatAt(document(), p);
    auto own = pictureSize(document(), f), natural = naturalPictureSize(document(), f);
    qreal aspect = aspectOf(document(), f);
    bool row = pictureRow(document()->findBlock(p)).size() > 1;
    auto within = [&](QSizeF box) { return row ? box.height() : std::min(box.width() / aspect, box.height()); };
    qreal height = size == PictureSize::Small      ? within(kSmall)
                   : size == PictureSize::Medium   ? within(kMedium)
                   : size == PictureSize::Large    ? within(kLarge)
                   : size == PictureSize::FitWidth ? maxPictureHeight(document(), p)
                   : natural.isEmpty()             ? own.height()
                                                   : natural.height();
    // (kept so: shown as wide as the note where that is less, as large where it is wider)
    setPictureHeight(p, std::max(height, kLeastHeight));
    pickPicture(p);
}

void NoteTextEdit::addCaption()
{
    int p = pickedPicture();
    if (p < 0 || isReadOnly())
        return;
    auto block = document()->findBlock(p);
    if (pictureRow(block).empty() || inTable(block) || block.textList())
        return;  // (no paragraph of their own under them there)
    QTextCursor c(document());
    if (auto caption = captionOf(block); caption.isValid()) {  // there already: to its end
        c.setPosition(caption.position() + caption.length() - 1);
    } else {
        c.setPosition(block.position() + block.length() - 1);
        QTextBlockFormat f = textAfter(block.blockFormat());
        f.setAlignment(block.blockFormat().alignment());  // under the pictures, as they are
        f.setProperty(kCaptionProperty, true);
        c.beginEditBlock();
        c.insertBlock(f, captionCharFormat());
        c.endEditBlock();
    }
    setTextCursor(c);
    setFocus();
}

void NoteTextEdit::alignPictures(Qt::Alignment align)
{
    int p = pickedPicture();
    if (p < 0 || isReadOnly())
        return;
    auto block = document()->findBlock(p);
    QTextCursor c(block);
    c.beginEditBlock();
    QTextBlockFormat f;
    f.setAlignment(align);
    c.mergeBlockFormat(f);
    if (auto caption = captionOf(block); caption.isValid())  // its caption with it
        QTextCursor(caption).mergeBlockFormat(f);
    c.endEditBlock();
    pickPicture(p);
}

bool NoteTextEdit::joinPreviousRow()
{
    int p = pickedPicture();
    if (p < 0 || isReadOnly())
        return false;
    auto block = document()->findBlock(p);
    auto target = rowAbove(block);
    if (!target.isValid())
        return false;
    auto f = formatAt(document(), p);
    if (qreal height = knownRowHeight(document(), target); height > 0)  // (as high as they are)
        f = sized(document(), f, height);
    QTextCursor c(document());
    c.setPosition(p);
    c.beginEditBlock();
    c.setPosition(p + 1, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    if (block.length() <= 1) {  // its line is empty: gone, and a caption it had a paragraph
        if (auto caption = captionOf(block); caption.isValid())
            setCaption(caption, false);
        removeEmptyBlock(block);
    }
    QTextCursor end(document());
    end.setPosition(target.position() + target.length() - 1);
    end.insertImage(f);
    evenRow(document(), end.block());
    c.endEditBlock();
    pickPicture(end.position() - 1);
    return true;
}

bool NoteTextEdit::pictureOwnLine()
{
    int p = pickedPicture();
    if (p < 0 || isReadOnly())
        return false;
    auto block = document()->findBlock(p);
    auto row = pictureRow(block);
    if (row.size() == 1 || inTable(block))
        return false;  // (on its own line already; in a cell, one line)
    if (row.empty()) {  // in a line of text: the line split around it
        auto f = formatAt(document(), p);
        QTextCursor c(document());
        c.setPosition(p);
        c.beginEditBlock();
        c.setPosition(p + 1, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        insertPictureLine(c, {f});
        c.endEditBlock();
        pickPicture(c.position() - 1);
        return true;
    }
    auto f = sized(document(), formatAt(document(), p), rowHeight(document(), block));  // as large as in the row
    auto caption = captionOf(block);
    auto after = caption.isValid() ? caption : block;
    QTextCursor end(document());  // (moves with what is taken out before it)
    end.setPosition(after.position() + after.length() - 1);
    auto lineFormat = block.blockFormat();
    lineFormat.clearProperty(kCaptionProperty);
    QTextCursor c(document());
    c.setPosition(p);
    c.beginEditBlock();
    c.setPosition(p + 1, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    end.insertBlock(lineFormat, QTextCharFormat());
    end.insertImage(f);
    c.endEditBlock();
    pickPicture(end.position() - 1);
    return true;
}

void NoteTextEdit::deletePicture(bool backwards, bool keepCaption)
{
    int p = pickedPicture();
    if (p < 0 || isReadOnly())
        return;
    auto block = document()->findBlock(p);
    auto row = pictureRow(block);
    int i = int(std::find(row.begin(), row.end(), p) - row.begin());
    bool joinsAbove = sameCell(block.previous(), block);  // (its line, emptied, goes into the one above)
    QTextCursor c(document());
    c.setPosition(p);
    c.beginEditBlock();
    c.setPosition(p + 1, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    bool empty = block.length() <= 1;
    if (empty) {  // its line gone, and its caption with it (or left as a paragraph)
        if (auto caption = captionOf(block); caption.isValid() && keepCaption) {
            setCaption(caption, false);
        } else if (caption.isValid()) {
            c.setPosition(block.position() + block.length() - 1);
            c.setPosition(caption.position() + caption.length() - 1, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        removeEmptyBlock(document()->findBlock(p));  // (the line left of the two)
        // (the note's only lines: the empty one left may have the caption's format)
        if (auto left = document()->findBlock(std::min(p, document()->characterCount() - 1));
            isCaption(left) && left.length() <= 1)
            setCaption(left, false);
    }
    c.endEditBlock();
    if (!empty && row.size() > 1) {  // the one after it, or else before it
        pickPicture(i + 1 < int(row.size()) ? row[i] : row[i - 1]);
        return;
    }
    unpickPicture();
    // the cursor where it was: (its line gone) at the end of the line before it, after
    // Backspace, else at the start of the next
    QTextCursor at(document());
    at.setPosition(std::min(empty && backwards && joinsAbove ? p - 1 : p, document()->characterCount() - 1));
    setTextCursor(at);
}

bool NoteTextEdit::movePicture(int from, int to)
{
    if (!isPictureAt(document(), from) || isReadOnly())
        return false;
    auto source = document()->findBlock(from);
    // dropped where it is: right before or after it; or, alone on its line, at the end of the
    // text above it or the start of the text under it (its caption's): it would stay there
    auto text = [&](const QTextBlock& b) { return sameCell(b, source) && b.length() > 1 && pictureRow(b).empty(); };
    bool alone = pictureRow(source).size() == 1 && !inTable(source);
    if (to == from || to == from + 1 || (alone && text(source.previous()) && to == from - 1) ||
        (alone && text(source.next()) && to == source.next().position())) {
        pickPicture(from);
        return false;
    }
    if (auto there = document()->findBlock(to); isCaption(there)) {
        if (there == captionOf(source)) {  // (onto its own caption: where it is)
            pickPicture(from);
            return false;
        }
        to = there.position() + there.length() - 1;  // (into a caption: under it, kept whole)
    }
    auto f = formatAt(document(), from);
    if (auto there = document()->findBlock(to); there != source)
        if (qreal height = knownRowHeight(document(), there); height > 0)  // (into a row: as high as it)
            f = sized(document(), f, height);
    QTextCursor dest(document());  // (moves with what is taken out before it)
    dest.setPosition(to);
    QTextCursor c(document());
    c.setPosition(from);
    c.beginEditBlock();
    c.setPosition(from + 1, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    if (source.length() <= 1 && dest.block() != source) {  // its line emptied: gone
        if (auto caption = captionOf(source); caption.isValid())
            setCaption(caption, false);
        removeEmptyBlock(source);
    }
    insertPictureLine(dest, {f});  // (into text: on a line of its own there)
    evenRow(document(), dest.block());
    c.endEditBlock();
    pickPicture(dest.position() - 1);
    return true;
}

void NoteTextEdit::adoptPictures(QTextDocument* pasted, const QMimeData* source)
{
    if (!storeImage || isReadOnly())
        return;
    std::vector<std::pair<int, QTextImageFormat>> foreign;
    for (auto block = pasted->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            auto f = frag.charFormat().toImageFormat();
            if (!isPicture(f) || document()->resource(QTextDocument::ImageResource, QUrl(f.name())).isValid())
                continue;  // (a formula; or this note's)
            for (int i = 0; i < frag.length(); ++i)
                foreign.push_back({frag.position() + i, f});
        }
    QHash<QString, QString> copies;  // (the same picture twice: one copy)
    for (auto& [pos, f] : foreign) {
        auto name = copies.value(f.name());
        if (name.isEmpty()) {
            QImage image = loadPicture ? loadPicture(f.name()) : QImage();
            if (image.isNull() && foreign.size() == 1 && source->hasImage())  // (one copied: its image is there too)
                image = qvariant_cast<QImage>(source->imageData());
            if (image.isNull() || (name = storeImage(image)).isEmpty())
                continue;  // (not to be had: as it was)
            document()->addResource(QTextDocument::ImageResource, QUrl(name), image);
            copies.insert(f.name(), name);
        }
        auto own = f;
        own.setName(name);
        QTextCursor c(pasted);
        c.setPosition(pos);
        c.setPosition(pos + 1, QTextCursor::KeepAnchor);
        c.setCharFormat(own);
    }
}

void NoteTextEdit::insertPictures(const QList<QImage>& images)
{
    if (!storeImage || isReadOnly())
        return;
    QStringList names;
    for (auto& image : images) {
        if (image.isNull())
            continue;
        auto name = storeImage(image);
        if (name.isEmpty())
            continue;
        document()->addResource(QTextDocument::ImageResource, QUrl(name), image);
        names << name;
    }
    QList<QTextImageFormat> pictures;
    for (auto& name : names) {
        QTextImageFormat f;
        f.setName(name);
        pictures << f;
    }
    placePictures(pictures);
}

void NoteTextEdit::placePictures(const QList<QTextImageFormat>& pictures)
{
    if (pictures.isEmpty() || isReadOnly())
        return;
    if (auto run = runAtCursor())  // (a formula being typed: done first, not split by them)
        leaveMath(*run, false);
    auto step = textCursor();  // (one step to undo, back to here)
    step.beginEditBlock();
    // where the cursor is: right after the one picked, in its line; into a line of pictures
    // where it is in it; else on a line of their own there (dropped: where the cursor was put
    // for the drop, without a word)
    auto c = textCursor();
    if (int p = pickedPicture(); p >= 0 && c.position() == p && !c.hasSelection())
        c.setPosition(p + 1);
    if (c.hasSelection())
        c.removeSelectedText();
    if (isCaption(c.block()))  // (into a caption: under it, kept whole)
        c.movePosition(QTextCursor::EndOfBlock);
    // as high as the line of pictures they go into (else as the first of them): the line
    // stays as high as it was
    auto sizedPictures = pictures;
    qreal height = knownRowHeight(document(), c.block());
    for (auto& f : pictures)
        if (height <= 0 && pictures.size() > 1 && pictureKnown(document(), f))
            height = pictureSize(document(), f).height();
    if (height > 0)
        for (auto& f : sizedPictures)
            f = sized(document(), f, height);
    insertPictureLine(c, sizedPictures);
    evenRow(document(), c.block());
    step.endEditBlock();
    pickPicture(c.position() - 1);
}

} // namespace on::gui
