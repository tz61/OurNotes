#include "gui/note_editor.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QPointer>
#include <QScrollBar>
#include <QStyle>
#include <QSplitter>
#include <QLabel>
#include <QFontInfo>
#include <QFontDatabase>
#include <QDesktopServices>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QMessageBox>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextList>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <utility>

#include "core/hash.h"
#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"
#include "gui/math_dialog.h"
#include "gui/math_render.h"
#include "gui/note_html.h"
#include "gui/search_field.h"
#include "gui/rich_text_images.h"
#include "gui/theme.h"
#include "gui/wrap_toolbar.h"

namespace on::gui {

// --- NoteTextEdit -----------------------------------------------------------------------

NoteTextEdit::NoteTextEdit(QWidget* parent) : QTextEdit(parent)
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setLineWrapMode(QTextEdit::WidgetWidth);
    images_ = FittedImageHandler::install(document(), this);
    images_->setColorSource([this] { return formulaColor(palette().color(QPalette::Text)); });
    preview_ = new QLabel(viewport());
    preview_->setStyleSheet("QLabel { background: palette(base); border: 1px solid palette(mid); "
                            "border-radius: 4px; padding: 6px; }");
    preview_->setAttribute(Qt::WA_TransparentForMouseEvents);
    preview_->hide();
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        if (committing_)
            return;
        if (textCursor().block().blockFormat().hasProperty(kHeadingMarkersProperty))
            revealed_ = QTextCursor(textCursor().block());  // (back in it after an undo, say)
        else if (!revealed_.isNull())
            finishHeading();
        // only leaving a formula renders it, so undo can bring back the source text
        if (editingMath_)
            commitMathEdits(false);
        editingMath_ = runAtCursor().has_value();
        updateMathPreview();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &NoteTextEdit::updateMathPreview);
    if (auto* ctx = AppContext::tryGet())
        connect(ctx, &AppContext::appearanceChanged, this, [this] { viewport()->update(); });
}

void NoteTextEdit::changeEvent(QEvent* e)
{
    QTextEdit::changeEvent(e);
    if (e->type() == QEvent::PaletteChange) {
        adaptColors();
        viewport()->update();
    }
}

// Character format for text in a heading of `level` (0: a paragraph): sizes relative to
// the note's font, as loading <h1>… gives them (so they look the same after a reload and
// follow the note's text size).
// Back to the note's own font: the format names no family. An empty family list is not
// that: Qt then picks a fallback, on macOS Apple Color Emoji, whose spaces and digits are
// three or four times as wide and whose lines are taller. Pasted text looked spread out,
// and so did what was typed after it or after `code`.
static void useNoteFont(QTextCharFormat& f)
{
    f.clearProperty(QTextFormat::FontFamilies);
    f.clearProperty(QTextFormat::FontFamily);
}

static QTextCharFormat headingCharFormat(QTextCharFormat f, int level)
{
    f.clearProperty(QTextFormat::FontPointSize);
    f.clearProperty(QTextFormat::FontPixelSize);
    if (level > 0)
        f.setProperty(QTextFormat::FontSizeAdjustment, std::max(0, 4 - level));
    else
        f.clearProperty(QTextFormat::FontSizeAdjustment);
    f.setFontWeight(level ? QFont::Bold : QFont::Normal);
    return f;
}

// Makes the cursor's paragraph a heading of `level`, or a plain paragraph (0).
static void applyHeading(QTextCursor cursor, int level)
{
    cursor.beginEditBlock();
    QTextBlockFormat bf = cursor.blockFormat();
    bf.setHeadingLevel(level);
    cursor.setBlockFormat(bf);
    auto block = cursor.block();
    std::vector<QTextFragment> fragments;
    for (auto it = block.begin(); !it.atEnd(); ++it)
        fragments.push_back(it.fragment());
    for (auto& fragment : fragments) {
        QTextCursor c(cursor.document());
        c.setPosition(fragment.position());
        c.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
        c.setCharFormat(headingCharFormat(fragment.charFormat(), level));
    }
    cursor.setBlockCharFormat(headingCharFormat(block.charFormat(), level));
    cursor.endEditBlock();
}

static QTextImageFormat mathFormat(const QString& latex, bool display)
{
    QTextImageFormat f;
    f.setName(mathResourceName(latex, display));
    f.setVerticalAlignment(QTextCharFormat::AlignMiddle);
    f.setToolTip(latex);
    return f;
}

static QString stripDelimiters(QString text, bool display)
{
    QString delim = display ? "$$" : "$";
    if (text.startsWith(delim) && text.endsWith(delim) && text.size() >= 2 * delim.size())
        return text.mid(delim.size(), text.size() - 2 * delim.size()).trimmed();
    while (text.startsWith('$'))
        text.remove(0, 1);
    while (text.endsWith('$') && !text.endsWith("\\$"))
        text.chop(1);
    return text.trimmed();
}

// Format for text around a formula: the text before it, without formula properties.
QTextCharFormat NoteTextEdit::outerFormat(const QTextBlock& block, int formulaPosInBlock) const
{
    QTextCharFormat f;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        auto frag = it.fragment();
        int pos = frag.position() - block.position();
        if (pos >= formulaPosInBlock)
            break;
        auto cf = frag.charFormat();
        if (!cf.isImageFormat() && !cf.hasProperty(kMathSourceProperty))
            f = cf;
    }
    f.clearProperty(QTextFormat::ObjectIndex);
    return f;
}

// --- formulas being edited in place -------------------------------------------------------
//
// A formula under edit is its LaTeX source between $…$ (or $$…$$), marked with the
// kMathSourceProperty char format property. Moving the cursor out renders it again.

