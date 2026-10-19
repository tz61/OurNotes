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

namespace fp::gui {

QString noteHtmlForEditor(const QString& html, int64_t libraryId, QTextDocument* doc)
{
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

QString fragmentHtml(const QTextFragment& frag, const QTextCharFormat& blockDefault)
{
    auto f = frag.charFormat();
    if (f.isImageFormat()) {
        auto img = f.toImageFormat();
        QString attrs;
        if (img.width() > 0)
            attrs += QString(" width=\"%1\"").arg(qRound(img.width()));
        if (img.height() > 0)
            attrs += QString(" height=\"%1\"").arg(qRound(img.height()));
        auto name = img.name();
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
    if (f.hasProperty(QTextFormat::ForegroundBrush) && !f.isAnchor())
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

} // namespace fp::gui
