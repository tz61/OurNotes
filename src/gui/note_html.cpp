#include "gui/note_html.h"

#include <QImage>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/math_render.h"
#include "gui/rich_text_images.h"

namespace {
// what marks a caption's paragraph while the HTML becomes a document (noteHtmlForEditor):
// a block's user state, which QTextDocument keeps whatever the paragraph begins with
constexpr int kCaptionState = 0x43415054;
// a formula's own colour in a copy (Qt's HTML keeps no colour for an image, but its alt text)
const QString kColorMark = "ournotes-color:";
}

namespace on::gui {

// Zotero stores formulas as <span class="math">$…$</span> and <pre class="math">$$…$$</pre>.
QString mathMarkupToImages(const QString& html)
{
    // a display formula may have a colour (OurNotes writes it so; an inline one is in a
    // coloured span, like text)
    static const QRegularExpression display(R"re(<pre class="math"(?: style="color: (#[0-9A-Fa-f]{6})")?>([\s\S]*?)</pre>)re");
    static const QRegularExpression inl(R"re(<span class="math">([\s\S]*?)</span>)re");
    auto strip = [](QString tex, const QString& delim) {
        tex = qs(htmlUnescape(ss(tex))).trimmed();
        if (tex.startsWith(delim) && tex.endsWith(delim) && tex.size() >= 2 * delim.size())
            tex = tex.mid(delim.size(), tex.size() - 2 * delim.size());
        return tex.trimmed();
    };
    QString result;
    qsizetype i = 0;
    for (auto it = display.globalMatch(html); it.hasNext();) {
        auto m = it.next();
        result += html.mid(i, m.capturedStart() - i);
        QString img = "<img src=\"" + mathResourceName(strip(m.captured(2), "$$"), true) + "\" style=\"vertical-align: middle\">";
        if (!m.captured(1).isEmpty())
            img = "<span style=\"color: " + m.captured(1) + "\">" + img + "</span>";
        result += "<p align=\"center\">" + img + "</p>";
        i = m.capturedEnd();
    }
    result += html.mid(i);
    QString final;
    i = 0;
    auto it = inl.globalMatch(result);
    while (it.hasNext()) {
        auto m = it.next();
        final += result.mid(i, m.capturedStart() - i);
        final += "<img src=\"" + mathResourceName(strip(m.captured(1), "$"), false) +
                 "\" style=\"vertical-align: middle\">";
        i = m.capturedEnd();
    }
    final += result.mid(i);
    return final;
}

namespace {

struct Row {
    qsizetype end = 0;  // after its </tr> and the room after it
    QString cells;  // its own cells, the tables in them left out
};

// The row that starts at `at`, the tables in its cells passed over (a row's end is not the
// first </tr>: a table in a cell has its own); nothing when none starts there, or it is not
// closed before its table is.
std::optional<Row> rowAt(const QString& html, qsizetype at)
{
    constexpr auto ci = QRegularExpression::CaseInsensitiveOption;
    static const QRegularExpression start(R"re(<tr\b[^>]*>)re", ci);
    static const QRegularExpression tag(R"re(<(/?)(table|tr)\b[^>]*>)re", ci);
    auto s = start.match(html, at, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    if (!s.hasMatch())
        return std::nullopt;
    Row row;
    qsizetype from = s.capturedEnd();  // its own since here
    int depth = 0;  // in tables in its cells
    for (auto it = tag.globalMatch(html, s.capturedEnd()); it.hasNext();) {
        auto t = it.next();
        bool closing = !t.capturedView(1).isEmpty();
        bool table = t.capturedView(2).compare(u"table", Qt::CaseInsensitive) == 0;
        if (table && !closing) {
            if (depth++ == 0)
                row.cells += html.mid(from, t.capturedStart() - from);
        } else if (depth > 0) {
            if (table && --depth == 0)
                from = t.capturedEnd();
        } else if (table || !closing) {
            return std::nullopt;  // its table's end, or the next row: not closed
        } else {
            row.cells += html.mid(from, t.capturedStart() - from);
            row.end = t.capturedEnd();
            while (row.end < html.size() && html[row.end].isSpace())
                ++row.end;
            return row;
        }
    }
    return std::nullopt;
}

} // namespace

QString tableHeadersForEditor(const QString& html)
{
    constexpr auto ci = QRegularExpression::CaseInsensitiveOption;
    constexpr auto here = QRegularExpression::AnchorAtOffsetMatchOption;
    static const QRegularExpression table(R"re(<table\b[^>]*>)re", ci);
    // what may come before the rows: room, a caption, column groups
    static const QRegularExpression before(R"re((?:\s+|<caption\b[\s\S]*?</caption\s*>|<colgroup\b[\s\S]*?</colgroup\s*>|<col\b[^>]*>)+)re", ci);
    static const QRegularExpression body(R"re(<tbody\b[^>]*>\s*)re", ci);
    static const QRegularExpression th(R"re(<th\b)re", ci), td(R"re(<td\b)re", ci);
    // each table's, in a table's cells too: what is taken out at a place, and put in
    struct Edit {
        qsizetype at, taken;
        QString put;
    };
    std::vector<Edit> edits;
    for (auto it = table.globalMatch(html); it.hasNext();) {
        auto m = it.next();
        qsizetype at = m.capturedEnd();
        if (auto b = before.match(html, at, QRegularExpression::NormalMatch, here); b.hasMatch())
            at = b.capturedEnd();
        qsizetype head = at;  // where the <thead> goes
        QString tbody;
        if (auto b = body.match(html, at, QRegularExpression::NormalMatch, here); b.hasMatch()) {
            tbody = b.captured(0);
            at = b.capturedEnd();
        }
        qsizetype rows = at;  // its first rows of header cells only
        while (auto r = rowAt(html, at)) {
            if (!r->cells.contains(th) || r->cells.contains(td))
                break;
            at = r->end;
        }
        if (at == rows)
            continue;
        edits.push_back({head, rows - head, "<thead>"});
        edits.push_back({at, 0, "</thead>" + tbody});
    }
    // in the order of their places (a table in a header cell: within its table's)
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.at < b.at; });
    QString out;
    qsizetype last = 0;
    for (auto& e : edits) {
        out += html.mid(last, e.at - last) + e.put;
        last = e.at + e.taken;
    }
    return out + html.mid(last);
}

void styleNoteTable(QTextTable* table)
{
    auto f = table->format();
    QTextTableFormat want = f;
    want.setBorder(1);
    want.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    want.setBorderBrush(QColor(128, 128, 128));  // a grey that shows on light and dark
    want.setBorderCollapse(true);
    want.setCellSpacing(0);
    want.setCellPadding(4);
    want.setTopMargin(4);
    want.setBottomMargin(4);
    if (want != f)
        table->setFormat(want);
    // each cell's content at the top of its row, as Zotero shows it (and as it is saved: a
    // table from a spreadsheet had its cells at the bottom)
    for (int r = 0; r < table->rows(); ++r)
        for (int c = 0; c < table->columns(); ++c) {
            auto cell = table->cellAt(r, c);
            auto cf = cell.format().toTableCellFormat();
            if (cell.row() != r || cell.column() != c || !cf.hasProperty(QTextFormat::TextVerticalAlignment))
                continue;  // (a span's, or as it should be)
            cf.clearProperty(QTextFormat::TextVerticalAlignment);
            cell.setFormat(cf);
        }
}

QTextCharFormat captionCharFormat()
{
    QTextCharFormat f;
    f.setForeground(QColor(0x88, 0x88, 0x88));  // (readable on dark and on light)
    f.setProperty(QTextFormat::FontSizeAdjustment, -1);
    f.setProperty(kCaptionProperty, true);
    return f;
}

void setCaption(QTextBlock block, bool caption)
{
    QTextCursor c(block);
    auto bf = block.blockFormat();
    if (caption)
        bf.setProperty(kCaptionProperty, true);
    else
        bf.clearProperty(kCaptionProperty);
    c.setBlockFormat(bf);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    auto cf = block.charFormat();
    if (caption) {
        cf.merge(captionCharFormat());
        // smaller and gray; text with a colour of its own keeps it
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            auto f = frag.charFormat();
            auto add = captionCharFormat();
            if (f.hasProperty(QTextFormat::ForegroundBrush) || f.hasProperty(kOriginalForegroundProperty))
                add.clearProperty(QTextFormat::ForegroundBrush);
            QTextCursor part(c.document());
            part.setPosition(frag.position());
            part.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            part.mergeCharFormat(add);
        }
    } else {
        for (int p : {(int)QTextFormat::ForegroundBrush, (int)QTextFormat::FontSizeAdjustment, kCaptionProperty})
            cf.clearProperty(p);
        // its text as it was before it was a caption's: its own colour and size kept
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            auto f = frag.charFormat();
            if (!f.hasProperty(kCaptionProperty))
                continue;
            if (f.foreground().color() == captionCharFormat().foreground().color())
                f.clearProperty(QTextFormat::ForegroundBrush);
            f.clearProperty(QTextFormat::FontSizeAdjustment);
            f.clearProperty(kCaptionProperty);
            QTextCursor part(c.document());
            part.setPosition(frag.position());
            part.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            part.setCharFormat(f);
        }
    }
    c.setBlockCharFormat(cf);
}

