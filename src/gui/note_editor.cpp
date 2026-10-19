#include "gui/note_editor.h"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
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
#include "gui/note_html.h"

namespace fp::gui {

// --- NoteTextEdit -----------------------------------------------------------------------

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
}

void NoteTextEdit::mouseMoveEvent(QMouseEvent* e)
{
    bool overLink = !anchorAt(e->pos()).isEmpty() && (e->modifiers() & Qt::ControlModifier);
    viewport()->setCursor(overLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
    QTextEdit::mouseMoveEvent(e);
}

// --- NoteEditor ---------------------------------------------------------------------------

NoteEditor::NoteEditor(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    toolbar_ = new QToolBar(this);
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
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
    toolbar_->addAction("🔗", this, [this] { insertLink(); })->setToolTip(tr("Insert link (Ctrl+K)"));
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
        if (!loading_ && !key_.isEmpty())
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
    edit_->document()->setModified(false);
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
    if (html == lastSaved_)
        return;
    lastSaved_ = html;
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

} // namespace fp::gui
