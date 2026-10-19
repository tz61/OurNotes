#pragma once

#include <QColor>
#include <QImage>
#include <QString>

namespace on::gui {

struct MathImage {
    QImage image;     // device pixels (devicePixelRatio set)
    qreal width = 0;  // logical pixels
    qreal ascent = 0;   // above the baseline
    qreal descent = 0;  // below the baseline
    bool ok = true;     // false: the LaTeX could not be parsed (image shows the source)
    QString error;
};

// Renders LaTeX math with MicroTeX. GUI thread only; results are cached.
// Inline formulas use text style, display formulas display style.
MathImage renderMath(const QString& latex, bool display, qreal pixelSize, const QColor& color, qreal dpr);

// Notes keep formulas as document images named "math:i:<base64url>" (inline) or
// "math:d:<base64url>" (display), so the LaTeX travels with the image.
QString mathResourceName(const QString& latex, bool display);
bool parseMathResourceName(const QString& name, QString* latex, bool* display);

} // namespace on::gui
