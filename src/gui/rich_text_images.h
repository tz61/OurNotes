#pragma once

#include <QColor>
#include <QObject>
#include <QSizeF>
#include <QTextObjectInterface>

#include <functional>
#include <vector>

class QTextBlock;
class QTextDocument;
class QTextImageFormat;

namespace on::gui {

// Pictures in notes (images that are no formulas). One without a size of its own is shown
// small: within kDefaultPictureBox, never larger than it is. Pictures alone on a line are a
// row: all as high as the first, a little apart (kPictureGap on each side). Whatever is wider
// than the text is made smaller, a row as a whole, so that it stays on one line.
constexpr QSizeF kDefaultPictureBox(360, 240);
constexpr qreal kPictureGap = 4;
bool isPicture(const QTextFormat& format);
// The positions of a block's pictures when it has nothing else; empty otherwise.
std::vector<int> pictureRow(const QTextBlock& block);
// As large as it is (in logical pixels); empty when it cannot be read.
QSizeF naturalPictureSize(const QTextDocument* doc, const QTextImageFormat& format);
// Whether its shape is known: it has a size of its own, or it is here to be measured (a
// picture not downloaded yet is a placeholder, of no shape of its own).
bool pictureKnown(const QTextDocument* doc, const QTextImageFormat& format);
// As large as it is shown alone: its own size, else the default; before fitting the width.
QSizeF pictureSize(const QTextDocument* doc, const QTextImageFormat& format);
// The height of a row (that of its first picture, of the first known), before fitting the width.
qreal rowHeight(const QTextDocument* doc, const QTextBlock& block);
// The highest the picture at `pos` (with the rest of its row) can be shown and still fit in
// the width of the text.
qreal maxPictureHeight(QTextDocument* doc, int pos);

// Lays out images in rich text (notes): pictures wider than the text area are
// scaled down to fit, so there is never horizontal scrolling, and "math:" images
// are rendered from their LaTeX and sit on the text baseline.
class FittedImageHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    // Installs the handler on `doc` (re-install after replacing the document layout).
    static FittedImageHandler* install(QTextDocument* doc, QObject* owner);

    // Colour for formulas, queried when laying out and painting (follows theme changes).
    void setColorSource(std::function<QColor()> source) { colorSource_ = std::move(source); }

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument,
                    const QTextFormat& format) override;

    // Where the picture at `pos` is drawn, in the document's coordinates (no room around it).
    QRectF pictureRect(QTextDocument* doc, int pos);
    // While the picture at `pos` is being resized: it (and its row) shown that high; 0: as
    // stored again.
    void setResizing(QTextDocument* doc, int pos, qreal height);

private:
    using QObject::QObject;
    struct Geometry {
        QSizeF size;     // laid-out size
        qreal top = 0;   // math: offset of the formula inside the box
        qreal scale = 1;
        qreal gap = 0;  // a picture in a row: room on each side
    };
    Geometry geometry(QTextDocument* doc, int posInDocument, const QTextFormat& format, qreal dpr);

    // a formula's own colour (that of the text it was typed in), else the formula colour
    QColor color(const QTextFormat& format) const
    {
        if (format.hasProperty(QTextFormat::ForegroundBrush))
            return format.brushProperty(QTextFormat::ForegroundBrush).color();
        return colorSource_ ? colorSource_() : QColor(Qt::black);
    }
    std::function<QColor()> colorSource_;
    struct {
        int block = -1, pos = -1;
        qreal height = 0;
    } resizing_;
};

} // namespace on::gui
