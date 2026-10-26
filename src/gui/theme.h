#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>

#include "core/config.h"
#include "pdf/document.h"

#include <vector>

namespace on::gui {

enum class Appearance { System, Light, Dark };

Appearance appearanceFromConfig(const Config& config);  // "/ui/appearance": system | light | dark
QString appearanceKey(Appearance appearance);

// Applies the appearance to the running application; can be called again to switch.
// System follows the platform theme (and its later changes); Light and Dark use the
// Fusion style with fixed palettes, so they look the same on every platform. On Windows,
// System is Light or Dark, whichever Windows is set to.
void applyAppearance(Appearance appearance);
bool isDarkTheme();

// Colour of formulas in notes: "/notes/formulaColor" if set, else the text colour.
QColor formulaColor(const QColor& textColor);

// Whether PDF pages are shown dark, a choice of the PDFs' own (L on a page, or the
// toolbar), kept for a dark and a light theme each: "/reader/darkPages" (dark by
// default) and "/reader/darkPagesInLightMode" (light by default).
bool darkPdfPages();
void setDarkPdfPages(bool dark);  // for the current theme; every PDF follows
// Links in PDFs marked, bold and (black ones) blue (on unless turned off); every PDF follows.
bool highlightPdfLinks();
void setHighlightPdfLinks(bool on);
// The page colour, 0..100, for light pages (white .. gray) and dark ones (black .. gray),
// shared by all PDFs: "/reader/pageTone/light" and "/reader/pageTone/dark".
int pageTone(bool darkPages);
void setPageTone(bool darkPages, int tone);
// The three colours of Q, W and E on a page (yellow, red and green to begin with), the
// same for all PDFs: "/reader/palette", in this computer's settings only.
QStringList annotationPalette();
void setAnnotationPalette(const QStringList& colors);  // every PDF follows
// The gray the pages are tinted with at that tone: multiplied onto light pages (white
// paper becomes it, black text stays), screened onto dark ones (black paper becomes it).
QColor pageToneColor(bool darkPages, int tone);
// Recolours a rendered page for dark mode: lightness is inverted and hues are kept, so
// black text on white becomes light text on a dark page while colours stay recognisable.
// Safe to call on any thread.
void darkenPage(QImage& image);
// The same, leaving the given areas (page space, in points) as they are: photos and
// figures keep their colours. An image covering most of the page (a scan, or a page
// background) is recoloured like the rest.
void darkenPage(QImage& image, const std::vector<pdf::Rect>& keep, double scale, pdf::Size page);
QColor darkPageColor();  // what white paper becomes

} // namespace on::gui
