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
#include <stdexcept>

// latex.h first: MicroTeX's headers include their "common.h" by that name, which MSVC looks
// for in the folders of the files including them, this one's too, and finds OurNotes'
// gui/common.h there. latex.h, beside MicroTeX's, has it included first.
#include "latex.h"
#include "core/formula.h"
#include "fonts/fonts.h"
#include "render.h"
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
                // \magnification, \DeclareMathSizes: every formula after it resized
                tex::DefaultTeXFont::enableMagnification(false);
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

// MicroTeX crashes on a style command (\displaystyle, \textstyle, ...) with nothing after it
// to style: followed by the end of a row or a cell (\\, \cr, \hline, &), of a group (}, \right)
// or of the formula. An empty group after it keeps it from that.
static QString guardStyles(QString tex)
{
    static const QRegularExpression re(
        R"((\\(?:display|text|script|scriptscript)style)(?![A-Za-z])(\s*)(?=\\\\|\\cr|\\hline|&|\}|\\right|$))");
    return tex.replace(re, "\\1{}\\2");
}

// An inline formula is one line, as MathJax has it: \\ and \newline outside an environment
// (matrix, cases...) and a group are a space in it. MicroTeX stacked what came after them
// under the rest, and the formula stood below the text of its line.
static QString oneLine(const QString& tex)
{
    QString out;
    int groups = 0, environments = 0;
    for (qsizetype i = 0; i < tex.size(); ++i) {
        if (tex[i] != '\\' || i + 1 >= tex.size()) {
            if (tex[i] == '{')
                ++groups;
            else if (tex[i] == '}')
                groups = std::max(0, groups - 1);
            out += tex[i];
            continue;
        }
        if (QStringView(tex).mid(i).startsWith(u"\\begin{"))
            ++environments;
        else if (QStringView(tex).mid(i).startsWith(u"\\end{"))
            environments = std::max(0, environments - 1);
        bool top = groups == 0 && environments == 0;
        if (top && tex[i + 1] == '\\') {  // \\, and its [length] if it has one
            i += 1;
            qsizetype j = i + 1;
            while (j < tex.size() && tex[j].isSpace())
                ++j;
            if (j < tex.size() && tex[j] == '[')
                if (auto close = tex.indexOf(']', j); close > 0)
                    i = close;
            out += ' ';
            continue;
        }
        if (top && QStringView(tex).mid(i).startsWith(u"\\newline") &&
            (i + 8 >= tex.size() || !tex[i + 8].isLetter())) {
            i += 7;
            out += ' ';
            continue;
        }
        out += tex[i];  // a command, or an escaped character (\{ is no group)
        out += tex[++i];
    }
    return out;
}

// Every { closed, and no } that opens nothing (\{ and \} are braces as text). MicroTeX
// takes a group left open at the end as closed, and showed half a formula as all of it.
static bool bracesBalanced(const QString& tex)
{
    int open = 0;
    for (qsizetype i = 0; i < tex.size(); ++i) {
        if (tex[i] == '\\') {
            ++i;  // an escaped character, or a command's first letter
        } else if (tex[i] == '{') {
            ++open;
        } else if (tex[i] == '}' && --open < 0) {
            return false;
        }
    }
    return open == 0;
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
            // as LaTeX::parse, but an inline formula in text style (not with "\textstyle "
            // before it, which made MicroTeX crash on some formulas), and with no width of
            // text (a width of 0): gather, multline and align spread over all of it, 100000
            // pixels then, and were shrunk to a dot to fit in a note
            if (!bracesBalanced(latex))
                throw std::runtime_error(QObject::tr("A { is not closed, or a } opens nothing").toStdString());
            tex::Formula formula;
            formula.setLaTeX(guardStyles(display ? latex : oneLine(latex)).toStdWString());
            tex::TeXRenderBuilder builder;
            std::unique_ptr<tex::TeXRender> r(builder.setStyle(display ? tex::TexStyle::display : tex::TexStyle::text)
                                                  .setTextSize(float(pixelSize))
                                                  .setWidth(tex::UnitType::pixel, 0, tex::Alignment::left)
                                                  .setLineSpace(tex::UnitType::pixel, float(pixelSize) / 3.f)
                                                  .setForeground(color.rgba())
                                                  .build(formula));
            int w = std::max(1, r->getWidth()), h = std::max(1, r->getHeight()), d = r->getDepth();
            // Ink reaches outside the TeX box (radicals, fraction parts, smoothing) and
            // MicroTeX rounds the box down to whole pixels: cut to the box, formulas lost
            // e.g. a subscript's tail. Draw with room around it and keep all the ink.
            int room = int(std::ceil(pixelSize * 0.5)) + 2;
            // \scalebox{1e6}{x} and the like: an image of hundreds of megabytes, or none
            qreal cw = std::ceil((w + 2.0 * room) * dpr), ch = std::ceil((h + 2.0 * room) * dpr);
            if (cw > 16384 || ch > 16384 || cw * ch > 32.0 * 1024 * 1024)
                throw std::runtime_error(QObject::tr("The formula is too large to show").toStdString());
            QImage canvas(QSize(int(cw), int(ch)), QImage::Format_ARGB32_Premultiplied);
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
