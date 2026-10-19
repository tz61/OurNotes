#include "gui/note_editor.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QScrollBar>
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

#include "core/hash.h"
#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"
#include "gui/math_dialog.h"
#include "gui/math_render.h"
#include "gui/note_html.h"
#include "gui/rich_text_images.h"
#include "gui/theme.h"
#include "gui/wrap_toolbar.h"

namespace fp::gui {

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

void NoteTextEdit::startMathEdit(QTextCursor cursor, const QString& latex, bool display)
{
    if (isReadOnly())
        return;
    commitMathEdits(true);  // one formula at a time
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
    startMathEdit(selection, latex, display);
}

void NoteTextEdit::commitMathEdits(bool all)
{
    if (committing_)
        return;
    auto runs = mathRuns();
    if (runs.empty())
        return;
    committing_ = true;
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
        if (latex.isEmpty())
            c.removeSelectedText();
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

// Typing the closing $ of $…$ (or $$…$$ on its own line) turns it into a formula.
// Like Zotero, inline math must not start or end with a space, so "$5 and $10" stays text.
bool NoteTextEdit::convertTypedMath()
{
    auto cursor = textCursor();
    if (cursor.hasSelection())
        return false;
    auto block = cursor.block();
    QString before = block.text().left(cursor.positionInBlock());
    static const QRegularExpression displayRe(R"(^\$\$([^$]*\S[^$]*)\$\$$)");
    static const QRegularExpression inlineRe(R"((?:^|[^\\$])\$([^\s$](?:[^$]*[^\s$\\])?)\$$)");
    QTextCursor sel(document());
    if (auto m = displayRe.match(before); m.hasMatch()) {
        sel.setPosition(block.position());
        sel.setPosition(cursor.position(), QTextCursor::KeepAnchor);
        commitAs(sel, m.captured(1).trimmed(), true);
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
        return true;
    }
    return false;
}

void NoteTextEdit::commitAs(QTextCursor selection, const QString& latex, bool display)
{
    QTextCharFormat textFormat = selection.charFormat();
    if (textFormat.isImageFormat() || textFormat.hasProperty(kMathSourceProperty))
        textFormat = QTextCharFormat();
    selection.beginEditBlock();
    selection.removeSelectedText();
    if (display) {
        QTextBlockFormat centred = selection.blockFormat();
        centred.setAlignment(Qt::AlignHCenter);
        selection.setBlockFormat(centred);
    }
    selection.insertImage(mathFormat(latex, display));
    if (display) {
        selection.insertBlock();
        QTextBlockFormat normal = selection.blockFormat();
        normal.setAlignment(Qt::AlignLeft);
        selection.setBlockFormat(normal);
    }
    selection.setCharFormat(textFormat);
    selection.endEditBlock();
    setTextCursor(selection);
}

void NoteTextEdit::keyPressEvent(QKeyEvent* e)
{
    if (!isReadOnly()) {
        if (auto run = runAtCursor()) {
            auto cursor = textCursor();
            int closing = run->end - (run->display ? 2 : 1);
            bool leave = e->key() == Qt::Key_Escape ||
                         ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier));
            if (e->text() == "$" && cursor.position() >= closing) {
                // type over the closing delimiter; leaving the formula renders it
                if (cursor.position() + 1 >= run->end)
                    leaveMath(*run, run->display);
                else {
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
    QTextEdit::keyPressEvent(e);
    if (e->text() == "$" && !isReadOnly() && !runAtCursor())
        convertTypedMath();
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
        cursor.insertBlock();
        QTextBlockFormat normal = cursor.blockFormat();
        normal.setAlignment(Qt::AlignLeft);
        cursor.setBlockFormat(normal);
        cursor.setCharFormat(textFormat);
    }
    setTextCursor(cursor);
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
    // keep pasted rich text but not foreign fonts/sizes
    if (source->hasHtml()) {
        QTextDocument tmp;
        tmp.setHtml(source->html());
        QTextCursor all(&tmp);
        all.select(QTextCursor::Document);
        QTextCharFormat reset;
        reset.setFontFamilies(QStringList{});
        reset.setFontPointSize(document()->defaultFont().pointSizeF());
        all.mergeCharFormat(reset);
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
    edit_->document()->setDocumentMargin(10);
    layout->addWidget(toolbar_);
    layout->addWidget(edit_, 1);

    blockStyle_ = new QComboBox(this);
    blockStyle_->addItems({tr("Paragraph"), tr("Heading 1"), tr("Heading 2"), tr("Heading 3")});
    blockStyle_->setFocusPolicy(Qt::NoFocus);
    connect(blockStyle_, &QComboBox::activated, this, [this](int i) { setHeading(i); });
    toolbar_->addWidget(blockStyle_);

    auto fmtAction = [this](const QString& text, const QString& tip, QKeySequence key, auto apply) {
        auto* a = toolbar_->addAction(text);
        a->setToolTip(tip);
        a->setCheckable(true);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
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
                if (key_.isEmpty() || lib != libraryId_ || (type != "item" && type != "*"))
                    return;
                if (type == "*" || keys.contains(key_)) {
                    if (action == "delete") {
                        clear();
                        return;
                    }
                    if (!saveTimer_.isActive())
                        load();  // changed elsewhere (sync, another editor)
                }
            });
    setEnabled(false);
}

NoteEditor::~NoteEditor() { flush(); }

void NoteEditor::setNote(int64_t libraryId, const QString& key)
{
    if (libraryId == libraryId_ && key == key_)
        return;
    flush();
    libraryId_ = libraryId;
    key_ = key;
    load();
}

void NoteEditor::clear()
{
    flush();
    key_.clear();
    libraryId_ = 0;
    loading_ = true;
    edit_->clear();
    loading_ = false;
    setEnabled(false);
}

void NoteEditor::load()
{
    auto data = AppContext::get().library().item(libraryId_, ss(key_));
    if (!data) {
        clear();
        return;
    }
    auto html = jqs(*data, "note");
    if (html == lastSaved_ && edit_->document()->isModified())
        return;
    loading_ = true;
    int pos = edit_->textCursor().position();
    edit_->document()->clear();
    edit_->setHtml(noteHtmlForEditor(html, libraryId_, edit_->document()));
    edit_->prepareDocument();
    edit_->document()->clearUndoRedoStacks();
    edit_->document()->setModified(false);
    lastSerialized_ = documentToNoteHtml(edit_->document());
    auto cursor = edit_->textCursor();
    cursor.setPosition(std::min(pos, edit_->document()->characterCount() - 1));
    edit_->setTextCursor(cursor);
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

void NoteEditor::setHeading(int level)
{
    auto cursor = edit_->textCursor();
    cursor.beginEditBlock();
    QTextBlockFormat bf = cursor.blockFormat();
    bf.setHeadingLevel(level);
    cursor.setBlockFormat(bf);
    cursor.select(QTextCursor::BlockUnderCursor);
    QTextCharFormat cf;
    static const qreal sizes[] = {0, 1.6, 1.35, 1.17};
    qreal base = edit_->document()->defaultFont().pointSizeF();
    cf.setFontWeight(level ? QFont::Bold : QFont::Normal);
    cf.setFontPointSize(level ? base * sizes[level] : base);
    cursor.mergeCharFormat(cf);
    cursor.mergeBlockCharFormat(cf);
    cursor.endEditBlock();
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

} // namespace fp::gui