void markForCopy(QTextDocument* doc)
{
    struct Change {
        int start, end;
        QTextCharFormat format;
    };
    std::vector<Change> changes;
    auto gray = captionCharFormat().foreground().color();
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().hasProperty(kCaptionProperty) && block.length() > 1)
            block.setUserState(kCaptionState);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            auto f = frag.charFormat();
            bool changed = false;
            if (f.hasProperty(kOriginalForegroundProperty)) {  // (hidden for contrast: its own)
                f.setForeground(f.property(kOriginalForegroundProperty).value<QColor>());
                f.clearProperty(kOriginalForegroundProperty);
                changed = true;
            }
            if (f.hasProperty(kCaptionProperty)) {  // (the caption's look: made again for a caption)
                if (f.foreground().color() == gray)
                    f.clearProperty(QTextFormat::ForegroundBrush);
                f.clearProperty(QTextFormat::FontSizeAdjustment);
                f.clearProperty(kCaptionProperty);
                changed = true;
            }
            if (f.isImageFormat() && parseMathResourceName(f.toImageFormat().name(), nullptr, nullptr) &&
                f.hasProperty(QTextFormat::ForegroundBrush)) {  // (a formula's colour: see readCopy)
                f.setProperty(QTextFormat::ImageAltText, kColorMark + f.foreground().color().name());
                changed = true;
            }
            if (changed)
                changes.push_back({frag.position(), frag.position() + frag.length(), f});
        }
    }
    QTextCursor c(doc);
    for (auto& change : changes) {
        c.setPosition(change.start);
        c.setPosition(change.end, QTextCursor::KeepAnchor);
        c.setCharFormat(change.format);
    }
    // an empty caption (one begun and left so, or where the selection ended) is not copied, as
    // it is not saved: its gray would be the line's it is pasted on
    std::vector<int> empty;
    for (auto block = doc->begin(); block.isValid(); block = block.next())
        if (block.blockFormat().hasProperty(kCaptionProperty) && block.length() <= 1)
            empty.push_back(block.position());
    for (auto it = empty.rbegin(); it != empty.rend(); ++it) {
        auto block = doc->findBlock(*it);
        auto previous = block.previous();
        if (previous.isValid() && QTextCursor(previous).currentFrame() == QTextCursor(block).currentFrame()) {
            c.setPosition(block.position() - 1);
            c.setPosition(block.position(), QTextCursor::KeepAnchor);
            c.removeSelectedText();
        } else {  // (the copy's first line: a paragraph, empty)
            QTextCursor(block).setBlockFormat(QTextBlockFormat());
            QTextCursor(block).setBlockCharFormat(QTextCharFormat());
        }
    }
}

