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
#include <QTextDocument>
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

} // namespace

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
        QImage image = imageResource(doc, img.name());
        QSizeF natural = image.isNull() ? QSizeF(16, 16) : QSizeF(image.size()) / image.devicePixelRatio();
        qreal w = img.width(), h = img.height();
        if (w > 0 && h > 0)
            natural = QSizeF(w, h);
        else if (w > 0 && natural.width() > 0)
            natural = QSizeF(w, natural.height() * w / natural.width());
        else if (h > 0 && natural.height() > 0)
            natural = QSizeF(natural.width() * h / natural.height(), h);
        g.size = natural;
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
        if (image.isNull()) {
            painter->setPen(QPen(Qt::gray, 1, Qt::DashLine));
            painter->drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
        } else {
            painter->drawImage(rect, image);
        }
    }
    painter->restore();
}

} // namespace on::gui
