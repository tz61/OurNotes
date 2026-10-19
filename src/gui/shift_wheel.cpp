#include "gui/shift_wheel.h"

#include <QApplication>
#include <QWheelEvent>

namespace on::gui {

void ShiftWheelScrolling::install()
{
    static ShiftWheelScrolling* filter = nullptr;
    if (filter)
        return;
    filter = new ShiftWheelScrolling(qApp);
    qApp->installEventFilter(filter);
}

bool ShiftWheelScrolling::eventFilter(QObject* watched, QEvent* e)
{
    if (e->type() != QEvent::Wheel)
        return false;
    auto* w = static_cast<QWheelEvent*>(e);
    auto others = Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    if (!(w->modifiers() & Qt::ShiftModifier) || (w->modifiers() & others) || w->angleDelta().x() != 0 ||
        w->angleDelta().y() == 0)
        return false;
    // down scrolls to the right, as in browsers
    QWheelEvent sideways(w->position(), w->globalPosition(), QPoint(w->pixelDelta().y(), 0),
                         QPoint(w->angleDelta().y(), 0), w->buttons(), w->modifiers() & ~Qt::ShiftModifier,
                         w->phase(), w->inverted(), w->source());
    QCoreApplication::sendEvent(watched, &sideways);  // goes up to the parents by itself
    e->accept();
    return true;
}

} // namespace on::gui