void readCopy(QTextDocument* doc)
{
    struct Change {
        int start, end;
        QTextCharFormat format;
    };
    std::vector<Change> changes;
    for (auto block = doc->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto f = it.fragment().charFormat();
            auto alt = f.property(QTextFormat::ImageAltText).toString();
            if (!f.isImageFormat() || !alt.startsWith(kColorMark))
                continue;
            f.setForeground(QColor(alt.mid(kColorMark.size())));
            f.clearProperty(QTextFormat::ImageAltText);
            changes.push_back({it.fragment().position(), it.fragment().position() + it.fragment().length(), f});
        }
    QTextCursor c(doc);
    for (auto& change : changes) {
        c.setPosition(change.start);
        c.setPosition(change.end, QTextCursor::KeepAnchor);
        c.setCharFormat(change.format);
    }
    markCaptions(doc);
}

void markCaptions(QTextDocument* doc)
{
    for (auto block = doc->begin(); block.isValid(); block = block.next())
        if (block.userState() == kCaptionState) {
            block.setUserState(-1);
            setCaption(block, true);
        }
}

void styleNoteTables(QTextDocument* doc)
{
    std::vector<QTextTable*> tables;
    std::function<void(QTextFrame*)> collect = [&](QTextFrame* frame) {
        for (auto* child : frame->childFrames()) {
            if (auto* t = qobject_cast<QTextTable*>(child))
                tables.push_back(t);
            collect(child);
        }
    };
    collect(doc->rootFrame());
    for (auto* t : tables)
        styleNoteTable(t);
}

