// Pictures in notes, as in Feishu or Slack: shown small unless made larger; a click picks
// one (an outline and four corners, no text cursor next to it), with buttons above it.
// Pictures alone on a line are a row, all as high; making one larger or smaller makes the
// row so, as wide as the text at most. A caption is the paragraph under a line of pictures.
// (In a table's cell, one line when saved, pictures stay where they are put.)

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QDropEvent>
#include <QFrame>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextList>
#include <QTextTable>
#include <QToolButton>

#include <algorithm>

#include "gui/icons.h"
#include "gui/note_editor.h"
#include "gui/note_html.h"
#include "gui/rich_text_images.h"

namespace on::gui {

namespace {

constexpr qreal kCorner = 8;               // a corner's square
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
    // the cursor in a line of pictures: one of them picked instead (the one after it, or the
    // last), so no text cursor stands there as high as they are
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        if (picking_)
            return;
        auto c = textCursor();
        auto row = c.hasSelection() || !dragged_.isNull() ? std::vector<int>{} : pictureRow(c.block());
        if (row.empty()) {
            unpickPicture();
            return;
        }
        int p = c.position();
        pickPicture(std::find(row.begin(), row.end(), p) != row.end() ? p : row.back());
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
    setCursorWidth(0);
    viewport()->update();
    pictureBarTimer_.start();
}

void NoteTextEdit::unpickPicture()
{
    if (picked_.isNull())
        return;
    picked_ = QTextCursor();
    setCursorWidth(caretWidth_);
    viewport()->update();
    pictureBarTimer_.start();
}

QRectF NoteTextEdit::pictureRect(int pos) const
{
    auto r = images_->pictureRect(document(), pos);
    return r.translated(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
}

int NoteTextEdit::pictureAt(const QPoint& viewportPos) const
{
    int p = cursorForPosition(viewportPos).position();
    for (int at : {p, p - 1})
        if (isPictureAt(document(), at) && pictureRect(at).contains(viewportPos))
            return at;
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
        if (std::abs(viewportPos.x() - corners[i].x()) <= kCorner && std::abs(viewportPos.y() - corners[i].y()) <= kCorner)
            return i;
    return -1;
}

void NoteTextEdit::placePictureBar()
{
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
    viewport()->update();
    pictureBarTimer_.start();
}

// --- the mouse --------------------------------------------------------------------------------

bool NoteTextEdit::pictureMousePress(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return false;
    auto pos = e->position().toPoint();
    if (int corner = pictureCornerAt(pos); corner >= 0) {
        resizing_.corner = corner;
        resizing_.start = pos;
        resizing_.rect = pictureRect(pickedPicture());
        resizing_.height = resizing_.rect.height();
        pictureBar_->hide();
        return true;
    }
    int hit = pictureAt(pos);
    if (hit < 0)
        return false;
    setFocus(Qt::MouseFocusReason);
    pickPicture(hit);
    pressedPicture_ = hit;
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
    if (pictureAt(pos) >= 0) {
        viewport()->setCursor(Qt::ArrowCursor);
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
        if (p >= 0) {
            if (std::abs(resizing_.height - resizing_.rect.height()) >= 0.5)  // (else nothing to undo)
                setPictureHeight(p, resizing_.height);
            else
                document()->markContentsDirty(p, 1);
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
    if (int p = pictureAt(e->position().toPoint()); p >= 0) {
        if (openPicture)
            openPicture(formatAt(document(), p).name());
        return;
    }
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
}

void NoteTextEdit::dropEvent(QDropEvent* e)
{
    if (!dragged_.isNull() && e->source() == this) {
        QDragLeaveEvent leave;  // (the text's own drop: its marker where it would go gone)
        QTextEdit::dragLeaveEvent(&leave);
        int to = cursorForPosition(e->position().toPoint()).position();
        movePicture(dragged_.selectionStart(), to);
        e->setDropAction(Qt::MoveAction);
        e->accept();
        return;
    }
    QTextEdit::dropEvent(e);
}

// --- the keys -----------------------------------------------------------------------------------

bool NoteTextEdit::pictureKey(QKeyEvent* e)
{
    int p = pickedPicture();
    if (p < 0)
        return false;
    auto block = document()->findBlock(p);
    auto row = pictureRow(block);
    int i = int(std::find(row.begin(), row.end(), p) - row.begin());
    bool inRow = i < int(row.size());
    auto mods = e->modifiers() & ~Qt::KeypadModifier;
    auto caretAt = [this](int pos) {
        QTextCursor c(document());
        c.setPosition(pos);
        setTextCursor(c);  // (into a line of pictures: one of them is picked)
    };
    if (e == QKeySequence::Copy || e == QKeySequence::Cut) {
        copyPicture(e == QKeySequence::Cut && !isReadOnly());
        return true;
    }
    // (with Ctrl or Alt too: the picture, not a word and the line before it)
    bool erase = e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete;
    if (isReadOnly())
        return erase;
    if (erase) {
        deletePicture(e->key() == Qt::Key_Backspace);
        return true;
    }
    // text (with Alt or AltGr too, as an editor takes it): on a line under the pictures;
    // what is pasted goes there too (insertFromMimeData)
    if (!e->text().isEmpty() && (e->text().at(0).isPrint() || e->text().at(0).category() == QChar::Other_Format) &&
        mods != Qt::ControlModifier && mods != (Qt::ControlModifier | Qt::ShiftModifier)) {
        caretForText();
        return false;  // (typed there)
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {  // (with any modifier)
        if (!inRow) {
            caretAt(p + 1);
            return false;  // (and the line breaks there, as usual)
        }
        setTextCursor(newLineAfterPictures());
        return true;
    }
    if ((e->key() == Qt::Key_Tab && mods == Qt::NoModifier) || (e->key() == Qt::Key_Backtab && mods == Qt::ShiftModifier))
        return inRow;
    if (mods != Qt::NoModifier)  // (Shift and an arrow: a selection from it, as in text)
        return false;
    switch (e->key()) {
    case Qt::Key_Left:
        if (inRow && i > 0) {
            pickPicture(row[i - 1]);
        } else if (inRow && block.previous().isValid()) {
            caretAt(block.position() - 1);
        } else if (!inRow) {
            unpickPicture();  // (the cursor is there already: it does not move)
            caretAt(p);
        }
        return true;
    case Qt::Key_Right:
        if (inRow && i + 1 < int(row.size()))
            pickPicture(row[i + 1]);
        else if (inRow && block.next().isValid())
            caretAt(block.next().position());
        else if (!inRow)
            caretAt(p + 1);
        return true;
    case Qt::Key_Up:
        if (!inRow)
            return false;
        if (block.previous().isValid())
            caretAt(block.position() - 1);
        return true;
    case Qt::Key_Down:
        if (!inRow)
            return false;
        if (block.next().isValid())
            caretAt(block.next().position());
        return true;
    default:
        return false;
    }
}

void NoteTextEdit::caretForText()
{
    int p = pickedPicture();
    if (p < 0)
        return;
    if (!pictureRow(document()->findBlock(p)).empty()) {
        setTextCursor(newLineAfterPictures());
        return;
    }
    QTextCursor c(document());  // (one in a line of text: after it)
    c.setPosition(p + 1);
    setTextCursor(c);
}

void NoteTextEdit::inputMethodEvent(QInputMethodEvent* e)
{
    if (!isReadOnly() && (!e->commitString().isEmpty() || !e->preeditString().isEmpty()))
        caretForText();
    QTextEdit::inputMethodEvent(e);
}

// Next to pictures and in captions: Enter ends a caption (a paragraph after it, not another
// caption); Backspace and Delete pick a picture rather than join a line with its row.
bool NoteTextEdit::captionKey(QKeyEvent* e)
{
    if (isReadOnly() || (e->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier)
        return false;
    auto c = textCursor();
    if (c.hasSelection())
        return false;
    auto block = c.block();
    bool enter = e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter;
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
    if (e->key() == Qt::Key_Backspace && c.atBlockStart() && sameCell(block.previous(), block)) {
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
    if (e->key() == Qt::Key_Delete && c.atBlockEnd() && sameCell(block.next(), block) && pictureRow(block).empty()) {
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

QTextCursor NoteTextEdit::newLineAfterPictures()
{
    int p = pickedPicture();
    QTextCursor c(document());
    if (p < 0)
        return textCursor();
    auto block = document()->findBlock(p);
    auto caption = captionOf(block);
    auto after = caption.isValid() ? caption : block;
    c.setPosition(after.position() + after.length() - 1);
    c.beginEditBlock();
    c.insertBlock(textAfter(block.blockFormat()), QTextCharFormat());
    c.endEditBlock();
    return c;
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
    auto f = formatAt(document(), from);
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
    auto step = textCursor();  // (one step to undo, back to here)
    step.beginEditBlock();
    // one picked (the cursor at it): under its line, as text typed; else where the cursor is
    // (where they were dropped, say: the cursor is put there without a word)
    auto c = textCursor();
    if (int p = pickedPicture(); p >= 0 && c.position() == p && !c.hasSelection()) {
        caretForText();
        c = textCursor();
    }
    if (c.hasSelection())
        c.removeSelectedText();
    insertPictureLine(c, pictures);
    evenRow(document(), c.block());
    step.endEditBlock();
    pickPicture(c.position() - 1);
}

} // namespace on::gui
