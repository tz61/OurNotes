#pragma once

#include <QObject>

namespace on::gui {

// Shift+wheel scrolls sideways everywhere (Qt otherwise pages up and down). macOS
// already turns it into a sideways wheel, which is left alone.
class ShiftWheelScrolling : public QObject {
public:
    static void install();  // on the application, once

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    using QObject::QObject;
};

} // namespace on::gui
