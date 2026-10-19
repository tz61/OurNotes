#include "gui/theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

#include "gui/app_context.h"
#include "gui/common.h"

namespace fp::gui {

namespace {

QString g_systemStyle;  // the platform's default style, captured before any change

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
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // lets platforms that support it (title bars on Windows/macOS) follow along
    QGuiApplication::styleHints()->setColorScheme(a == Appearance::Dark    ? Qt::ColorScheme::Dark
                                                  : a == Appearance::Light ? Qt::ColorScheme::Light
                                                                           : Qt::ColorScheme::Unknown);
#endif
    if (a == Appearance::System) {
        if (QApplication::style()->name() != g_systemStyle)
            QApplication::setStyle(QStyleFactory::create(g_systemStyle));
        QApplication::setPalette(QPalette());  // back to the platform theme's palette
    } else {
        if (QApplication::style()->name().compare("fusion", Qt::CaseInsensitive) != 0)
            QApplication::setStyle(QStyleFactory::create("Fusion"));
        QApplication::setPalette(makePalette(a == Appearance::Dark));
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

} // namespace fp::gui
