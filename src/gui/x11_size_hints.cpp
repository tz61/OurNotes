#include "gui/x11_size_hints.h"

#ifdef ON_HAVE_XCB

#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QWidget>

#include <cstdlib>
#include <cstring>
#include <xcb/xcb.h>

namespace on::gui {

namespace {

// WM_SIZE_HINTS (ICCCM 4.1.2.3): flags, x, y, width, height, min, max, increments, ...
constexpr int kHintsLength = 18;
constexpr int kFlags = 0, kWidthInc = 9, kHeightInc = 10;
constexpr uint32_t kResizeInc = 1 << 6;  // PResizeInc

void dropSteps(xcb_connection_t* c, xcb_window_t window)
{
    auto cookie = xcb_get_property(c, 0, window, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 0, kHintsLength);
    auto* reply = xcb_get_property_reply(c, cookie, nullptr);
    if (!reply)
        return;
    uint32_t hints[kHintsLength];
    if (reply->format == 32 && xcb_get_property_value_length(reply) >= (int)sizeof hints) {
        std::memcpy(hints, xcb_get_property_value(reply), sizeof hints);
        if ((hints[kFlags] & kResizeInc) && (hints[kWidthInc] > 1 || hints[kHeightInc] > 1)) {
            hints[kFlags] &= ~kResizeInc;
            hints[kWidthInc] = hints[kHeightInc] = 0;
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS,
                                32, kHintsLength, hints);
            xcb_flush(c);
        }
    }
    std::free(reply);
}

class SizeStepsFilter : public QAbstractNativeEventFilter {
public:
    explicit SizeStepsFilter(xcb_connection_t* c) : c_(c) {}

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override
    {
        if (type != "xcb_generic_event_t")
            return false;
        auto* e = static_cast<xcb_generic_event_t*>(message);
        if ((e->response_type & ~0x80) != XCB_PROPERTY_NOTIFY)
            return false;
        auto* p = reinterpret_cast<xcb_property_notify_event_t*>(e);
        if (p->atom == XCB_ATOM_WM_NORMAL_HINTS && p->state == XCB_PROPERTY_NEW_VALUE)
            dropSteps(c_, p->window);  // ours: Qt only hears of its own windows' properties
        return false;
    }

private:
    xcb_connection_t* c_;
};

xcb_connection_t* connection()
{
    auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
    if (!x11 || qGuiApp->devicePixelRatio() == 1.0)
        return nullptr;  // no steps at 100%
    return x11->connection();
}

} // namespace

void dropX11SizeSteps()
{
    if (auto* c = connection()) {
        static SizeStepsFilter filter(c);
        qGuiApp->installNativeEventFilter(&filter);
    }
}

void dropX11SizeSteps(QWidget* window)
{
    if (auto* c = connection(); c && window->windowHandle())
        dropSteps(c, (xcb_window_t)window->winId());
}

} // namespace on::gui

#else

namespace on::gui {
void dropX11SizeSteps() {}
void dropX11SizeSteps(QWidget*) {}
} // namespace on::gui

#endif
