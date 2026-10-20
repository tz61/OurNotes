#pragma once

#include <QtGlobal>

class QWidget;

namespace on::gui {

// In macOS full screen, a pointer at the top of the screen brings the menu bar and the
// window's title bar in over the window (they do not push it down). How far they reach
// into the window, in its pixels, as they show; 0 while they are hidden, and elsewhere.
#ifdef Q_OS_MACOS
int fullScreenTitleBarOverlap(QWidget* window);
#else
inline int fullScreenTitleBarOverlap(QWidget*) { return 0; }
#endif

} // namespace on::gui
