#pragma once

#include <QColor>
#include <QString>

#include "core/config.h"

namespace fp::gui {

enum class Appearance { System, Light, Dark };

Appearance appearanceFromConfig(const Config& config);  // "/ui/appearance": system | light | dark
QString appearanceKey(Appearance appearance);

// Applies the appearance to the running application; can be called again to switch.
// System follows the platform theme (and its later changes); Light and Dark use the
// Fusion style with fixed palettes, so they look the same on every platform.
void applyAppearance(Appearance appearance);
bool isDarkTheme();

// Colour of formulas in notes: "/notes/formulaColor" if set, else the text colour.
QColor formulaColor(const QColor& textColor);

} // namespace fp::gui
