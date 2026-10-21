#pragma once

#include <QCursor>
#include <QIcon>
#include <QString>
#include <QStringList>

namespace on::gui {

// Built-in SVG icons, so the application looks the same on every platform.
// Monochrome icons follow the palette's text colour (light and dark themes).
QIcon icon(const QString& name);
QIcon itemTypeIcon(const QString& itemType, const QString& contentType = {}, const QString& linkMode = {});
QIcon colorIcon(const QString& color, int size = 14, bool round = true);
// A gear in the text colour with three colours inside it (the first at the upper left,
// the second at the upper right, the third below), drawn now: call it again after a
// change of theme.
QIcon paletteIcon(const QStringList& colors, int size = 18);
// The icon `name` as a mouse cursor, dark with a white rim (seen on dark pages as on
// white ones), at 24 px with its hot spot at hotSpot (in those pixels).
QCursor iconCursor(const QString& name, QPoint hotSpot, qreal devicePixelRatio);

} // namespace on::gui
