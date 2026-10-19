#pragma once

#include <QColor>
#include <QObject>
#include <QTextObjectInterface>

class QTextDocument;

namespace fp::gui {

// Lays out images in rich text (notes): pictures wider than the text area are
// scaled down to fit, so there is never horizontal scrolling, and "math:" images
// are rendered from their LaTeX and sit on the text baseline.
class FittedImageHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    // Installs the handler on `doc` (re-install after replacing the document layout).
    static FittedImageHandler* install(QTextDocument* doc, QObject* owner);

    void setTextColor(const QColor& c) { textColor_ = c; }

    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument,
                    const QTextFormat& format) override;

private:
    using QObject::QObject;
    struct Geometry {
        QSizeF size;     // laid-out size
        qreal top = 0;   // math: offset of the formula inside the box
        qreal scale = 1;
    };
    Geometry geometry(QTextDocument* doc, int posInDocument, const QTextFormat& format, qreal dpr);

    QColor textColor_ = Qt::black;
};

} // namespace fp::gui
