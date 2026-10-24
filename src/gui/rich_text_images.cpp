#include "gui/rich_text_images.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QUrl>

#include <algorithm>

#include "gui/math_render.h"

namespace on::gui {

namespace {

QImage imageResource(QTextDocument* doc, const QString& name)
{
    QVariant v = doc->resource(QTextDocument::ImageResource, QUrl(name));
    if (v.canConvert<QImage>())
        return v.value<QImage>();
    if (v.canConvert<QPixmap>())
        return v.value<QPixmap>().toImage();
    if (v.typeId() == QMetaType::QByteArray)
        return QImage::fromData(v.toByteArray());
    return {};
}

// Width left for an object in its paragraph: page width minus margins, block
// indentation and list indentation.
qreal availableWidth(QTextDocument* doc, int pos)
{
    qreal w = doc->textWidth();
    if (w <= 0)
        return 1e9;
    w -= 2 * doc->documentMargin() + 6;
    QTextBlock block = doc->findBlock(pos);
    if (block.isValid()) {
        auto bf = block.blockFormat();
        w -= bf.leftMargin() + bf.rightMargin() + std::max<qreal>(0, bf.textIndent()) + bf.indent() * doc->indentWidth();
        if (auto* list = block.textList())
            w -= list->format().indent() * doc->indentWidth();
    }
    return std::max<qreal>(16, w);
}

QFont formatFont(QTextDocument* doc, const QTextFormat& format)
{
    return format.toCharFormat().font().resolve(doc->defaultFont());
}

// (from the block's fragments: a cursor would lay the block out, from within its layout)
QTextImageFormat imageFormatAt(const QTextDocument* doc, int pos)
{
    auto block = doc->findBlock(pos);
    for (auto it = block.begin(); !it.atEnd(); ++it)
        if (auto frag = it.fragment(); frag.position() <= pos && pos < frag.position() + frag.length())
            return frag.charFormat().toImageFormat();
    return {};
}

qreal aspectOf(const QSizeF& s) { return s.height() > 0 ? s.width() / s.height() : 1; }

} // namespace

bool isPicture(const QTextFormat& format)
{
    return format.isImageFormat() && !parseMathResourceName(format.toImageFormat().name(), nullptr, nullptr);
}

std::vector<int> pictureRow(const QTextBlock& block)
{
    std::vector<int> out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        auto frag = it.fragment();
        if (!isPicture(frag.charFormat()))
            return {};
        for (int i = 0; i < frag.length(); ++i)
            out.push_back(frag.position() + i);
    }
    return out;
}

QSizeF naturalPictureSize(const QTextDocument* doc, const QTextImageFormat& format)
{
    QImage image = imageResource(const_cast<QTextDocument*>(doc), format.name());
    return image.isNull() ? QSizeF() : QSizeF(image.size()) / image.devicePixelRatio();
}

QTextCharFormat textFormatOf(QTextCharFormat format)
{
    const auto properties = format.properties();
    for (auto it = properties.begin(); it != properties.end(); ++it)
        if (it.key() >= QTextFormat::ImageName && it.key() < QTextFormat::ImageName + 0x100)
            format.clearProperty(it.key());
    for (int property : {int(QTextFormat::ObjectType), int(QTextFormat::ObjectIndex), int(QTextFormat::TextVerticalAlignment),
                         int(QTextFormat::TextToolTip)})
        format.clearProperty(property);
    return format;
}

bool pictureKnown(const QTextDocument* doc, const QTextImageFormat& format)
{
    return (format.width() > 0 && format.height() > 0) || !naturalPictureSize(doc, format).isEmpty();
}

QSizeF pictureSize(const QTextDocument* doc, const QTextImageFormat& format)
{
    QSizeF natural = naturalPictureSize(doc, format);
    qreal w = format.width(), h = format.height();
    if (w > 0 && h > 0)
        return {w, h};
    if (natural.isEmpty())
        return {w > 0 ? w : 16, h > 0 ? h : 16};
    if (w > 0)
        return {w, w / aspectOf(natural)};
    if (h > 0)
        return {h * aspectOf(natural), h};
    // small, unless it is smaller
    qreal scale = std::min({1.0, kDefaultPictureBox.width() / natural.width(), kDefaultPictureBox.height() / natural.height()});
    return natural * scale;
}

