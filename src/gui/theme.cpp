#include "gui/theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QPainter>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>

#include "gui/app_context.h"
#include "gui/common.h"

namespace on::gui {

namespace {

QString g_systemStyle;  // the platform's default style, captured before any change
Appearance g_current = Appearance::System;

// Follow system with Windows' own style did not look like Light or Dark: windowsvista
// has no dark look at all, windows11 has a look of its own, and it covered the picture
// behind the item list. On Windows the system's light or dark is Light or Dark.
#ifdef Q_OS_WIN
constexpr bool kSystemAsLightOrDark = true;
#else
constexpr bool kSystemAsLightOrDark = false;
#endif

bool systemIsDark() { return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark; }

// The system turning light or dark while it is followed: the application follows, and so
// do what draws in its colours (dark PDF pages, notes, the picture behind the items).
void watchSystemScheme()
{
    static bool watching = false;
    if (watching)
        return;
    watching = true;
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
        if (g_current != Appearance::System)
            return;
        QTimer::singleShot(0, qApp, [] {  // after the platform's own palette change
            if (g_current != Appearance::System)
                return;
            applyAppearance(Appearance::System);
            if (auto* ctx = AppContext::tryGet())
                emit ctx->appearanceChanged();
        });
    });
}

QPalette makePalette(bool dark)
{
    struct Colors {
        const char *window, *windowText, *base, *alternateBase, *text, *button, *buttonText, *highlight,
            *highlightedText, *link, *linkVisited, *tooltipBase, *tooltipText, *placeholder, *light, *midlight, *mid,
            *darkColor, *shadow, *disabledText, *disabledHighlight;
    };
    static const Colors darkColors{"#2a2e32", "#fcfcfc", "#1b1e20", "#232629", "#fcfcfc", "#31363b", "#fcfcfc",
                                   "#3daee9", "#fcfcfc", "#1d99f3", "#9b59b6", "#31363b", "#fcfcfc", "#8f969d",
                                   "#454b52", "#3a3f45", "#4d545b", "#191b1d", "#0f1012", "#6e7175", "#2f4b5c"};
    static const Colors lightColors{"#eff0f1", "#232627", "#ffffff", "#f6f7f8", "#232627", "#fcfcfc", "#232627",
                                    "#3daee9", "#ffffff", "#2980b9", "#7f3fbf", "#f7f7f7", "#232627", "#7f8c8d",
                                    "#ffffff", "#f3f4f5", "#bfc4c9", "#a0a8b0", "#474a4c", "#a0a3a6", "#b8d9ec"};
    const auto& c = dark ? darkColors : lightColors;
    QPalette p;
    p.setColor(QPalette::Window, QColor(c.window));
    p.setColor(QPalette::WindowText, QColor(c.windowText));
    p.setColor(QPalette::Base, QColor(c.base));
    p.setColor(QPalette::AlternateBase, QColor(c.alternateBase));
    p.setColor(QPalette::Text, QColor(c.text));
    p.setColor(QPalette::Button, QColor(c.button));
    p.setColor(QPalette::ButtonText, QColor(c.buttonText));
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, QColor(c.highlight));
    p.setColor(QPalette::HighlightedText, QColor(c.highlightedText));
    p.setColor(QPalette::Link, QColor(c.link));
    p.setColor(QPalette::LinkVisited, QColor(c.linkVisited));
    p.setColor(QPalette::ToolTipBase, QColor(c.tooltipBase));
    p.setColor(QPalette::ToolTipText, QColor(c.tooltipText));
    p.setColor(QPalette::PlaceholderText, QColor(c.placeholder));
    p.setColor(QPalette::Light, QColor(c.light));
    p.setColor(QPalette::Midlight, QColor(c.midlight));
    p.setColor(QPalette::Mid, QColor(c.mid));
    p.setColor(QPalette::Dark, QColor(c.darkColor));
    p.setColor(QPalette::Shadow, QColor(c.shadow));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, QColor(c.disabledText));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(c.disabledHighlight));
    return p;
}

} // namespace

Appearance appearanceFromConfig(const Config& config)
{
    auto v = config.get("/ui/appearance", "system");
    auto s = v.is_string() ? v.get<std::string>() : std::string("system");
    return s == "dark" ? Appearance::Dark : s == "light" ? Appearance::Light : Appearance::System;
}

QString appearanceKey(Appearance a)
{
    return a == Appearance::Dark ? "dark" : a == Appearance::Light ? "light" : "system";
}

void applyAppearance(Appearance a)
{
    if (g_systemStyle.isEmpty())
        g_systemStyle = QApplication::style()->name();
    g_current = a;
    watchSystemScheme();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // lets platforms that support it (title bars on Windows/macOS) follow along
    QGuiApplication::styleHints()->setColorScheme(a == Appearance::Dark    ? Qt::ColorScheme::Dark
                                                  : a == Appearance::Light ? Qt::ColorScheme::Light
                                                                           : Qt::ColorScheme::Unknown);
#endif
    if (a == Appearance::System && !kSystemAsLightOrDark) {
        if (QApplication::style()->name() != g_systemStyle)
            QApplication::setStyle(QStyleFactory::create(g_systemStyle));
        QApplication::setPalette(QPalette());  // back to the platform theme's palette
    } else {
        if (QApplication::style()->name().compare("fusion", Qt::CaseInsensitive) != 0)
            QApplication::setStyle(QStyleFactory::create("Fusion"));
        bool dark = a == Appearance::Dark || (a == Appearance::System && systemIsDark());
        QApplication::setPalette(makePalette(dark));
    }
    // Qt passes the change to top-level widgets through their native windows, so
    // widgets never shown yet (dialogs, menus) would keep the old colours
    for (auto* w : QApplication::topLevelWidgets())
        if (!w->windowHandle()) {
            QEvent change(QEvent::ApplicationPaletteChange);
            QCoreApplication::sendEvent(w, &change);
        }
}