QTextCharFormat NoteTextEdit::sourceFormat(bool display, QTextCharFormat base) const
{
    if (base.isImageFormat() || base.hasProperty(kMathSourceProperty))
        base = QTextCharFormat();
    base.setFontFamilies(QStringList{QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
    QColor tint = palette().color(QPalette::Highlight);
    tint.setAlphaF(0.2);
    base.setBackground(tint);
    base.setProperty(kMathSourceProperty, display ? 2 : 1);
    return base;
}

std::vector<NoteTextEdit::MathRun> NoteTextEdit::mathRuns() const
{
    std::vector<MathRun> runs;
    for (auto block = document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            int kind = frag.charFormat().property(kMathSourceProperty).toInt();
            if (!kind)
                continue;
            if (!runs.empty() && runs.back().end == frag.position() && runs.back().display == (kind == 2))
                runs.back().end = frag.position() + frag.length();
            else
                runs.push_back({frag.position(), frag.position() + frag.length(), kind == 2});
        }
    return runs;
}

std::optional<NoteTextEdit::MathRun> NoteTextEdit::runAtCursor() const
{
    int p = textCursor().position();
    for (auto& r : mathRuns())
        if (p > r.start && p < r.end)
            return r;
    return std::nullopt;
}

void NoteTextEdit::startMathEdit(QTextCursor cursor, const QString& latex, bool display, const QString& original)
{
    if (isReadOnly())
        return;
    commitMathEdits(true);  // one formula at a time
    pairTyped_ = false;
    if (document()->isUndoRedoEnabled())
        session_ = {document()->availableUndoSteps(), cursor.selectionStart(),
                    cursor.selectionEnd() - cursor.selectionStart(), display, original};
    auto format = sourceFormat(display, cursor.charFormat());
    QString delim = display ? "$$" : "$";
    committing_ = true;  // don't render the new formula while it is being created
    cursor.beginEditBlock();
    cursor.removeSelectedText();
    if (display) {
        // a display formula gets its own centred paragraph
        if (cursor.positionInBlock() > 0)
            cursor.insertBlock();
        QTextBlockFormat centred = cursor.blockFormat();
        centred.setAlignment(Qt::AlignHCenter);
        cursor.setBlockFormat(centred);
    }
    int start = cursor.position();
    cursor.insertText(delim + latex + delim, format);
    cursor.setPosition(start + delim.size() + latex.size());
    cursor.endEditBlock();
    setTextCursor(cursor);
    committing_ = false;
    editingMath_ = true;
    updateMathPreview();
}

// The cursor put between the signs of a formula left empty ($$|$$, or $|$ in a line): it
// opens again, to be typed into. Only when the user puts it there (a click, the arrow
// keys), not when an undo does, or undoing past it would open it again at once.
bool NoteTextEdit::reopenEmptyMath()
{
    auto cursor = textCursor();
    if (isReadOnly() || internal_ || committing_ || cursor.hasSelection() || runAtCursor())
        return false;
    auto block = cursor.block();
    QString text = block.text();
    int at = cursor.positionInBlock();
    int left = 0, right = 0;
    while (at - left > 0 && text[at - left - 1] == '$')
        ++left;
    while (at + right < text.size() && text[at + right] == '$')
        ++right;
    if (left != right || (left != 1 && left != 2) || (at - left > 0 && text[at - left - 1] == '\\'))
        return false;  // (\$ is a dollar sign)
    QTextCursor signs(document());
    signs.setPosition(block.position() + at - left);
    signs.setPosition(block.position() + at + right, QTextCursor::KeepAnchor);
    for (auto it = block.begin(); !it.atEnd(); ++it) {  // text, not a formula's source already
        auto frag = it.fragment();
        if (frag.position() < signs.selectionEnd() && frag.position() + frag.length() > signs.selectionStart() &&
            (frag.charFormat().hasProperty(kMathSourceProperty) || frag.charFormat().isImageFormat()))
            return false;
    }
    bool display = left == 2;
    commitMathEdits(true);
    pairTyped_ = false;
    if (document()->isUndoRedoEnabled())
        session_ = {document()->availableUndoSteps(), signs.selectionStart(), 2 * left, display, QString()};
    committing_ = true;
    signs.setCharFormat(sourceFormat(display, signs.charFormat()));
    committing_ = false;
    editingMath_ = true;
    updateMathPreview();
    return true;
}

void NoteTextEdit::insertMath(bool display)
{
    if (runAtCursor())
        return;
    auto cursor = textCursor();
    startMathEdit(cursor, cursor.selectedText().replace(QChar::ParagraphSeparator, ' ').trimmed(), display);
}

void NoteTextEdit::editMathInPlace(QTextCursor selection)
{
    QString latex;
    bool display = false;
    if (selection.isNull() || isReadOnly() ||
        !parseMathResourceName(selection.charFormat().toImageFormat().name(), &latex, &display))
        return;
    startMathEdit(selection, latex, display, latex);
}

void NoteTextEdit::commitMathEdits(bool all)
{
    if (committing_)
        return;
    auto runs = mathRuns();
    if (runs.empty()) {
        session_.undoSteps = -1;
        return;
    }
    committing_ = true;
    if (runs.size() == 1 && session_.undoSteps >= 0) {
        auto run = runs.front();
        int p = textCursor().position();
        int fromEnd = document()->characterCount() - p;  // the text after the formula does not change
        if (all || !(p > run.start && p < run.end)) {
            QTextCursor c(document());
            c.setPosition(run.start);
            c.setPosition(run.end, QTextCursor::KeepAnchor);
            QTextCursor after;
            if (finishMathSession(stripDelimiters(c.selectedText(), run.display), &after)) {
                // keep the cursor where the user put it
                QTextCursor user(document());
                if (p <= run.start)
                    user.setPosition(p);
                else if (p >= run.end)
                    user.setPosition(std::clamp(document()->characterCount() - fromEnd, 0, document()->characterCount() - 1));
                if (p > run.start && (p < run.end || user.position() == after.position()))
                    user = after;
                setTextCursor(user);
                committing_ = false;
                if (all)
                    editingMath_ = false;
                updateMathPreview();
                return;
            }
        }
    }
    auto user = textCursor();
    int p = user.position();
    bool after = false;  // the user's cursor ends up right after a rendered formula
    QTextCharFormat textFormat;
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        if (!all && p > it->start && p < it->end)
            continue;  // still being edited
        QTextCursor c(document());
        c.setPosition(it->start);
        c.setPosition(it->end, QTextCursor::KeepAnchor);
        QString latex = stripDelimiters(c.selectedText(), it->display);
        c.beginEditBlock();
        if (latex.isEmpty())  // its signs stay, as text (see finishMathSession)
            c.insertText(it->display ? "$$$$" : "$$", outerFormat(c.block(), c.selectionStart() - c.block().position()));
        else
            c.insertImage(mathFormat(latex, it->display));
        c.endEditBlock();
        if (p == it->end && !latex.isEmpty()) {
            after = true;
            textFormat = outerFormat(c.block(), c.positionInBlock() - 1);
        }
    }
    if (after) {
        user = textCursor();
        user.setCharFormat(textFormat);
        setTextCursor(user);
    }
    committing_ = false;
    if (all)
        editingMath_ = false;
    updateMathPreview();
}

// Renders the formula of the session as one undoable edit: back to the document as it
// was when the formula was opened, then the whole change at once. `after` gets a
// cursor right after the formula. False when the history no longer allows it (the
// session's steps were undone, or the history was cleared).
bool NoteTextEdit::finishMathSession(const QString& latex, QTextCursor* after)
{
    auto* doc = document();
    auto session = session_;
    session_.undoSteps = -1;
    // availableUndoSteps() counts commands; undo() takes back a whole edit block
    int target = session.undoSteps;
    if (target < 0 || doc->availableUndoSteps() <= target || !doc->isUndoRedoEnabled())
        return false;
    while (doc->availableUndoSteps() > target) {
        int before = doc->availableUndoSteps();
        doc->undo();
        if (doc->availableUndoSteps() >= before)
            break;
    }
    if (doc->availableUndoSteps() != target) {  // a block reached back past the session: leave it
        while (doc->isRedoAvailable())
            doc->redo();
        return false;
    }
    doc->clearUndoRedoStacks(QTextDocument::RedoStack);
    QTextCursor c(doc);
    c.setPosition(session.start);
    if (!session.original.isEmpty() && latex == session.original) {
        c.setPosition(session.start + session.length);  // opened and closed unchanged: no step at all
        *after = c;
        return true;
    }
    c.setPosition(session.start + session.length, QTextCursor::KeepAnchor);
    // Left empty, its signs stay as text ($$$$, or $$ in a line), to type the formula into
    // later: the cursor put between them opens it again (reopenEmptyMath).
    QString signs = session.display ? "$$$$" : "$$";
    if (latex.isEmpty() && c.selectedText() == signs) {
        c.setPosition(session.start + session.length);  // as they were: no step
        *after = c;
        return true;
    }
    c.beginEditBlock();
    auto textFormat = outerFormat(c.block(), c.selectionStart() - c.block().position());
    c.removeSelectedText();
    if (latex.isEmpty()) {
        c.insertText(signs, textFormat);
    } else {
        if (session.display) {
            if (c.positionInBlock() > 0)
                c.insertBlock();
            QTextBlockFormat centred = c.blockFormat();
            centred.setAlignment(Qt::AlignHCenter);
            c.setBlockFormat(centred);
        }
        c.insertImage(mathFormat(latex, session.display));
        c.setCharFormat(outerFormat(c.block(), c.positionInBlock() - 1));
    }
    c.endEditBlock();
    *after = c;
    return true;
}

void NoteTextEdit::updateMathPreview()
{
    auto run = runAtCursor();
    if (!run) {
        preview_->hide();
        return;
    }
    QTextCursor c(document());
    c.setPosition(run->start);
    c.setPosition(run->end, QTextCursor::KeepAnchor);
    QString latex = stripDelimiters(c.selectedText(), run->display);
    if (latex.isEmpty()) {
        preview_->setText(tr("Type LaTeX, e.g. \\frac{a}{b}"));
    } else {
        auto m = renderMath(latex, run->display, QFontInfo(font()).pixelSize() * 1.25,
                            formulaColor(palette().color(QPalette::Text)), devicePixelRatioF());
        preview_->setPixmap(QPixmap::fromImage(m.image));
        preview_->setToolTip(m.ok ? QString() : m.error);
    }
    preview_->adjustSize();
    QTextCursor endC(document());
    endC.setPosition(run->end);
    QRect r = cursorRect(endC);
    int x = std::clamp(r.left() - preview_->width() / 2, 4, std::max(4, viewport()->width() - preview_->width() - 4));
    int y = r.bottom() + 6;
    if (y + preview_->height() > viewport()->height()) {
        QTextCursor startC(document());
        startC.setPosition(run->start);
        y = std::max(0, cursorRect(startC).top() - preview_->height() - 6);
    }
    preview_->move(x, y);
    preview_->show();
    preview_->raise();
}

QTextCursor NoteTextEdit::mathAt(const QPoint& pos)
{
    auto hit = cursorForPosition(pos);
    for (int p : {hit.position(), hit.position() - 1}) {
        if (p < 0)
            continue;
        QTextCursor c(document());
        c.setPosition(p);
        c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        if (c.selectedText() != QString(QChar::ObjectReplacementCharacter))
            continue;
        auto f = c.charFormat();
        if (!f.isImageFormat() || !parseMathResourceName(f.toImageFormat().name(), nullptr, nullptr))
            continue;
        QTextCursor before(document()), after(document());
        before.setPosition(p);
        after.setPosition(p + 1);
        qreal left = cursorRect(before).left(), right = cursorRect(after).left();
        if (pos.x() >= std::min(left, right) - 2 && pos.x() <= std::max(left, right) + 2)
            return c;
    }
    return {};
}

bool NoteTextEdit::editMathInDialog(QTextCursor selection)
{
    QString latex;
    bool display = false;
    if (selection.isNull() || !parseMathResourceName(selection.charFormat().toImageFormat().name(), &latex, &display))
        return false;
    if (isReadOnly())
        return true;
    MathDialog dlg(latex, display, this);
    if (dlg.exec() != QDialog::Accepted)
        return true;
    if (dlg.latex().isEmpty())
        selection.removeSelectedText();
    else
        selection.insertImage(mathFormat(dlg.latex(), dlg.isDisplay()));
    return true;
}

void NoteTextEdit::prepareDocument()
{
    editingMath_ = false;
    preview_->hide();
    ++internal_;
    for (auto block = document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            auto f = frag.charFormat();
            QString latex;
            if (!f.isImageFormat() || !parseMathResourceName(f.toImageFormat().name(), &latex, nullptr))
                continue;
            if (f.verticalAlignment() == QTextCharFormat::AlignMiddle)
                continue;
            QTextCursor c(document());
            c.setPosition(frag.position());
            c.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            auto img = f.toImageFormat();
            img.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            img.setToolTip(latex);
            c.setCharFormat(img);
        }
    --internal_;
    adaptColors();
}

// Text colours that are unreadable on the current background (black text from a
// light theme in dark mode, and vice versa) are shown in the normal text colour; the
// original is kept in kOriginalForegroundProperty and written back when saving.
void NoteTextEdit::adaptColors()
{
    QColor base = palette().color(QPalette::Base);
    struct Change {
        int start, end;
        QTextCharFormat format;
    };
    std::vector<Change> changes;
    for (auto block = document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            auto f = frag.charFormat();
            if (f.isImageFormat())
                continue;
            bool hidden = f.hasProperty(kOriginalForegroundProperty);
            if (!hidden && !f.hasProperty(QTextFormat::ForegroundBrush))
                continue;
            QColor original = hidden ? f.property(kOriginalForegroundProperty).value<QColor>() : f.foreground().color();
            bool unreadable = std::abs(original.lightness() - base.lightness()) < 90;
            if (unreadable && !hidden) {
                f.setProperty(kOriginalForegroundProperty, original);
                f.clearForeground();
            } else if (!unreadable && hidden) {
                f.setForeground(original);
                f.clearProperty(kOriginalForegroundProperty);
            } else {
                continue;
            }
            changes.push_back({frag.position(), frag.position() + frag.length(), f});
        }
    if (changes.empty())
        return;
    ++internal_;
    bool modified = document()->isModified();
    bool undo = document()->isUndoRedoEnabled();
    document()->setUndoRedoEnabled(false);  // display-only adjustment, not an edit
    for (auto& ch : changes) {
        QTextCursor c(document());
        c.setPosition(ch.start);
        c.setPosition(ch.end, QTextCursor::KeepAnchor);
        c.setCharFormat(ch.format);
    }
    document()->setUndoRedoEnabled(undo);
    document()->setModified(modified);
    --internal_;
}

// Typing a $ that closes a formula turns it into one: matched up with the $$ or $ before
// it in the line, $$…$$ (or $$… and a single $) is a display formula, centred on a line of
// its own when it is the line; $…$ an inline one. Like Zotero, inline math must not start
// or end with a space, so "$5 and $10" stays text.
bool NoteTextEdit::convertTypedMath()
{
    auto cursor = textCursor();
    if (cursor.hasSelection())
        return false;
    auto block = cursor.block();
    QString before = block.text().left(cursor.positionInBlock());
    bool lineEnds = cursor.atBlockEnd();
    static const QRegularExpression displayRe(R"((^|[^\\$])\$\$([^$]*\S[^$]*)\$(\$?)$)");
    static const QRegularExpression inlineRe(R"((?:^|[^\\$])\$([^\s$](?:[^$]*[^\s$\\])?)\$$)");
    QTextCursor sel(document());
    if (auto m = displayRe.match(before); m.hasMatch()) {
        int start = int(m.capturedStart(0) + m.capturedLength(1));
        sel.setPosition(block.position() + start);
        sel.setPosition(cursor.position(), QTextCursor::KeepAnchor);
        commitAs(sel, m.captured(2).trimmed(), true, start == 0 && lineEnds);
        swallowDollar_ = m.captured(3).isEmpty();  // closed with one $: the second one typed is not text
        return true;
    }
    if (auto m = inlineRe.match(before); m.hasMatch()) {
        sel.setPosition(block.position() + m.capturedStart(1) - 1);
        sel.setPosition(cursor.position(), QTextCursor::KeepAnchor);
        commitAs(sel, m.captured(1), false);
        return true;
    }
    // "$$" starts an empty formula to type into: a display formula on an empty line,
    // an inline one anywhere else
    static const QRegularExpression pairRe(R"((?:^|[^\\$])\$\$$)");
    if (pairRe.match(before).hasMatch()) {
        sel.setPosition(cursor.position() - 2);
        sel.setPosition(cursor.position(), QTextCursor::KeepAnchor);
        bool display = before == "$$" && block.length() - 1 == 2;
        startMathEdit(sel, {}, display);
        pairTyped_ = !display;
        return true;
    }
    return false;
}

// --- Markdown -------------------------------------------------------------------------------
//
// As in Zotero's note editor: a block marker typed at the start of a paragraph, or the
// closing delimiter of an inline one, turns into formatting. It is a step of its own,
// so Ctrl+Z gives back the typed markup.

bool NoteTextEdit::convertTypedMarkdown(const QString& typed)
{
    auto cursor = textCursor();
    if (cursor.hasSelection())
        return false;
    auto block = cursor.block();
    QString before = block.text().left(cursor.positionInBlock());

    static const QRegularExpression blockRe(R"(^(#{1,3}|[-*+]|\d{1,3}[.)]|>) $)");
    if (typed == " " && markdownBlock)
        if (auto m = blockRe.match(before); m.hasMatch()) {
            auto marker = m.captured(1);
            QString kind = marker.startsWith('#')  ? QString("heading%1").arg(marker.size())
                           : marker == ">"         ? QString("quote")
                           : marker[0].isDigit()   ? QString("numbered")
                                                   : QString("bullet");
            if ((kind == "bullet" || kind == "numbered") && block.textList())
                return false;  // already a list item
            QTextCursor marked = cursor;
            marked.beginEditBlock();
            marked.setPosition(block.position(), QTextCursor::KeepAnchor);
            marked.removeSelectedText();
            markdownBlock(kind);
            marked.endEditBlock();
            return true;
        }

    // the closing delimiter was just typed: nothing but a space or punctuation before the
    // opening one, no space just inside either
    struct Inline {
        QRegularExpression re;
        int delimiter;
        std::function<void(QTextCharFormat&, bool on)> style;
    };
    static const std::vector<Inline> rules = {
        {QRegularExpression(R"((?:^|[^\w*])\*\*([^*\s](?:[^*]*[^*\s])?)\*\*$)"), 2,
         [](QTextCharFormat& f, bool on) { f.setFontWeight(on ? QFont::Bold : QFont::Normal); }},
        {QRegularExpression(R"((?:^|[^\w_])__([^_\s](?:[^_]*[^_\s])?)__$)"), 2,
         [](QTextCharFormat& f, bool on) { f.setFontWeight(on ? QFont::Bold : QFont::Normal); }},
        {QRegularExpression(R"((?:^|[^\w*])\*([^*\s](?:[^*]*[^*\s])?)\*$)"), 1,
         [](QTextCharFormat& f, bool on) { f.setFontItalic(on); }},
        {QRegularExpression(R"((?:^|[^\w_])_([^_\s](?:[^_]*[^_\s])?)_$)"), 1,
         [](QTextCharFormat& f, bool on) { f.setFontItalic(on); }},
        {QRegularExpression(R"((?:^|[^~])~~([^~\s](?:[^~]*[^~\s])?)~~$)"), 2,
         [](QTextCharFormat& f, bool on) { f.setFontStrikeOut(on); }},
        {QRegularExpression(R"((?:^|[^`])`([^`]+)`$)"), 1,
         [](QTextCharFormat& f, bool on) {
             if (on)
                 f.setFontFamilies(QStringList{QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
             else
                 useNoteFont(f);
             f.setFontFixedPitch(on);
         }},
    };
    for (auto& rule : rules) {
        auto m = rule.re.match(before);
        if (!m.hasMatch())
            continue;
        int start = block.position() + int(m.capturedStart(1)), end = block.position() + int(m.capturedEnd(1));
        QTextCursor c = cursor;  // undo puts the cursor back where the step began
        c.beginEditBlock();
        c.setPosition(end);
        c.setPosition(end + rule.delimiter, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(start - rule.delimiter);
        c.setPosition(start, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(start - rule.delimiter);
        c.setPosition(end - rule.delimiter, QTextCursor::KeepAnchor);
        QTextCharFormat on;
        rule.style(on, true);
        c.mergeCharFormat(on);
        c.endEditBlock();
        // what is typed next is plain again (set, not merged: merging cannot take a font away)
        QTextCharFormat off = currentCharFormat();
        rule.style(off, false);
        setCurrentCharFormat(off);
        return true;
    }
    static const QRegularExpression linkRe(R"(\[([^\]]+)\]\(([^)\s]+)\)$)");
    if (auto m = linkRe.match(before); m.hasMatch()) {
        QTextCursor c = cursor;
        c.beginEditBlock();
        c.setPosition(block.position() + int(m.capturedStart(0)));
        c.setPosition(block.position() + int(m.capturedEnd(0)), QTextCursor::KeepAnchor);
        QTextCharFormat f = c.charFormat();
        f.setAnchor(true);
        f.setAnchorHref(m.captured(2));
        f.setFontUnderline(true);
        f.setForeground(palette().link());
        c.insertText(m.captured(1), f);
        c.endEditBlock();
        QTextCharFormat plain = f;
        plain.setAnchor(false);
        plain.setAnchorHref({});
        plain.setFontUnderline(false);
        plain.clearForeground();
        setCurrentCharFormat(plain);
        return true;
    }
    return false;
}

// Backspace at the start of a heading shows its "## " before the text, as typed: the level
// can be changed there (a # more or less), or the heading taken back to text (the space
// after the #s deleted). Leaving the line makes it a heading again (finishHeading).
void NoteTextEdit::revealHeading(QTextCursor cursor, int level)
{
    commitMathEdits(true);
    cursor.beginEditBlock();
    applyHeading(cursor, 0);
    auto bf = cursor.blockFormat();
    bf.setProperty(kHeadingMarkersProperty, level);
    cursor.setBlockFormat(bf);
    cursor.movePosition(QTextCursor::StartOfBlock);
    auto f = cursor.charFormat();  // the first character's
    if (f.isImageFormat() || f.hasProperty(kMathSourceProperty))
        f = cursor.block().charFormat();
    cursor.insertText(QString(level, '#') + ' ', f);
    cursor.endEditBlock();
    setTextCursor(cursor);  // after the "## "
}

// The line shown as "## …" left: a heading of as many levels as #s before a space, or
// text as it is.
void NoteTextEdit::finishHeading()
{
    auto block = revealed_.block();
    revealed_ = QTextCursor();
    if (!block.isValid() || !block.blockFormat().hasProperty(kHeadingMarkersProperty) || isReadOnly())
        return;
    static const QRegularExpression markers(R"(^(#{1,6}) )");
    auto m = markers.match(block.text());
    QTextCursor c(block);
    committing_ = true;  // the cursor is moving elsewhere: no more of this on the way
    c.beginEditBlock();
    auto bf = block.blockFormat();
    bf.clearProperty(kHeadingMarkersProperty);
    c.setBlockFormat(bf);
    if (m.hasMatch()) {
        c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, int(m.capturedLength()));
        c.removeSelectedText();
        applyHeading(c, int(m.captured(1).size()));
    }
    c.endEditBlock();
    committing_ = false;
}

// Markdown text as rich text in `out`; formulas ($…$, $$…$$) become formulas.
void NoteTextEdit::markdownDocument(const QString& markdown, QTextDocument* out) const
{
    // set aside first: Markdown would read the _ and * in them as emphasis
    static const QRegularExpression math(
        R"re(\$\$([^$]*\S[^$]*)\$\$|(?<![\\$])\$([^\s$](?:[^$]*[^\s$\\])?)\$(?!\d))re");
    std::vector<std::pair<QString, bool>> formulas;
    QString text;
    qsizetype last = 0;
    for (auto it = math.globalMatch(markdown); it.hasNext();) {
        auto m = it.next();
        bool display = m.capturedLength(1) > 0;
        text += markdown.mid(last, m.capturedStart() - last);
        text += QChar(0xE000) + QString::number(formulas.size()) + QChar(0xE001);
        formulas.push_back({(display ? m.captured(1) : m.captured(2)).trimmed(), display});
        last = m.capturedEnd();
    }
    text += markdown.mid(last);
    out->setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
    static const QRegularExpression token(QString(QChar(0xE000)) + "(\\d+)" + QChar(0xE001));
    for (auto c = out->find(token); !c.isNull(); c = out->find(token, c)) {
        size_t i = c.selectedText().mid(1).chopped(1).toULong();
        if (i >= formulas.size())
            continue;
        auto [latex, display] = formulas[i];
        bool alone = c.block().text().trimmed() == c.selectedText();
        c.insertImage(mathFormat(latex, display));
        if (display && alone) {
            auto bf = c.blockFormat();
            bf.setAlignment(Qt::AlignHCenter);
            c.setBlockFormat(bf);
        }
    }
}

void NoteTextEdit::commitAs(QTextCursor selection, const QString& latex, bool display, bool ownLine)
{
    QTextCharFormat textFormat = selection.charFormat();
    if (textFormat.isImageFormat() || textFormat.hasProperty(kMathSourceProperty))
        textFormat = QTextCharFormat();
    selection.beginEditBlock();
    selection.removeSelectedText();
    if (display && ownLine) {
        QTextBlockFormat centred = selection.blockFormat();
        centred.setAlignment(Qt::AlignHCenter);
        selection.setBlockFormat(centred);
    }
    selection.insertImage(mathFormat(latex, display));
    if (display && ownLine) {
        selection.insertBlock();
        QTextBlockFormat normal = selection.blockFormat();
        normal.setAlignment(Qt::AlignLeft);
        selection.setBlockFormat(normal);
    }
    selection.setCharFormat(textFormat);
    selection.endEditBlock();
    setTextCursor(selection);
}

int NoteTextEdit::zoomKey(const QKeyEvent* e) const
{
    if (!zoomBy || !(e->modifiers() & Qt::ControlModifier) || (e->modifiers() & Qt::AltModifier))
        return -2;
    if (e->matches(QKeySequence::ZoomIn) || e->key() == Qt::Key_Equal || e->key() == Qt::Key_Plus)
        return 1;
    if (e->matches(QKeySequence::ZoomOut) || e->key() == Qt::Key_Minus)
        return -1;
    if (e->key() == Qt::Key_0 && !(e->modifiers() & Qt::ShiftModifier))
        return 0;
    return -2;
}

void NoteTextEdit::wheelEvent(QWheelEvent* e)
{
    if (!zoomBy || !(e->modifiers() & Qt::ControlModifier)) {
        QTextEdit::wheelEvent(e);  // (its own Ctrl+wheel zoom would bypass zoomBy)
        return;
    }
    wheelZoom_ += e->angleDelta().y();
    for (; wheelZoom_ >= 120; wheelZoom_ -= 120)
        zoomBy(1);
    for (; wheelZoom_ <= -120; wheelZoom_ += 120)
        zoomBy(-1);
    e->accept();
}

void NoteTextEdit::keyPressEvent(QKeyEvent* e)
{
    if (int zoom = zoomKey(e); zoom != -2) {
        zoomBy(zoom);
        return;
    }
    bool swallow = std::exchange(swallowDollar_, false);
    if (swallow && e->text() == "$")
        return;  // the second $ of a $$ that one $ already closed
    if (markdown_ && !isReadOnly() && e->key() == Qt::Key_Backspace && e->modifiers() == Qt::NoModifier) {
        auto cursor = textCursor();
        int level = cursor.blockFormat().headingLevel();
        if (level > 0 && !cursor.hasSelection() && cursor.atBlockStart() && !runAtCursor()) {
            revealHeading(cursor, level);
            return;
        }
    }
    if (e->key() == Qt::Key_Y && e->modifiers() == Qt::ControlModifier) {
        commitMathEdits(true);
        redo();
        return;
    }
    if (findRequested && e->matches(QKeySequence::Find)) {
        findRequested();
        return;
    }
    if (!isReadOnly()) {
        if (auto run = runAtCursor()) {
            auto cursor = textCursor();
            int closing = run->end - (run->display ? 2 : 1);
            bool leave = e->key() == Qt::Key_Escape ||
                         ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier));
            if (e->text() == "$" && cursor.position() >= closing) {
                // type over the closing delimiter; leaving the formula renders it
                if (cursor.position() + 1 >= run->end) {
                    // opened with $$ in a line (an inline formula, $…$): "$$" typed to close
                    // it is not one $ too many
                    swallowDollar_ = std::exchange(pairTyped_, false) && !run->display;
                    leaveMath(*run, run->display);
                } else {
                    cursor.movePosition(QTextCursor::NextCharacter);
                    setTextCursor(cursor);
                }
                return;
            }
            if (leave) {
                leaveMath(*run, run->display && e->key() != Qt::Key_Escape);
                return;
            }
        }
    }
    if (e->key() == Qt::Key_V && e->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && !isReadOnly()) {
        insertPlainText(QApplication::clipboard()->text());  // as it is, Markdown or not
        return;
    }
    QTextEdit::keyPressEvent(e);
    switch (e->key()) {  // the cursor moved between the signs of an empty formula
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        reopenEmptyMath();
        return;
    default:
        break;
    }
    if (e->text() == "$" && !isReadOnly() && !runAtCursor())
        convertTypedMath();
    else if (markdown_ && !e->text().isEmpty() && e->text()[0].isPrint() &&
             !(e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) && !isReadOnly() && !runAtCursor())
        convertTypedMarkdown(e->text());  // typed text only: after Ctrl+Z the markup stays
}

// Moves the cursor past a formula being edited, which renders it; after a display
// formula, nextLine continues on the following line (a new one at the end).
void NoteTextEdit::leaveMath(const MathRun& run, bool nextLine)
{
    auto cursor = textCursor();
    cursor.setPosition(run.end);
    setTextCursor(cursor);
    if (!nextLine)
        return;
    cursor = textCursor();
    if (!cursor.atBlockEnd())
        return;
    if (cursor.block().next().isValid()) {
        cursor.movePosition(QTextCursor::NextBlock);
    } else {
        auto textFormat = cursor.charFormat();
        cursor.joinPreviousEditBlock();  // part of the formula's step
        cursor.insertBlock();
        QTextBlockFormat normal = cursor.blockFormat();
        normal.setAlignment(Qt::AlignLeft);
        cursor.setBlockFormat(normal);
        cursor.setCharFormat(textFormat);
        cursor.endEditBlock();
    }
    setTextCursor(cursor);
}

// Ctrl+Y redoes (only Ctrl+Shift+Z is standard on Linux); claimed here so a
// surrounding shortcut does not take it.
bool NoteTextEdit::event(QEvent* e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        auto* k = static_cast<QKeyEvent*>(e);
        // the note's, not the window's (zoom applies to the PDF there)
        if ((k->key() == Qt::Key_Y && k->modifiers() == Qt::ControlModifier) || zoomKey(k) != -2 ||
            (findRequested && k->matches(QKeySequence::Find))) {
            e->accept();
            return true;
        }
    }
    return QTextEdit::event(e);
}

void NoteTextEdit::focusOutEvent(QFocusEvent* e)
{
    QTextEdit::focusOutEvent(e);
    if (e->reason() != Qt::PopupFocusReason)
        commitMathEdits(true);
}

void NoteTextEdit::mousePressEvent(QMouseEvent* e)
{
    pressPos_ = e->position().toPoint();
    QTextEdit::mousePressEvent(e);
}

void NoteTextEdit::contextMenuEvent(QContextMenuEvent* e)
{
    std::unique_ptr<QMenu> menu(createStandardContextMenu(e->pos()));
    auto math = mathAt(e->pos());
    auto* first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
    if (!math.isNull()) {
        QString latex;
        parseMathResourceName(math.charFormat().toImageFormat().name(), &latex, nullptr);
        auto* editAct = new QAction(tr("Edit Formula"), menu.get());
        connect(editAct, &QAction::triggered, this, [this, math] { editMathInPlace(math); });
        auto* dialogAct = new QAction(tr("Formula Editor…"), menu.get());
        connect(dialogAct, &QAction::triggered, this, [this, math] { editMathInDialog(math); });
        auto* copyAct = new QAction(tr("Copy LaTeX"), menu.get());
        connect(copyAct, &QAction::triggered, this, [latex] { QApplication::clipboard()->setText(latex); });
        for (auto* a : {editAct, dialogAct, copyAct})
            menu->insertAction(first, a);
        menu->insertSeparator(first);
    }
    if (!isReadOnly()) {
        menu->addSeparator();
        menu->addAction(tr("Insert Inline Formula"), this, [this] { insertMath(false); });
        menu->addAction(tr("Insert Display Formula"), this, [this] { insertMath(true); });
    }
    menu->exec(e->globalPos());
}

bool NoteTextEdit::canInsertFromMimeData(const QMimeData* source) const
{
    return (storeImage && source->hasImage()) || QTextEdit::canInsertFromMimeData(source);
}

void NoteTextEdit::insertFromMimeData(const QMimeData* source)
{
    // Into a formula being typed, text is its LaTeX (without $ signs of its own); into an
    // empty one, just opened with $$, the formula is done at once.
    if (auto run = runAtCursor(); run && source->hasText() && !isReadOnly()) {
        QTextCursor whole(document());
        whole.setPosition(run->start);
        whole.setPosition(run->end, QTextCursor::KeepAnchor);
        bool empty = stripDelimiters(whole.selectedText(), run->display).isEmpty();
        static const QRegularExpression lineBreaks(R"(\s*[\r\n]+\s*)");
        QString latex = source->text().replace(lineBreaks, " ").trimmed();
        while (latex.startsWith('$'))
            latex.remove(0, 1);
        while (latex.endsWith('$') && !latex.endsWith("\\$"))
            latex.chop(1);
        QTextCursor inside(document());
        inside.setPosition(run->start + 1);
        auto cursor = textCursor();
        cursor.insertText(latex.trimmed(), inside.charFormat());
        setTextCursor(cursor);
        if (empty && !latex.trimmed().isEmpty())
            if (auto now = runAtCursor())
                leaveMath(*now, now->display);
        return;
    }
    if (storeImage && source->hasImage()) {
        auto image = qvariant_cast<QImage>(source->imageData());
        if (!image.isNull()) {
            auto name = storeImage(image);
            if (!name.isEmpty()) {
                document()->addResource(QTextDocument::ImageResource, QUrl(name), image);
                QTextImageFormat fmt;
                fmt.setName(name);
                qreal maxWidth = std::max(100.0, viewport()->width() - 40.0);
                if (image.width() > maxWidth) {
                    fmt.setWidth(maxWidth);
                    fmt.setHeight(image.height() * maxWidth / image.width());
                }
                textCursor().insertImage(fmt);
                return;
            }
        }
    }
    if (storeImage && source->hasUrls()) {
        for (auto& url : source->urls()) {
            if (!url.isLocalFile() || QImageReader::imageFormat(url.toLocalFile()).isEmpty())
                continue;
            QMimeData img;
            img.setImageData(QImage(url.toLocalFile()));
            insertFromMimeData(&img);
        }
        return;
    }
    static const QRegularExpression markdownRe(
        R"(^(#{1,6} |\s*[-*+] |\s*\d+[.)] |> |```)|\*\*[^*\n]+\*\*|__[^_\n]+__|`[^`\n]+`|\[[^\]\n]+\]\([^)\s]+\))",
        QRegularExpression::MultilineOption);
    if (markdown_ && !source->hasHtml() && source->hasText() && markdownRe.match(source->text()).hasMatch()) {
        QTextDocument pasted;
        markdownDocument(source->text(), &pasted);
        auto cursor = textCursor();
        bool emptyLine = cursor.block().length() <= 1;
        int at = cursor.position();
        cursor.beginEditBlock();
        cursor.insertFragment(QTextDocumentFragment(&pasted));
        if (emptyLine) {  // the first paragraph went into this one, which kept its own format
            QTextCursor first(document());
            first.setPosition(at);
            first.setBlockFormat(pasted.begin().blockFormat());
        }
        cursor.endEditBlock();
        setTextCursor(cursor);
        return;
    }
    // keep pasted rich text but not foreign fonts and sizes; relative sizes (headings)
    // follow the note's text size
    if (source->hasHtml()) {
        QTextDocument tmp;
        tmp.setHtml(source->html());
        std::vector<QTextFragment> fragments;
        for (auto block = tmp.begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                fragments.push_back(it.fragment());
        for (auto& fragment : fragments) {
            auto f = fragment.charFormat();
            useNoteFont(f);
            f.clearProperty(QTextFormat::FontPointSize);
            f.clearProperty(QTextFormat::FontPixelSize);
            QTextCursor c(&tmp);
            c.setPosition(fragment.position());
            c.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            c.setCharFormat(f);
        }
        textCursor().insertFragment(QTextDocumentFragment(&tmp));
        return;
    }
    QTextEdit::insertFromMimeData(source);
}

void NoteTextEdit::mouseReleaseEvent(QMouseEvent* e)
{
    auto href = anchorAt(e->pos());
    if (!href.isEmpty() && (e->modifiers() & Qt::ControlModifier || isReadOnly())) {
        QDesktopServices::openUrl(QUrl(href));
        return;
    }
    QTextEdit::mouseReleaseEvent(e);
    // a click (not a drag) on a formula opens it for editing right here
    if (e->button() == Qt::LeftButton && (e->position().toPoint() - pressPos_).manhattanLength() < 4) {
        auto math = mathAt(e->position().toPoint());
        if (!math.isNull())
            editMathInPlace(math);
        else
            reopenEmptyMath();
    }
}

void NoteTextEdit::mouseMoveEvent(QMouseEvent* e)
{
    bool overLink = !anchorAt(e->pos()).isEmpty() && (e->modifiers() & Qt::ControlModifier);
    bool overMath = !isReadOnly() && !mathAt(e->pos()).isNull();
    viewport()->setCursor(overLink || overMath ? Qt::PointingHandCursor : Qt::IBeamCursor);
    if (overMath)
        viewport()->setToolTip(tr("Click to edit the formula"));
    QTextEdit::mouseMoveEvent(e);
}

// --- NoteEditor ---------------------------------------------------------------------------

NoteEditor::NoteEditor(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    toolbar_ = new WrapToolBar(this);  // wraps onto more rows when the pane is narrow
    edit_ = new NoteTextEdit(this);
    edit_->setAcceptRichText(true);
    edit_->setPlaceholderText(tr("Write a note…"));
    edit_->storeImage = [this](const QImage& img) { return storeImage(img); };
    edit_->setMarkdownShortcuts(true);
    edit_->markdownBlock = [this](const QString& kind) {
        if (kind.startsWith("heading"))
            setHeading(kind.mid(7).toInt());
        else if (kind == "bullet" || kind == "numbered")
            toggleList(kind == "numbered");
        else if (kind == "quote" && edit_->textCursor().blockFormat().leftMargin() < 20)
            toggleQuote();
        updateToolbar();
    };
    edit_->document()->setDocumentMargin(10);
    layout->addWidget(toolbar_);
    layout->addWidget(edit_, 1);
    buildFindBar();
    layout->addWidget(findBar_);
    edit_->findRequested = [this] { openFind(); };

    blockStyle_ = new QComboBox(this);
    blockStyle_->addItems({tr("Paragraph"), tr("Heading 1"), tr("Heading 2"), tr("Heading 3")});
    blockStyle_->setFocusPolicy(Qt::NoFocus);
    connect(blockStyle_, &QComboBox::activated, this, [this](int i) { setHeading(i); });
    toolbar_->addWidget(blockStyle_);

    auto fmtAction = [this](const QString& text, const QString& tip, QKeySequence key, auto apply) {
        auto* a = toolbar_->addAction(text);
        a->setToolTip(key.isEmpty() ? tip : tip + " (" + key.toString(QKeySequence::NativeText) + ")");
        a->setCheckable(true);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(a);  // its shortcut works in the text too, not just on the toolbar
        connect(a, &QAction::triggered, this, [this, apply](bool on) {
            QTextCharFormat f;
            apply(f, on);
            edit_->mergeCurrentCharFormat(f);
        });
        return a;
    };
    boldAct_ = fmtAction("B", tr("Bold"), QKeySequence::Bold,
                         [](QTextCharFormat& f, bool on) { f.setFontWeight(on ? QFont::Bold : QFont::Normal); });
    italicAct_ = fmtAction("I", tr("Italic"), QKeySequence::Italic, [](QTextCharFormat& f, bool on) { f.setFontItalic(on); });
    underlineAct_ = fmtAction("U", tr("Underline"), QKeySequence::Underline,
                              [](QTextCharFormat& f, bool on) { f.setFontUnderline(on); });
    strikeAct_ = fmtAction("S", tr("Strikethrough"), QKeySequence(),
                           [](QTextCharFormat& f, bool on) { f.setFontStrikeOut(on); });
    for (auto* a : {boldAct_, italicAct_, underlineAct_, strikeAct_}) {
        QFont f = toolbar_->font();
        if (a == boldAct_)
            f.setBold(true);
        if (a == italicAct_)
            f.setItalic(true);
        if (a == underlineAct_)
            f.setUnderline(true);
        if (a == strikeAct_)
            f.setStrikeOut(true);
        a->setFont(f);
    }
    toolbar_->addSeparator();
    toolbar_->addAction("•", this, [this] { toggleList(false); })->setToolTip(tr("Bulleted list"));
    toolbar_->addAction("1.", this, [this] { toggleList(true); })->setToolTip(tr("Numbered list"));
    toolbar_->addAction("❝", this, [this] { toggleQuote(); })->setToolTip(tr("Block quote"));
    toolbar_->addAction(tr("Link"), this, [this] { insertLink(); })->setToolTip(tr("Insert link (Ctrl+K)"));
    auto* mathBtn = new QToolButton(this);
    mathBtn->setText(QString::fromUtf8("\u2211"));
    mathBtn->setToolTip(tr("Insert formula: Ctrl+M inline, Ctrl+Shift+M display; or type $…$ / $$…$$"));
    mathBtn->setPopupMode(QToolButton::InstantPopup);
    auto* mathMenu = new QMenu(mathBtn);
    mathMenu->addAction(tr("Inline Formula ($…$)"), this, [this] { edit_->insertMath(false); });
    mathMenu->addAction(tr("Display Formula ($$…$$)"), this, [this] { edit_->insertMath(true); });
    mathBtn->setMenu(mathMenu);
    toolbar_->addWidget(mathBtn);
    for (auto [key, display] : {std::pair{"Ctrl+M", false}, std::pair{"Ctrl+Shift+M", true}}) {
        auto* a = new QAction(this);
        a->setShortcut(QKeySequence(key));
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(a, &QAction::triggered, this, [this, display = display] { edit_->insertMath(display); });
        addAction(a);
    }
    auto* link = new QAction(this);
    link->setShortcut(QKeySequence("Ctrl+K"));
    link->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(link, &QAction::triggered, this, [this] { insertLink(); });
    addAction(link);

    auto* colorBtn = new QToolButton(this);
    colorBtn->setText("A");
    colorBtn->setToolTip(tr("Text and highlight colour"));
    colorBtn->setPopupMode(QToolButton::InstantPopup);
    auto* colorMenu = new QMenu(colorBtn);
    for (auto& c : schema::kAnnotationColors) {
        colorMenu->addAction(colorIcon(c.hex), tr("Text: %1").arg(c.name), this,
                             [this, hex = QString(c.hex)] { setTextColor(QColor(hex), false); });
    }
    colorMenu->addSeparator();
    for (auto& c : schema::kAnnotationColors) {
        QColor bg(c.hex);
        bg.setAlphaF(0.5);
        colorMenu->addAction(colorIcon(c.hex, 14, false), tr("Highlight: %1").arg(c.name), this,
                             [this, hex = QString(c.hex)] { setTextColor(QColor(hex), true); });
    }
    colorMenu->addSeparator();
    colorMenu->addAction(tr("Clear formatting"), this, [this] {
        auto cursor = edit_->textCursor();
        cursor.setCharFormat(QTextCharFormat());
        edit_->setTextCursor(cursor);
    });
    colorBtn->setMenu(colorMenu);
    toolbar_->addWidget(colorBtn);

    // text size of this view of the note
    toolbar_->addSeparator();
    auto zoomButton = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        toolbar_->addWidget(b);
        return b;
    };
    connect(zoomButton(QString::fromUtf8("\u2212"), tr("Smaller text (Ctrl+-)")), &QToolButton::clicked, this,
            [this] { zoomBy(-1); });
    zoomButton_ = zoomButton({}, tr("Text size; click for the default from the preferences (Ctrl+0)"));
    zoomButton_->setObjectName("noteZoom");
    connect(zoomButton_, &QToolButton::clicked, this, [this] { zoomBy(0); });
    connect(zoomButton("+", tr("Larger text (Ctrl+=)")), &QToolButton::clicked, this, [this] { zoomBy(1); });
    edit_->zoomBy = [this](int steps) { zoomBy(steps); };
    baseFont_ = edit_->font();
    defaultZoom_ = defaultZoom();
    setZoom(defaultZoom_);
    connect(&AppContext::get(), &AppContext::appearanceChanged, this, [this] {
        if (int d = defaultZoom(); d != defaultZoom_) {  // the default was changed: every note follows
            defaultZoom_ = d;
            setZoom(d);
        }
    });

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(700);
    connect(&saveTimer_, &QTimer::timeout, this, &NoteEditor::save);
    connect(edit_->document(), &QTextDocument::contentsChanged, this, [this] {
        if (!loading_ && !key_.isEmpty() && !edit_->isInternalEdit())
            saveTimer_.start();
    });
    connect(edit_, &QTextEdit::cursorPositionChanged, this, &NoteEditor::updateToolbar);
    connect(edit_, &QTextEdit::currentCharFormatChanged, this, &NoteEditor::updateToolbar);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString& action, const QStringList& keys) {
                // (0: every library, after a sync)
                if (key_.isEmpty() || (lib != libraryId_ && lib != 0) || (type != "item" && type != "*"))
                    return;
                if (type == "*" || keys.contains(key_)) {
                    if (action == "delete") {
                        clear();
                        return;
                    }
                    if (!saveTimer_.isActive())
                        load(false);  // changed elsewhere (sync, another editor)
                }
            });
    setEnabled(false);
}