QString noteHtmlForEditor(const QString& rawHtml, int64_t libraryId, QTextDocument* doc)
{
    QString html = tableHeadersForEditor(mathMarkupToImages(rawHtml));
    // a caption (<p class="caption">) is marked by a user state, which QTextDocument keeps
    // (it keeps no class); markCaptions makes it a caption. (An empty one is left out: an
    // empty paragraph is dropped, and its state would go to the one after it. Empty: nothing
    // in it but room and tags, spans, comments, <br>; a picture is something.)
    constexpr auto ci = QRegularExpression::CaseInsensitiveOption;
    static const QRegularExpression caption(R"re(<p\b([^>]*?)\s*\bclass\s*=\s*["']caption["']([^>]*)>)re", ci);
    static const QRegularExpression end(R"re(</p\s*>)re", ci);
    static const QRegularExpression tag(R"re(<!--[\s\S]*?-->|<(?!img\b)[^>]*>)re", ci);
    static const QRegularExpression style(R"re(\bstyle\s*=\s*(["'])(.*?)\1)re", ci);
    QString marked;
    qsizetype done = 0;
    for (auto it = caption.globalMatch(html); it.hasNext();) {
        auto m = it.next();
        auto close = end.match(html, m.capturedEnd());
        if (close.hasMatch() && html.mid(m.capturedEnd(), close.capturedStart() - m.capturedEnd()).remove(tag).trimmed().isEmpty())
            continue;
        QString attrs = m.captured(1) + m.captured(2), state = QString("-qt-user-state: %1").arg(kCaptionState);
        if (auto s = style.match(attrs); s.hasMatch()) {
            QString css = s.captured(2).trimmed().replace('"', '\'');  // (in double quotes now: font-family: "Times")
            if (css.endsWith(';'))
                css.chop(1);
            attrs.replace(s.capturedStart(), s.capturedLength(), "style=\"" + (css.isEmpty() ? state : css + "; " + state) + "\"");
        } else {
            attrs += " style=\"" + state + "\"";
        }
        marked += html.mid(done, m.capturedStart() - done) + "<p" + attrs + ">";
        done = m.capturedEnd();
    }
    html = marked + html.mid(done);
    static const QRegularExpression img(R"re(<img\b([^>]*?)data-attachment-key="([A-Za-z0-9]+)"([^>]*)>)re");
    auto& lib = AppContext::get().library();
    QString out;
    qsizetype last = 0;
    auto it = img.globalMatch(html);
    while (it.hasNext()) {
        auto m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        auto key = m.captured(2);
        auto name = "attachment:" + key;
        if (auto att = lib.item(libraryId, ss(key))) {
            QImage image(qpath(lib.attachmentFile(libraryId, *att)));
            if (!image.isNull())
                doc->addResource(QTextDocument::ImageResource, QUrl(name), image);
        }
        out += "<img" + m.captured(1) + "src=\"" + name + "\"" + m.captured(3) + ">";
        last = m.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

namespace {

QString esc(const QString& s) { return s.toHtmlEscaped(); }

bool isMonospace(const QTextCharFormat& f)
{
    if (f.fontFixedPitch())
        return true;
    auto families = f.fontFamilies().toStringList();
    for (auto& fam : families)
        if (fam.contains("mono", Qt::CaseInsensitive) || fam.contains("courier", Qt::CaseInsensitive))
            return true;
    return false;
}

QString mathSourceLatex(const QString& text, bool display)
{
    QString delim = display ? "$$" : "$";
    QString t = text;
    if (t.startsWith(delim) && t.endsWith(delim) && t.size() >= 2 * delim.size())
        t = t.mid(delim.size(), t.size() - 2 * delim.size());
    return t.trimmed();
}

// The colour a formula has of its own (that of the text it was typed in), also when hidden
// for contrast in dark mode; invalid if none.
QColor ownFormulaColor(const QTextCharFormat& f)
{
    if (f.hasProperty(kOriginalForegroundProperty))
        return f.property(kOriginalForegroundProperty).value<QColor>();
    return f.hasProperty(QTextFormat::ForegroundBrush) ? f.foreground().color() : QColor();
}

// An inline formula with a colour of its own is in a coloured span, as Zotero colours text.
QString inlineFormula(const QTextCharFormat& f, const QString& latex)
{
    QString html = "<span class=\"math\">$" + esc(latex) + "$</span>";
    QColor colour = ownFormulaColor(f);
    return colour.isValid() ? "<span style=\"color: " + colour.name() + "\">" + html + "</span>" : html;
}

QString displayFormula(const QTextCharFormat& f, const QString& latex)
{
    QColor colour = ownFormulaColor(f);
    return "<pre class=\"math\"" + (colour.isValid() ? " style=\"color: " + colour.name() + "\"" : QString()) + ">$$" + esc(latex) +
           "$$</pre>";
}

// caption: the fragment is in a caption, whose gray (the text's unless it has a colour of its
// own) is not saved, hidden as too near the background to read (adaptColors) or not.
QString fragmentHtml(const QTextFragment& frag, const QTextCharFormat& blockDefault, QSizeF size = {}, bool caption = false)
{
    auto f = frag.charFormat();
    auto gray = captionCharFormat().foreground().color();
    if (caption && (f.hasProperty(kOriginalForegroundProperty)
                        ? f.property(kOriginalForegroundProperty).value<QColor>() == gray
                        : f.hasProperty(QTextFormat::ForegroundBrush) && f.foreground().color() == gray)) {
        f.clearProperty(kOriginalForegroundProperty);
        f.clearProperty(QTextFormat::ForegroundBrush);
    }
    if (int kind = f.property(kMathSourceProperty).toInt()) {
        auto latex = mathSourceLatex(frag.text(), kind == 2);
        // an empty one is its signs, as text: what stays when it is left so
        return latex.isEmpty() ? esc(kind == 2 ? "$$$$" : "$$") : inlineFormula(f, latex);
    }
    if (f.isImageFormat()) {
        auto img = f.toImageFormat();
        QString attrs;
        qreal w = size.isValid() ? size.width() : img.width(), h = size.isValid() ? size.height() : img.height();
        if (w > 0)
            attrs += QString(" width=\"%1\"").arg(qRound(w));
        if (h > 0)
            attrs += QString(" height=\"%1\"").arg(qRound(h));
        auto name = img.name();
        QString latex;
        if (parseMathResourceName(name, &latex, nullptr))
            return inlineFormula(f, latex);
        if (name.startsWith("attachment:"))
            return "<img data-attachment-key=\"" + esc(name.mid(11)) + "\"" + attrs + ">";
        return "<img src=\"" + esc(name) + "\"" + attrs + ">";
    }
    QString text = esc(frag.text());
    text.replace(QChar::LineSeparator, "<br>");
    text.replace(QChar::Nbsp, "&nbsp;");
    QString open, close;
    auto wrap = [&](const QString& tag, const QString& attrs = {}) {
        open += "<" + tag + attrs + ">";
        close = "</" + tag + ">" + close;
    };
    if (f.isAnchor() && !f.anchorHref().isEmpty())
        wrap("a", " href=\"" + esc(f.anchorHref()) + "\"");
    if (f.fontWeight() >= QFont::DemiBold && blockDefault.fontWeight() < QFont::DemiBold)
        wrap("strong");
    if (f.fontItalic())
        wrap("em");
    if (f.fontUnderline() && !f.isAnchor())
        wrap("u");
    if (f.fontStrikeOut())
        wrap("s");
    if (f.verticalAlignment() == QTextCharFormat::AlignSuperScript)
        wrap("sup");
    else if (f.verticalAlignment() == QTextCharFormat::AlignSubScript)
        wrap("sub");
    if (isMonospace(f) && !isMonospace(blockDefault))
        wrap("code");
    QStringList style;
    if (f.hasProperty(kOriginalForegroundProperty) && !f.isAnchor())  // (a link's colour is not its own)
        style << "color: " + f.property(kOriginalForegroundProperty).value<QColor>().name();
    else if (f.hasProperty(QTextFormat::ForegroundBrush) && !f.isAnchor())
        style << "color: " + f.foreground().color().name();
    if (f.hasProperty(QTextFormat::BackgroundBrush) && f.background().style() != Qt::NoBrush)
        style << "background-color: " + f.background().color().name();
    if (!style.isEmpty())
        wrap("span", " style=\"" + style.join("; ") + "\"");
    return open + text + close;
}

// Where a block is, for the weight its text has unless it is marked bold: a heading's and a
// header cell's text is bold by definition; another cell's is not (Qt reads a <th> as bold
// text, which a cell not in the header row keeps as <strong>).
enum class Cell { None, Header, Body };

QString blockContent(const QTextBlock& block, Cell cell = Cell::None)
{
    QString out;
    auto def = block.charFormat();
    if (cell == Cell::Body)
        def.setFontWeight(QFont::Normal);
    if (block.blockFormat().headingLevel() > 0 || cell == Cell::Header)
        def.setFontWeight(QFont::Bold);  // headings are bold by definition
    bool caption = block.blockFormat().hasProperty(kCaptionProperty);
    // pictures alone on a line: all as high as the row, as they are shown (when all can be
    // measured: one not downloaded yet keeps the size it has, or has none)
    auto row = pictureRow(block);
    qreal height = row.size() > 1 ? rowHeight(block.document(), block) : 0;
    for (auto it = block.begin(); height > 0 && !it.atEnd(); ++it)
        if (!pictureKnown(block.document(), it.fragment().charFormat().toImageFormat()))
            height = 0;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        auto frag = it.fragment();
        if (!frag.isValid())
            continue;
        if (!isPicture(frag.charFormat())) {
            out += fragmentHtml(frag, def, {}, caption);
            continue;
        }
        QSizeF size;  // (as stored)
        if (height > 0) {
            auto own = pictureSize(block.document(), frag.charFormat().toImageFormat());
            size = QSizeF(height * own.width() / std::max<qreal>(1, own.height()), height);
        }
        for (int i = 0; i < frag.length(); ++i)  // (the same picture twice is one fragment)
            out += fragmentHtml(frag, def, size);
    }
    return out;
}

// A paragraph holding only a display formula (rendered, or being edited): as Zotero keeps it.
std::optional<QString> displayFormulaHtml(const QTextBlock& block)
{
    if (auto first = block.begin(); !first.atEnd() && first.fragment().charFormat().property(kMathSourceProperty).toInt() == 2 &&
                                    first.fragment().length() == block.length() - 1 &&
                                    !mathSourceLatex(first.fragment().text(), true).isEmpty())
        return displayFormula(first.fragment().charFormat(), mathSourceLatex(first.fragment().text(), true));
    if (block.text() == QString(QChar::ObjectReplacementCharacter)) {
        auto f = block.begin().fragment().charFormat();
        QString latex;
        bool display = false;
        if (f.isImageFormat() && parseMathResourceName(f.toImageFormat().name(), &latex, &display) && display)
            return displayFormula(f, latex);
    }
    return std::nullopt;
}

void serializeBlocks(QTextFrame::iterator it, QString& out, Cell cell = Cell::None);

// A cell whose blocks are all plain paragraphs: written as their text, lines apart, as Zotero
// writes such a cell too; anything else (a list, a heading, a quote, code, a formula on a
// line of its own) is written as the blocks it is.
bool plainCell(const QTextTableCell& cell)
{
    for (auto it = cell.begin(); !it.atEnd(); ++it) {
        auto block = it.currentBlock();
        if (it.currentFrame() || !block.isValid())
            continue;
        auto bf = block.blockFormat();
        if (block.textList() || bf.headingLevel() > 0 || bf.nonBreakableLines() || bf.leftMargin() >= 20 ||
            bf.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth) || displayFormulaHtml(block))
            return false;
    }
    return true;
}

// The header row's cells are <th>, and say how they are aligned (a <th> is centred unless
// it says otherwise); other cells say so when centred or to the right.
void serializeTable(QTextTable* table, QString& out)
{
    int headerRows = table->format().headerRowCount();
    out += "<table>";
    for (int r = 0; r < table->rows(); ++r) {
        bool header = r < headerRows;
        QString tag = header ? "th" : "td";
        out += "<tr>";
        for (int c = 0; c < table->columns(); ++c) {
            auto cell = table->cellAt(r, c);
            if (cell.row() != r || cell.column() != c)
                continue;  // covered by a span
            QString attrs;
            if (cell.rowSpan() > 1)
                attrs += QString(" rowspan=\"%1\"").arg(cell.rowSpan());
            if (cell.columnSpan() > 1)
                attrs += QString(" colspan=\"%1\"").arg(cell.columnSpan());
            bool plain = plainCell(cell);
            auto align = cell.firstCursorPosition().blockFormat().alignment();
            if (!plain)
                ;  // each of its paragraphs says how it is aligned (on the cell, all would take it)
            else if (align & Qt::AlignHCenter)
                attrs += " align=\"center\"";
            else if (align & Qt::AlignRight)
                attrs += " align=\"right\"";
            else if (header)
                attrs += " align=\"left\"";
            out += "<" + tag + attrs + ">";
            auto kind = header ? Cell::Header : Cell::Body;
            if (plain) {
                bool afterText = false;  // <br> only between two lines of text: a table breaks the line itself
                for (auto it = cell.begin(); !it.atEnd(); ++it) {
                    if (auto* inner = qobject_cast<QTextTable*>(it.currentFrame())) {  // a table in the cell
                        serializeTable(inner, out);
                        afterText = false;
                    } else if (it.currentBlock().isValid()) {
                        if (afterText)
                            out += "<br>";
                        out += blockContent(it.currentBlock(), kind);
                        afterText = true;
                    }
                }
            } else {
                serializeBlocks(cell.begin(), out, kind);
            }
            out += "</" + tag + ">";
        }
        out += "</tr>";
    }
    out += "</table>";
}

void serializeBlocks(QTextFrame::iterator it, QString& out, Cell cell)
{
    QTextList* openList = nullptr;
    QList<QTextList*> listStack;
    bool inQuote = false, inPre = false;
    auto closeLists = [&](int toDepth) {
        while (listStack.size() > toDepth) {
            auto* l = listStack.takeLast();
            out += "</li>";
            out += l->format().style() == QTextListFormat::ListDecimal || l->format().style() == QTextListFormat::ListLowerAlpha ||
                           l->format().style() == QTextListFormat::ListUpperAlpha ||
                           l->format().style() == QTextListFormat::ListLowerRoman ||
                           l->format().style() == QTextListFormat::ListUpperRoman
                       ? "</ol>"
                       : "</ul>";
        }
    };
    for (; !it.atEnd(); ++it) {
        if (auto* child = it.currentFrame()) {
            closeLists(0);
            if (inQuote) {
                out += "</blockquote>";
                inQuote = false;
            }
            if (auto* table = qobject_cast<QTextTable*>(child))
                serializeTable(table, out);
            else
                serializeBlocks(child->begin(), out, cell);
            continue;
        }
        auto block = it.currentBlock();
        if (!block.isValid())
            continue;
        auto bf = block.blockFormat();
        auto* list = block.textList();
        bool pre = bf.nonBreakableLines();
        bool quote = !list && !pre && bf.leftMargin() >= 20 && bf.headingLevel() == 0;
        if (!pre && inPre) {
            out += "</pre>";
            inPre = false;
        }
        if (quote != inQuote) {
            closeLists(0);
            out += quote ? "<blockquote>" : "</blockquote>";
            inQuote = quote;
        }
        if (list) {
            int depth = std::max(1, list->format().indent());
            int idx = listStack.indexOf(list);
            if (idx >= 0) {
                closeLists(idx + 1);
                out += "</li>";
            } else {
                while (!listStack.isEmpty() && listStack.last()->format().indent() >= depth)
                    closeLists(listStack.size() - 1);
                auto st = list->format().style();
                bool ordered = st == QTextListFormat::ListDecimal || st == QTextListFormat::ListLowerAlpha ||
                               st == QTextListFormat::ListUpperAlpha || st == QTextListFormat::ListLowerRoman ||
                               st == QTextListFormat::ListUpperRoman;
                out += ordered ? "<ol>" : "<ul>";
                listStack << list;
            }
            out += "<li>" + blockContent(block, cell);
            openList = list;
            continue;
        }
        closeLists(0);
        openList = nullptr;
        // (a caption that is only a formula: a caption still, the formula in its line)
        if (auto formula = displayFormulaHtml(block); formula && !block.blockFormat().hasProperty(kCaptionProperty)) {
            out += *formula;
            continue;
        }
        auto content = blockContent(block, cell);
        if (bf.headingLevel() > 0) {
            auto tag = QString("h%1").arg(std::min(6, bf.headingLevel()));
            out += "<" + tag + ">" + content + "</" + tag + ">";
        } else if (pre) {
            if (!inPre) {
                out += "<pre>";
                inPre = true;
            } else {
                out += "\n";
            }
            out += esc(block.text());
        } else if (bf.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            out += "<hr>";
        } else {
            QString align;
            if (bf.alignment() & Qt::AlignHCenter)
                align = " style=\"text-align: center\"";
            else if (bf.alignment() & Qt::AlignRight)
                align = " style=\"text-align: right\"";
            bool caption = bf.hasProperty(kCaptionProperty);
            if (caption && block.text().trimmed().isEmpty())
                continue;  // a caption begun and left empty
            out += "<p" + QString(caption ? " class=\"caption\"" : "") + align + ">" + content + "</p>";
        }
    }
    (void)openList;
    closeLists(0);
    if (inPre)
        out += "</pre>";
    if (inQuote)
        out += "</blockquote>";
}

} // namespace

