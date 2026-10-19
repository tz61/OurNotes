#include "gui/math_render.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>

#include <cmath>
#include <memory>

#include "latex.h"
#include "platform/qt/graphic_qt.h"

namespace fp::gui {

namespace {

// MicroTeX loads its fonts from a resource folder at runtime.
QString findResources()
{
    QStringList candidates;
    if (qEnvironmentVariableIsSet("FREEPEN_MICROTEX_RES"))
        candidates << qEnvironmentVariable("FREEPEN_MICROTEX_RES");
    QString app = QCoreApplication::applicationDirPath();
    candidates << app + "/../share/freepen/microtex"  // Linux install
               << app + "/microtex"                   // Windows
               << app + "/../Resources/microtex";     // macOS bundle
#ifdef FREEPEN_MICROTEX_BUILD_RES
    candidates << QStringLiteral(FREEPEN_MICROTEX_BUILD_RES);  // build tree
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
            std::wstring tex = ((display ? QString() : QString("\\textstyle ")) + latex).toStdWString();
            std::unique_ptr<tex::TeXRender> r(
                tex::LaTeX::parse(tex, 100000, float(pixelSize), float(pixelSize) / 3.f, color.rgba()));
            int w = std::max(1, r->getWidth()), h = std::max(1, r->getHeight()), d = r->getDepth();
            m.width = w;
            m.descent = d;
            m.ascent = h - d;
            m.image = QImage(QSize(int(std::ceil(w * dpr)), int(std::ceil(h * dpr))),
                             QImage::Format_ARGB32_Premultiplied);
            m.image.setDevicePixelRatio(dpr);
            m.image.fill(Qt::transparent);
            QPainter p(&m.image);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::TextAntialiasing);
            tex::Graphics2D_qt g2(&p);
            r->draw(g2, 0, 0);
        } catch (const std::exception& e) {
            m = sourceImage(latex, display, pixelSize, dpr, QString::fromLocal8Bit(e.what()));
        }
    }
    cache().insert(key, m);
    return m;
}

} // namespace fp::gui