NoteEditor::~NoteEditor() { flush(); }

QSize NoteEditor::minimumSizeHint() const
{
    QSize size = QWidget::minimumSizeHint();
    if (findBar_->isHidden()) {  // as with the find bar
        auto bar = findBar_->minimumSizeHint();
        size.rheight() += bar.height();
        size.setWidth(std::max(size.width(), bar.width()));
    }
    return size;
}

void NoteEditor::buildFindBar()
{
    findBar_ = new QWidget(this);
    findBar_->setObjectName("noteFindBar");
    auto* h = new QHBoxLayout(findBar_);
    h->setContentsMargins(6, 3, 6, 3);
    h->setSpacing(2);
    find_ = new SearchField(findBar_, "/notes/findHistory");
    find_->setObjectName("findInNote");
    find_->setPlaceholderText(tr("Find in note"));
    find_->setClearButtonEnabled(true);
    find_->addAction(icon("search"), QLineEdit::LeadingPosition);
    find_->addCaseToggle("/notes/findCaseSensitive");
    // As wide at least as with the clear button, which shows only with text (Qt gives each
    // icon at the end iconSize + 6, and a margin): each key changed the bar's minimum width,
    // the window's with it, and the window moved its edge.
    auto* style = find_->style();
    find_->setMinimumWidth(find_->minimumSizeHint().width() +
                           style->pixelMetric(QStyle::PM_LineEditIconSize, nullptr, find_) + 6 +
                           style->pixelMetric(QStyle::PM_LineEditIconMargin, nullptr, find_));
    h->addWidget(find_, 1);
    auto button = [&](const char* ic, const QString& tip) {
        auto* b = new QToolButton(findBar_);
        b->setIcon(icon(ic));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        h->addWidget(b);
        return b;
    };
    connect(button("up", tr("Previous (Shift+Enter)")), &QToolButton::clicked, this, [this] { findAgain(true); });
    connect(button("down", tr("Next (Enter)")), &QToolButton::clicked, this, [this] { findAgain(false); });
    findStatus_ = new QLabel(findBar_);
    findStatus_->setObjectName("noteFindStatus");
    auto fm = findStatus_->fontMetrics();  // room for the longest it says, also when empty
    findStatus_->setMinimumWidth(std::max(fm.horizontalAdvance(tr("No results")), fm.horizontalAdvance(tr("%1 of %2").arg(999).arg(999))));
    h->addWidget(findStatus_);
    connect(button("close", tr("Close (Esc)")), &QToolButton::clicked, this, [this] { closeFind(true); });
    findBar_->hide();

    connect(find_, &QLineEdit::textChanged, this, [this] { updateHits(true); });
    connect(find_, &QLineEdit::returnPressed, this, [this] { findAgain(false); });
    connect(find_, &SearchField::shiftReturnPressed, this, [this] { findAgain(true); });
    connect(find_, &SearchField::caseSensitivityChanged, this, [this] { updateHits(true); });
    connect(find_, &SearchField::escapePressed, this, [this] { closeFind(true); });
    // the note changed under the hits (typed in, or loaded)
    connect(edit_->document(), &QTextDocument::contentsChanged, this, [this] {
        if (!findBar_->isHidden())
            updateHits(false);
    });
}