QString documentToNoteHtml(const QTextDocument* doc)
{
    QString body;
    serializeBlocks(doc->rootFrame()->begin(), body);
    // drop trailing empty paragraphs
    while (body.endsWith("<p></p>"))
        body.chop(7);
    return "<div data-schema-version=\"9\">" + body + "</div>";
}

QString commentHtmlForEditor(const QString& comment)
{
    // as typed in notes: $…$ with no space inside its ends, so prices stay text
    static const QRegularExpression math(
        R"re(\$\$([^$]*\S[^$]*)\$\$|(?<![\\$])\$([^\s$](?:[^$]*[^\s$\\])?)\$(?!\d))re");
    static const QRegularExpression displayLine(R"re(^\s*\$\$[^$]*\S[^$]*\$\$\s*$)re");
    QString html;
    for (auto& line : comment.split('\n')) {
        QString body;
        qsizetype last = 0;
        auto it = math.globalMatch(line);
        while (it.hasNext()) {
            auto m = it.next();
            body += line.mid(last, m.capturedStart() - last).toHtmlEscaped();
            bool display = m.capturedLength(1) > 0;
            auto latex = (display ? m.captured(1) : m.captured(2)).trimmed();
            body += "<img src=\"" + mathResourceName(latex, display) + "\" style=\"vertical-align: middle\">";
            last = m.capturedEnd();
        }
        body += line.mid(last).toHtmlEscaped();
        html += displayLine.match(line).hasMatch() ? "<p align=\"center\">" + body + "</p>" : "<p>" + body + "</p>";
    }
    return html;
}

QString documentToComment(const QTextDocument* doc)
{
    QStringList lines;
    for (auto block = doc->begin(); block.isValid(); block = block.next()) {
        QString line;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            auto frag = it.fragment();
            auto f = frag.charFormat();
            QString latex;
            bool display = false;
            if (f.isImageFormat()) {
                if (parseMathResourceName(f.toImageFormat().name(), &latex, &display))
                    line += display ? "$$" + latex + "$$" : "$" + latex + "$";
                continue;  // other pictures have no place in a comment
            }
            QString text = frag.text();  // a formula being edited is its $…$ source already
            text.replace(QChar::LineSeparator, '\n');
            text.replace(QChar::Nbsp, ' ');
            text.remove(QChar::ObjectReplacementCharacter);
            line += text;
        }
        lines << line;
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    return lines.join('\n');
}

} // namespace on::gui
