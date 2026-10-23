#include "gui/math_render.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>

#include <cmath>
#include <memory>

#include "latex.h"
#include "platform/qt/graphic_qt.h"

namespace on::gui {

namespace {

// MicroTeX loads its fonts from a resource folder at runtime.
QString findResources()
{
    QStringList candidates;
    if (qEnvironmentVariableIsSet("OURNOTES_MICROTEX_RES"))
        candidates << qEnvironmentVariable("OURNOTES_MICROTEX_RES");
    QString app = QCoreApplication::applicationDirPath();
    candidates << app + "/../share/ournotes/microtex"  // Linux install
               << app + "/microtex"                   // Windows
               << app + "/../Resources/microtex";     // macOS bundle
#ifdef OURNOTES_MICROTEX_BUILD_RES
    candidates << QStringLiteral(OURNOTES_MICROTEX_BUILD_RES);  // build tree
#endif
    for (auto& c : candidates)
        if (QFileInfo::exists(c + "/.clatexmath-res_root"))
            return QDir(c).absolutePath();
    return {};
}

QHash<QString, MathImage>& cache()
{
    static QHash<QString, MathImage> c;
    return c;
}

int g_state = 0;  // 0 untried, 1 ready, -1 unavailable

// MicroTeX keeps QFonts in globals; free them while the application still exists.
void releaseMicroTeX()
{
    cache().clear();
    if (g_state == 1)
        tex::LaTeX::release();
    g_state = 0;
}

bool initialise()
{
    if (g_state == 0) {
        auto res = findResources();
        g_state = -1;
        if (!res.isEmpty()) {
            try {
                tex::LaTeX::init(res.toStdString());
                g_state = 1;
                qAddPostRoutine(releaseMicroTeX);
            } catch (const std::exception&) {
            }
        }
    }
    return g_state == 1;
}

// Shows the LaTeX source when it cannot be rendered.
MathImage sourceImage(const QString& latex, bool display, qreal pixelSize, qreal dpr, const QString& error)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(std::max(8, int(pixelSize * 0.9)));
    QFontMetricsF fm(font);
    QString text = display ? "$$" + latex + "$$" : "$" + latex + "$";
    MathImage m;
    m.ok = false;
    m.error = error;
    m.width = std::ceil(fm.horizontalAdvance(text)) + 4;
    m.ascent = std::ceil(fm.ascent()) + 1;
    m.descent = std::ceil(fm.descent()) + 1;
    m.image = QImage(QSize(int(m.width * dpr), int((m.ascent + m.descent) * dpr)), QImage::Format_ARGB32_Premultiplied);
    m.image.setDevicePixelRatio(dpr);
    m.image.fill(QColor(220, 60, 60, 28));
    QPainter p(&m.image);
    p.setFont(font);
    p.setPen(QColor(200, 50, 50));
    p.drawText(QPointF(2, m.ascent), text);
    return m;
}

} // namespace

QString mathResourceName(const QString& latex, bool display)
{
    return QString(display ? "math:d:" : "math:i:") +
           QString::fromLatin1(latex.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool parseMathResourceName(const QString& name, QString* latex, bool* display)
{
    if (!name.startsWith("math:i:") && !name.startsWith("math:d:"))
        return false;
    if (display)
        *display = name.startsWith("math:d:");
    if (latex)
        *latex = QString::fromUtf8(QByteArray::fromBase64(name.mid(7).toLatin1(), QByteArray::Base64UrlEncoding));
    return true;
}

// MicroTeX crashes on a style command (\displaystyle, \textstyle, ...) followed by the end
// of a row or a cell (\\, \cr, \hline, &): as "\textstyle " before an inline formula that
// begins with \\ is. An empty group between them keeps it from that.
static QString guardStyles(QString tex)
{
    static const QRegularExpression re(
        R"((\\(?:display|text|script|scriptscript)style)(?![A-Za-z])(\s*)(?=\\\\|\\cr|\\hline|&))");
    return tex.replace(re, "\\1{}\\2");
}

MathImage renderMath(const QString& latex, bool display, qreal pixelSize, const QColor& color, qreal dpr)
{
    QString key = QString("%1|%2|%3|%4|%5").arg(display).arg(pixelSize).arg(color.rgba()).arg(dpr).arg(latex);
    auto it = cache().find(key);
    if (it != cache().end())
        return *it;
    if (cache().size() > 500)
        cache().clear();

    MathImage m;
    if (!initialise()) {
        m = sourceImage(latex, display, pixelSize, dpr, QObject::tr("LaTeX fonts not found"));
    } else {
        try {
            std::wstring tex = guardStyles((display ? QString() : QString("\\textstyle ")) + latex).toStdWString();
            std::unique_ptr<tex::TeXRender> r(
                tex::LaTeX::parse(tex, 100000, float(pixelSize), float(pixelSize) / 3.f, color.rgba()));
            int w = std::max(1, r->getWidth()), h = std::max(1, r->getHeight()), d = r->getDepth();
            // Ink reaches outside the TeX box (radicals, fraction parts, smoothing) and
            // MicroTeX rounds the box down to whole pixels: cut to the box, formulas lost
            // e.g. a subscript's tail. Draw with room around it and keep all the ink.
            int room = int(std::ceil(pixelSize * 0.5)) + 2;
            QImage canvas(QSize(int(std::ceil((w + 2 * room) * dpr)), int(std::ceil((h + 2 * room) * dpr))),
                          QImage::Format_ARGB32_Premultiplied);
            canvas.setDevicePixelRatio(dpr);
            canvas.fill(Qt::transparent);
            {
                QPainter p(&canvas);
                p.setRenderHint(QPainter::Antialiasing);
                p.setRenderHint(QPainter::TextAntialiasing);
                tex::Graphics2D_qt g2(&p);
                r->draw(g2, room, room);
            }
            QRect keep(QPoint(int(std::floor(room * dpr)), int(std::floor(room * dpr))),
                       QPoint(int(std::ceil((room + w) * dpr)) - 1, int(std::ceil((room + h) * dpr)) - 1));
            for (int y = 0; y < canvas.height(); ++y) {
                auto* line = reinterpret_cast<const QRgb*>(canvas.constScanLine(y));
                for (int x = 0; x < canvas.width(); ++x)
                    if (qAlpha(line[x]))
                        keep |= QRect(x - 1, y - 1, 3, 3);  // and a pixel around the ink
            }
            keep &= canvas.rect();
            m.image = canvas.copy(keep);
            m.image.setDevicePixelRatio(dpr);
            qreal baseline = (room + h - d) * dpr;  // in canvas pixels
            m.width = keep.width() / dpr;
            m.ascent = (baseline - keep.top()) / dpr;
            m.descent = (keep.bottom() + 1 - baseline) / dpr;
        } catch (const std::exception& e) {
            m = sourceImage(latex, display, pixelSize, dpr, QString::fromLocal8Bit(e.what()));
        }
    }
    cache().insert(key, m);
    return m;
}

} // namespace on::gui