void NoteEditor::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape && findBar_->isVisible()) {
        closeFind(true);
        return;
    }
    QWidget::keyPressEvent(e);  // (Esc on to the page or the item list)
}

void NoteEditor::openFind()
{
    findBar_->show();
    find_->setFocus();
    find_->selectAll();
    updateHits(true);
}

void NoteEditor::closeFind(bool focusNote)
{
    if (!findBar_->isVisible())
        return;
    find_->clear();
    hits_.clear();
    hit_ = -1;
    markHits();
    findBar_->hide();
    if (focusNote)
        edit_->setFocus();
}

void NoteEditor::updateHits(bool fromCursor)
{
    hits_.clear();
    auto text = find_->text();
    if (!text.isEmpty()) {
        QTextDocument::FindFlags flags;
        if (find_->caseSensitive())
            flags |= QTextDocument::FindCaseSensitively;
        for (auto c = edit_->document()->find(text, 0, flags); !c.isNull(); c = edit_->document()->find(text, c, flags))
            hits_.push_back(c);
    }
    if (hits_.empty()) {
        hit_ = -1;
    } else if (fromCursor) {  // the first at the cursor or after it, as typing goes on
        int at = edit_->textCursor().selectionStart();
        hit_ = 0;
        for (int i = 0; i < (int)hits_.size(); ++i)
            if (hits_[i].selectionStart() >= at) {
                hit_ = i;
                break;
            }
        edit_->setTextCursor(hits_[hit_]);
    } else {
        hit_ = std::clamp(hit_, 0, int(hits_.size()) - 1);
    }
    markHits();
}