qreal rowHeight(const QTextDocument* doc, const QTextBlock& block)
{
    auto row = pictureRow(block);
    for (int p : row)
        if (auto f = imageFormatAt(doc, p); pictureKnown(doc, f))
            return pictureSize(doc, f).height();
    return row.empty() ? 0 : pictureSize(doc, imageFormatAt(doc, row.front())).height();
}

qreal maxPictureHeight(QTextDocument* doc, int pos)
{
    auto row = pictureRow(doc->findBlock(pos));
    if (row.empty())  // (one in a line of text)
        row = {pos};
    qreal aspects = 0, gaps = row.size() > 1 ? 2 * kPictureGap * row.size() : 0;
    for (int p : row)
        aspects += aspectOf(pictureSize(doc, imageFormatAt(doc, p)));
    return std::max<qreal>(16, (availableWidth(doc, pos) - 1 - gaps) / aspects);
}

FittedImageHandler* FittedImageHandler::install(QTextDocument* doc, QObject* owner)
{
    auto* handler = new FittedImageHandler(owner);
    doc->documentLayout()->registerHandler(QTextFormat::ImageObject, handler);
    return handler;
}

FittedImageHandler::Geometry FittedImageHandler::geometry(QTextDocument* doc, int pos, const QTextFormat& format,
                                                          qreal dpr)
{
    Geometry g;
    auto img = format.toImageFormat();
    QString latex;
    bool display = false;
    if (parseMathResourceName(img.name(), &latex, &display)) {
        QFont font = formatFont(doc, format);
        auto m = renderMath(latex, display, QFontInfo(font).pixelSize() * 1.2, color(format), dpr);
        // Qt centres AlignMiddle objects on half the x-height; pad the box so that
        // the formula's own baseline lands on the text baseline.
        QFontMetricsF metrics(font);
        qreal halfX = metrics.xHeight() / 2;
        qreal over = m.ascent - m.descent - halfX;
        qreal padTop = over < 0 ? -over : 0, padBottom = over > 0 ? over : 0;
        if (img.verticalAlignment() != QTextCharFormat::AlignMiddle)
            padTop = padBottom = 0;
        g.size = QSizeF(m.width, m.ascent + m.descent + padTop + padBottom);
        g.top = padTop;
        // In a table, the cells' first lines line up on their text at the top of their row:
        // a formula reaches no higher above the line than the text does, so as not to push
        // its cell's text below the others'. What it has above that goes into the cell's
        // room at the top (the padding); only a taller formula makes its line taller.
        auto* table = qobject_cast<QTextTable*>(doc->frameAt(pos));  // (not a cursor: it would lay out)
        if (table && img.verticalAlignment() == QTextCharFormat::AlignMiddle) {
            qreal room = table->format().cellPadding();
            qreal ascent = std::max(m.descent + halfX, m.ascent - room);
            // with no text in its paragraph to hold the line up, as high as text would be
            static const QRegularExpression text(QString("[^\\s") + QChar::ObjectReplacementCharacter + "]");
            if (!doc->findBlock(pos).text().contains(text))
                ascent = std::max(ascent, metrics.ascent());
            g.size.setHeight(2 * ascent - halfX);  // as tall above the baseline as that, at AlignMiddle
            g.top = ascent - m.ascent;  // above the box when the formula is taller
        }
    } else {
        g.size = pictureSize(doc, img);
        // a row (or a picture being resized): as high as the row, and the row as a whole as
        // wide as the text at most
        auto block = doc->findBlock(pos);
        auto row = pictureRow(block);
        bool resizing = resizing_.height > 0 && resizing_.block == block.blockNumber() && (!row.empty() || resizing_.pos == pos);
        if (resizing && row.empty())  // one in a line of text
            g.size = QSizeF(resizing_.height * aspectOf(g.size), resizing_.height);
        if (row.size() > 1 || (resizing && !row.empty())) {
            qreal height = resizing ? resizing_.height : rowHeight(doc, block);
            g.gap = row.size() > 1 ? kPictureGap : 0;
            qreal widths = 0, gaps = 2 * g.gap * row.size();  // (the gaps stay as wide)
            for (int p : row)
                widths += height * aspectOf(pictureSize(doc, imageFormatAt(doc, p)));
            qreal fit = std::clamp<qreal>((availableWidth(doc, pos) - 1 - gaps) / std::max<qreal>(1, widths), 0.01, 1);
            g.size = QSizeF(height * aspectOf(g.size) * fit + 2 * g.gap, height * fit);
            return g;
        }
    }
    qreal avail = availableWidth(doc, pos);
    if (g.size.width() > avail) {
        g.scale = avail / g.size.width();
        g.size *= g.scale;
        g.top *= g.scale;
    }
    return g;
}

