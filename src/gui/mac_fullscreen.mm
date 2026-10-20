#include "gui/mac_fullscreen.h"

#include <QWidget>
#include <QWindow>

#include <algorithm>

#import <AppKit/AppKit.h>

namespace on::gui {

int fullScreenTitleBarOverlap(QWidget* window)
{
    if (!window->isFullScreen() || !window->windowHandle())
        return 0;
    NSWindow* own = reinterpret_cast<NSView*>(window->winId()).window;
    if (!own)
        return 0;
    // AppKit shows the title bar in a window of its own over the full-screen one; hidden,
    // it is transparent. Its alpha follows it fading in and out.
    NSRect frame = own.frame;
    double points = 0;
    for (NSWindow* w in NSApp.windows) {
        if (!w.isVisible || w.alphaValue <= 0 || ![NSStringFromClass([w class]) isEqualToString:@"NSToolbarFullScreenWindow"])
            continue;
        if (NSMaxX(w.frame) <= NSMinX(frame) || NSMinX(w.frame) >= NSMaxX(frame))
            continue;  // over another screen's window
        points = std::max(points, (NSMaxY(frame) - NSMinY(w.frame)) * w.alphaValue);
    }
    // points to the window's pixels, which the interface scale makes larger than points
    double pointsPerPixel = window->windowHandle()->devicePixelRatio() / own.backingScaleFactor;
    return pointsPerPixel > 0 ? std::max(0, int(points / pointsPerPixel + 0.5)) : 0;
}

} // namespace on::gui