void NoteEditor::findAgain(bool backwards)
{
    if (hits_.empty())
        updateHits(true);
    if (hits_.empty())
        return;
    int n = int(hits_.size());
    hit_ = ((hit_ + (backwards ? -1 : 1)) % n + n) % n;
    edit_->setTextCursor(hits_[hit_]);
    find_->remember(find_->text());
    markHits();
}

// all hits marked as a PDF's are, the current one more
void NoteEditor::markHits()
{
    QList<QTextEdit::ExtraSelection> marks;
    for (int i = 0; i < (int)hits_.size(); ++i) {
        QTextEdit::ExtraSelection m;
        m.cursor = hits_[i];
        m.format.setBackground(i == hit_ ? QColor(255, 140, 0, 150) : QColor(255, 220, 0, 110));
        marks << m;
    }
    edit_->setExtraSelections(marks);
    findStatus_->setText(find_->text().isEmpty() ? QString()
                         : hits_.empty()         ? tr("No results")
                                                 : tr("%1 of %2").arg(hit_ + 1).arg(hits_.size()));
}

void NoteEditor::setNote(int64_t libraryId, const QString& key)
{
    if (libraryId == libraryId_ && key == key_)
        return;
    closeFind(false);
    flush();
    libraryId_ = libraryId;
    key_ = key;
    load();
}

