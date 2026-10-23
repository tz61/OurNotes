#include "gui/note_html.h"

#include <QImage>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QUrl>

#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/math_render.h"

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

QString noteHtmlForEditor(const QString& rawHtml, int64_t libraryId, QTextDocument* doc)
{
    QString html = mathMarkupToImages(rawHtml);
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

QString fragmentHtml(const QTextFragment& frag, const QTextCharFormat& blockDefault)
{
    auto f = frag.charFormat();
    if (int kind = f.property(kMathSourceProperty).toInt()) {
        auto latex = mathSourceLatex(frag.text(), kind == 2);
        // an empty one is its signs, as text: what stays when it is left so
        return latex.isEmpty() ? esc(kind == 2 ? "$$$$" : "$$") : inlineFormula(f, latex);
    }
    if (f.isImageFormat()) {
        auto img = f.toImageFormat();
        QString attrs;
        if (img.width() > 0)
            attrs += QString(" width=\"%1\"").arg(qRound(img.width()));
        if (img.height() > 0)
            attrs += QString(" height=\"%1\"").arg(qRound(img.height()));
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
    if (f.hasProperty(kOriginalForegroundProperty))
        style << "color: " + f.property(kOriginalForegroundProperty).value<QColor>().name();
    else if (f.hasProperty(QTextFormat::ForegroundBrush) && !f.isAnchor())
        style << "color: " + f.foreground().color().name();
    if (f.hasProperty(QTextFormat::BackgroundBrush) && f.background().style() != Qt::NoBrush)
        style << "background-color: " + f.background().color().name();
    if (!style.isEmpty())
        wrap("span", " style=\"" + style.join("; ") + "\"");
    return open + text + close;
}

QString blockContent(const QTextBlock& block)
{
    QString out;
    auto def = block.charFormat();
    if (block.blockFormat().headingLevel() > 0)
        def.setFontWeight(QFont::Bold);  // headings are bold by definition
    for (auto it = block.begin(); !it.atEnd(); ++it)
        if (it.fragment().isValid())
            out += fragmentHtml(it.fragment(), def);
    return out;
}

void serializeFrame(QTextFrame* frame, QString& out);

void serializeTable(QTextTable* table, QString& out)
{
    out += "<table>";
    for (int r = 0; r < table->rows(); ++r) {
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
            out += "<td" + attrs + ">";
            QStringList paras;
            for (auto it = cell.begin(); !it.atEnd(); ++it)
                if (it.currentBlock().isValid())
                    paras << blockContent(it.currentBlock());
            out += paras.join("<br>");
            out += "</td>";
        }
        out += "</tr>";
    }
    out += "</table>";
}

void serializeFrame(QTextFrame* frame, QString& out)
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
    for (auto it = frame->begin(); !it.atEnd(); ++it) {
        if (auto* child = it.currentFrame()) {
            closeLists(0);
            if (inQuote) {
                out += "</blockquote>";
                inQuote = false;
            }
            if (auto* table = qobject_cast<QTextTable*>(child))
                serializeTable(table, out);
            else
                serializeFrame(child, out);
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
            out += "<li>" + blockContent(block);
            openList = list;
            continue;
        }
        closeLists(0);
        openList = nullptr;
        // a paragraph holding only a display formula (rendered, or being edited)
        if (auto first = block.begin(); !first.atEnd() && first.fragment().charFormat().property(kMathSourceProperty).toInt() == 2 &&
                                        first.fragment().length() == block.length() - 1 &&
                                        !mathSourceLatex(first.fragment().text(), true).isEmpty()) {
            out += displayFormula(first.fragment().charFormat(), mathSourceLatex(first.fragment().text(), true));
            continue;
        }
        if (block.text() == QString(QChar::ObjectReplacementCharacter)) {
            auto f = block.begin().fragment().charFormat();
            QString latex;
            bool display = false;
            if (f.isImageFormat() && parseMathResourceName(f.toImageFormat().name(), &latex, &display) && display) {
                out += displayFormula(f, latex);
                continue;
            }
        }
        auto content = blockContent(block);
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
            out += "<p" + align + ">" + content + "</p>";
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
    serializeFrame(doc->rootFrame(), body);
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