bool isDarkTheme() { return QApplication::palette().color(QPalette::Window).lightness() < 128; }

bool darkPdfPages()
{
    bool dark = isDarkTheme();
    auto* ctx = AppContext::tryGet();
    if (!ctx)
        return dark;
    auto v = ctx->config().get(dark ? "/reader/darkPages" : "/reader/darkPagesInLightMode", dark);
    return v.is_boolean() ? v.get<bool>() : dark;
}

void setDarkPdfPages(bool dark)
{
    auto& ctx = AppContext::get();
    ctx.config().set(isDarkTheme() ? "/reader/darkPages" : "/reader/darkPagesInLightMode", dark);
    ctx.config().save();
    emit ctx.appearanceChanged();
}

bool highlightPdfLinks()
{
    auto* ctx = AppContext::tryGet();
    auto v = ctx ? ctx->config().get("/reader/highlightLinks", true) : Json(true);
    return !v.is_boolean() || v.get<bool>();
}

void setHighlightPdfLinks(bool on)
{
    auto& ctx = AppContext::get();
    ctx.config().set("/reader/highlightLinks", on);
    ctx.config().save();
    emit ctx.appearanceChanged();
}

int pageTone(bool darkPages)
{
    auto* ctx = AppContext::tryGet();
    if (!ctx)
        return 0;
    auto v = ctx->config().get(darkPages ? "/reader/pageTone/dark" : "/reader/pageTone/light", 0);
    return v.is_number_integer() ? std::clamp(v.get<int>(), 0, 100) : 0;
}

void setPageTone(bool darkPages, int tone)
{
    auto& ctx = AppContext::get();
    ctx.config().set(darkPages ? "/reader/pageTone/dark" : "/reader/pageTone/light", std::clamp(tone, 0, 100));
    emit ctx.pageToneChanged();
}

QStringList annotationPalette()
{
    QStringList colors = {"#ffd400", "#ff6666", "#5fb236"};  // as kAnnotationColors begins
    auto* ctx = AppContext::tryGet();
    auto stored = ctx ? ctx->config().get("/reader/palette", Json()) : Json();
    for (int i = 0; stored.is_array() && i < 3 && i < (int)stored.size(); ++i)
        if (stored[i].is_string() && QColor(qs(stored[i].get<std::string>())).isValid())
            colors[i] = qs(stored[i].get<std::string>());
    return colors;
}

void setAnnotationPalette(const QStringList& colors)
{
    auto& ctx = AppContext::get();
    Json list = Json::array();
    for (auto& c : colors.mid(0, 3))
        list.push_back(ss(QColor(c).name()));
    ctx.config().set("/reader/palette", list);
    ctx.config().save();
    emit ctx.annotationPaletteChanged();
}

QColor pageToneColor(bool darkPages, int tone)
{
    constexpr int kGray = 128;  // where both ends meet
    int t = std::clamp(tone, 0, 100);
    int g = darkPages ? (kGray * t + 50) / 100 : 255 - ((255 - kGray) * t + 50) / 100;
    return QColor(g, g, g);
}

namespace {
// paper and ink after recolouring: black paper (the page colour lifts it towards gray, see
// pageToneColor), soft white ink
constexpr int kPaper[3] = {0, 0, 0};
constexpr int kInk[3] = {226, 226, 226};
} // namespace

QColor darkPageColor() { return QColor(kPaper[0], kPaper[1], kPaper[2]); }

void darkenPage(QImage& image, const std::vector<pdf::Rect>& keep, double scale, pdf::Size page)
{
    QImage original = image;  // shares the pixels until darkenPage() writes
    darkenPage(image);
    QPainter p(&image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    double pageArea = double(page.width) * page.height;
    for (auto& r : keep) {
        if (double(r.width()) * r.height() > 0.75 * pageArea)
            continue;
        QRect px = QRectF(r.x0 * scale, r.y0 * scale, r.width() * scale, r.height() * scale).toAlignedRect() &
                   image.rect();
        p.drawImage(px.topLeft(), original, px);
    }
}

void darkenPage(QImage& image)
{
    if (image.format() != QImage::Format_RGB888)
        image = image.convertToFormat(QImage::Format_RGB888);
    // Shifting every channel by 255 - max - min mirrors the HSL lightness and keeps
    // hue and saturation; the result is then mapped onto the paper..ink range.
    static const auto lut = [] {
        std::array<std::array<uchar, 256>, 3> t{};
        for (int c = 0; c < 3; ++c)
            for (int v = 0; v < 256; ++v)
                t[c][v] = uchar(kPaper[c] + (v * (kInk[c] - kPaper[c]) + 127) / 255);
        return t;
    }();
    for (int y = 0; y < image.height(); ++y) {
        uchar* px = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x, px += 3) {
            int r = px[0], g = px[1], b = px[2];
            int shift = 255 - std::max({r, g, b}) - std::min({r, g, b});
            px[0] = lut[0][r + shift];
            px[1] = lut[1][g + shift];
            px[2] = lut[2][b + shift];
        }
    }
}

QColor formulaColor(const QColor& textColor)
{
    if (auto* ctx = AppContext::tryGet()) {
        auto v = ctx->config().get("/notes/formulaColor", "");
        if (v.is_string()) {
            QColor c(qs(v.get<std::string>()));
            if (c.isValid())
                return c;
        }
    }
    return textColor;
}

} // namespace on::gui