void NoteEditor::clear()
{
    closeFind(false);
    flush();
    key_.clear();
    libraryId_ = 0;
    loading_ = true;
    edit_->clear();
    loading_ = false;
    setEnabled(false);
}

void NoteEditor::load(bool force)
{
    auto data = AppContext::get().library().item(libraryId_, ss(key_));
    if (!data) {
        clear();
        return;
    }
    auto html = jqs(*data, "note");
    if (!force && (html == lastSaved_ || html == lastSerialized_))
        return;
    loading_ = true;
    int pos = edit_->textCursor().position();
    int scroll = edit_->verticalScrollBar()->value();
    edit_->document()->clear();
    edit_->setHtml(noteHtmlForEditor(html, libraryId_, edit_->document()));
    edit_->prepareDocument();
    edit_->document()->clearUndoRedoStacks();
    edit_->document()->setModified(false);
    lastSerialized_ = documentToNoteHtml(edit_->document());
    auto cursor = edit_->textCursor();
    cursor.setPosition(std::min(pos, edit_->document()->characterCount() - 1));
    edit_->setTextCursor(cursor);
    if (!force) {  // changed elsewhere, while being read here: it stays where it was
        edit_->verticalScrollBar()->setValue(scroll);
        if (findBar_->isVisible())
            updateHits(false);
    }
    lastSaved_ = html;
    loading_ = false;
    bool editable = AppContext::get().editable(libraryId_);
    edit_->setReadOnly(!editable);
    toolbar_->setEnabled(editable);
    setEnabled(true);
}

