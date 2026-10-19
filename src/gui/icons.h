#pragma once

#include <QIcon>
#include <QString>

namespace on::gui {

// Built-in SVG icons, so the application looks the same on every platform.
// Monochrome icons follow the palette's text colour (light and dark themes).
QIcon icon(const QString& name);
QIcon itemTypeIcon(const QString& itemType, const QString& contentType = {}, const QString& linkMode = {});
QIcon colorIcon(const QString& color, int size = 14, bool round = true);

} // namespace on::gui