QSizeF FittedImageHandler::intrinsicSize(QTextDocument* doc, int pos, const QTextFormat& format)
{
    return geometry(doc, pos, format, qApp->devicePixelRatio()).size;
}

void FittedImageHandler::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int pos,
                                    const QTextFormat& format)
{
    qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : qApp->devicePixelRatio();
    auto g = geometry(doc, pos, format, dpr);
    auto img = format.toImageFormat();
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    QString latex;
    bool display = false;
    if (parseMathResourceName(img.name(), &latex, &display)) {
        QFont font = formatFont(doc, format);
        auto m = renderMath(latex, display, QFontInfo(font).pixelSize() * 1.2, color(format), dpr);
        painter->drawImage(QRectF(rect.left(), rect.top() + g.top, m.width * g.scale,
                                  (m.ascent + m.descent) * g.scale),
                           m.image);
    } else {
        QImage image = imageResource(doc, img.name());
        QRectF at = rect.adjusted(g.gap, 0, -g.gap, 0);
        if (image.isNull()) {
            painter->setPen(QPen(Qt::gray, 1, Qt::DashLine));
            painter->drawRect(at.adjusted(0.5, 0.5, -0.5, -0.5));
        } else {
            painter->drawImage(at, image);
        }
    }
    painter->restore();
}

QRectF FittedImageHandler::pictureRect(QTextDocument* doc, int pos)
{
    QTextBlock block = doc->findBlock(pos);
    // (laid out; where the block is in the note, in a table's cell too: its layout's
    // position is the cell's)
    auto origin = doc->documentLayout()->blockBoundingRect(block).topLeft();
    auto* layout = block.layout();
    if (!layout)
        return {};
    int at = pos - block.position();
    QTextLine line = layout->lineForTextPosition(at);
    if (!line.isValid())
        return {};
    auto format = imageFormatAt(doc, pos);
    if (!isPicture(format))
        return {};
    auto g = geometry(doc, pos, format, qApp->devicePixelRatio());
    // its left: where the cursor is before it or after it, the lesser (right to left, after
    // it is its left); were they not as far apart as it is wide (text both ways around it),
    // by the way its paragraph goes
    qreal x0 = line.cursorToX(at), x1 = line.cursorToX(at + 1);
    qreal left = std::abs(std::abs(x1 - x0) - g.size.width()) < 1 ? std::min(x0, x1)
                 : block.textDirection() == Qt::RightToLeft ? x0 - g.size.width()
                                                            : x0;
    qreal x = origin.x() + left;
    qreal baseline = origin.y() + line.y() + line.ascent();  // (a picture stands on it)
    return QRectF(x + g.gap, baseline - g.size.height(), g.size.width() - 2 * g.gap, g.size.height());
}

void FittedImageHandler::setResizing(QTextDocument* doc, int pos, qreal height)
{
    resizing_.block = doc->findBlock(pos).blockNumber();
    resizing_.pos = pos;
    resizing_.height = height;
}

} // namespace on::gui