void NoteEditor::save()
{
    saveTimer_.stop();
    if (key_.isEmpty() || !edit_->document()->isModified())
        return;
    auto html = documentToNoteHtml(edit_->document());
    if (html == lastSaved_ || html == lastSerialized_)
        return;
    lastSaved_ = lastSerialized_ = html;
    edit_->document()->setModified(false);
    auto& lib = AppContext::get().library();
    lib.updateItem(libraryId_, ss(key_), [&](Json& d) { d["note"] = ss(html); });
    emit saved(key_);
}

void NoteEditor::flush()
{
    if (saveTimer_.isActive())
        save();
}

void NoteEditor::appendHtml(const QString& html)
{
    auto cursor = edit_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertBlock();
    cursor.insertHtml(html);
    edit_->setTextCursor(cursor);
    edit_->document()->setModified(true);
    saveTimer_.start();
}

void NoteEditor::focusEditor() { edit_->setFocus(); }

namespace {
constexpr int kZoomSteps[] = {50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300};
}

int NoteEditor::defaultZoom()
{
    auto v = AppContext::get().config().get("/notes/zoom", 100);
    return std::clamp(v.is_number() ? v.get<int>() : 100, kZoomSteps[0], std::end(kZoomSteps)[-1]);
}

void NoteEditor::setZoom(int percent)
{
    zoom_ = std::clamp(percent, kZoomSteps[0], std::end(kZoomSteps)[-1]);
    // the document's default font: every size in a note is relative to it
    QFont f = baseFont_;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(baseFont_.pointSizeF() * zoom_ / 100.0);
    else
        f.setPixelSize(std::max(1, int(std::lround(baseFont_.pixelSize() * zoom_ / 100.0))));
    edit_->setFont(f);
    edit_->setTabStopDistance(4 * QFontMetricsF(f).horizontalAdvance(' '));  // a tab is four spaces wide
    zoomButton_->setText(QString("%1%").arg(zoom_));
}

