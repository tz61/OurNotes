#pragma once

class QWidget;

namespace on::gui {

// At an interface scale above 100% (QT_SCALE_FACTOR), Qt asks the X11 window manager to
// size its windows in steps of the scaled pixel: 2 pixels at 150-200%, 3 at 250-300%.
// KWin keeps to them even for a maximized window, which then ends a pixel or two short of
// the screen's edge, and on every change of the minimum size it rounds a window down
// another step. Installed once, this takes the steps out of every window's size hints
// as Qt sets them. Does nothing elsewhere than on X11, or at 100%.
void dropX11SizeSteps();
// The same for one window, at once: after a change that has Qt set its size hints again
// (its minimum size), and before it is maximized, so the window manager goes by hints
// without steps.
void dropX11SizeSteps(QWidget* window);

} // namespace on::gui