void NoteEditor::zoomBy(int steps)
{
    if (steps == 0) {
        setZoom(defaultZoom_);
        return;
    }
    int next = zoom_;
    if (steps > 0) {
        for (int s : kZoomSteps)
            if (s > zoom_) {
                next = s;
                break;
            }
    } else {
        for (int s : kZoomSteps)
            if (s < zoom_)
                next = s;
    }
    setZoom(next);
}

void NoteEditor::setHeading(int level)
{
    applyHeading(edit_->textCursor(), level);
    edit_->setCurrentCharFormat(headingCharFormat(edit_->currentCharFormat(), level));
}

void NoteEditor::toggleList(bool ordered)
{
    auto cursor = edit_->textCursor();
    auto style = ordered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc;
    if (auto* list = cursor.currentList(); list && list->format().style() == style) {
        // remove from list
        for (int i = list->count() - 1; i >= 0; --i)
            if (list->item(i) == cursor.block())
                list->remove(cursor.block());
        QTextBlockFormat bf = cursor.blockFormat();
        bf.setIndent(0);
        cursor.setBlockFormat(bf);
        return;
    }
    QTextListFormat lf;
    lf.setStyle(style);
    cursor.createList(lf);
}

void NoteEditor::toggleQuote()
{
    auto cursor = edit_->textCursor();
    QTextBlockFormat bf = cursor.blockFormat();
    bool quoted = bf.leftMargin() >= 20;
    bf.setLeftMargin(quoted ? 0 : 24);
    bf.setRightMargin(quoted ? 0 : 12);
    cursor.setBlockFormat(bf);
}

void NoteEditor::insertLink()
{
    auto cursor = edit_->textCursor();
    bool ok = false;
    auto current = cursor.charFormat().anchorHref();
    auto url = QInputDialog::getText(this, tr("Insert Link"), tr("URL:"), QLineEdit::Normal,
                                     current.isEmpty() ? "https://" : current, &ok);
    if (!ok)
        return;
    QTextCharFormat f;
    if (url.trimmed().isEmpty() || url == "https://") {
        f.setAnchor(false);
        f.setAnchorHref({});
        f.setFontUnderline(false);
        f.clearForeground();
    } else {
        f.setAnchor(true);
        f.setAnchorHref(url.trimmed());
        f.setFontUnderline(true);
        f.setForeground(palette().link());
    }
    if (!cursor.hasSelection()) {
        if (url.trimmed().isEmpty())
            return;
        cursor.insertText(url.trimmed(), f);
    } else {
        cursor.mergeCharFormat(f);
    }
}

void NoteEditor::setTextColor(const QColor& c, bool background)
{
    QTextCharFormat f;
    if (background) {
        QColor bg = c;
        bg.setAlphaF(0.5);
        f.setBackground(bg);
    } else {
        f.setForeground(c);
    }
    edit_->mergeCurrentCharFormat(f);
}

void NoteEditor::updateToolbar()
{
    auto f = edit_->currentCharFormat();
    auto bf = edit_->textCursor().blockFormat();
    QSignalBlocker b1(boldAct_), b2(italicAct_), b3(underlineAct_), b4(strikeAct_), b5(blockStyle_);
    boldAct_->setChecked(f.fontWeight() >= QFont::Bold);
    italicAct_->setChecked(f.fontItalic());
    underlineAct_->setChecked(f.fontUnderline());
    strikeAct_->setChecked(f.fontStrikeOut());
    blockStyle_->setCurrentIndex(std::min(3, std::max(0, bf.headingLevel())));
}

QString NoteEditor::storeImage(const QImage& image)
{
    if (key_.isEmpty())
        return {};
    auto& lib = AppContext::get().library();
    auto key = newKey();
    auto dir = lib.storageDir() / key;
    fs::create_directories(dir);
    auto file = dir / "image.png";
    if (!image.save(qpath(file), "PNG"))
        return {};
    Json att{{"key", key},
             {"itemType", "attachment"},
             {"linkMode", "embedded_image"},
             {"parentItem", ss(key_)},
             {"contentType", "image/png"},
             {"filename", "image.png"},
             {"md5", md5File(file)},
             {"mtime", QFileInfo(qpath(file)).lastModified().toMSecsSinceEpoch()}};
    lib.createItem(libraryId_, att);
    return "attachment:" + qs(key);
}

namespace {

std::vector<QPointer<QSplitter>>& notesSplits()
{
    static std::vector<QPointer<QSplitter>> splits;
    return splits;
}

void setListHeight(QSplitter* split, int list)
{
    int total = std::max(split->height(), list + 200);  // not laid out yet: in proportion
    QSignalBlocker block(split);
    split->setSizes({list, std::max(1, total - list)});
}

} // namespace

void shareNotesSplit(QSplitter* split)
{
    split->setStretchFactor(0, 0);  // more room goes to the note
    split->setStretchFactor(1, 1);
    split->setCollapsible(1, false);
    setListHeight(split, AppContext::get().config().get("/ui/notesListHeight", 120).get<int>());
    auto& splits = notesSplits();
    splits.erase(std::remove_if(splits.begin(), splits.end(), [](auto& s) { return s.isNull(); }), splits.end());
    splits.push_back(split);
    QObject::connect(split, &QSplitter::splitterMoved, split, [split] {
        auto sizes = split->sizes();
        if (sizes.size() < 2 || sizes[0] <= 0)
            return;  // the list hidden (a single note): nothing to share
        AppContext::get().config().set("/ui/notesListHeight", sizes[0]);
        for (auto& other : notesSplits())
            if (other && other != split)
                setListHeight(other, sizes[0]);
    });
}

void enableNoteTrash(QListWidget* list, std::function<int64_t()> libraryId)
{
    auto trash = [list, libraryId](const QString& key, const QString& title) {
        int64_t lib = libraryId();
        if (key.isEmpty() || !AppContext::get().editable(lib))
            return;
        QMessageBox box(QMessageBox::Question, QObject::tr("Move to Trash"),
                        QObject::tr("Move the note \"%1\" to the trash?").arg(title), QMessageBox::NoButton, list);
        box.setObjectName("confirmTrashNote");
        auto* move = box.addButton(QObject::tr("Move to Trash"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(move);
        box.exec();
        if (box.clickedButton() == static_cast<QAbstractButton*>(move))
            AppContext::get().library().trashItems(lib, {ss(key)});
    };
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(list, &QWidget::customContextMenuRequested, list, [list, libraryId, trash](const QPoint& pos) {
        auto* item = list->itemAt(pos);
        if (!item || !AppContext::get().editable(libraryId()))
            return;
        QMenu menu(list);
        menu.addAction(icon("trash"), QObject::tr("Move Note to Trash…"), list,
                       [trash, key = item->data(Qt::UserRole).toString(), title = item->text()] { trash(key, title); });
        menu.exec(list->viewport()->mapToGlobal(pos));
    });
    // Delete (or Backspace) on the selected note
    struct Keys : QObject {
        std::function<void()> onDelete;
        using QObject::QObject;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() != QEvent::KeyPress)
                return false;
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->matches(QKeySequence::Delete) || k->key() == Qt::Key_Backspace) {
                onDelete();
                return true;
            }
            return false;
        }
    };
    auto* keys = new Keys(list);
    keys->onDelete = [list, trash] {
        if (auto* item = list->currentItem())
            trash(item->data(Qt::UserRole).toString(), item->text());
    };
    list->installEventFilter(keys);
}

} // namespace on::gui
