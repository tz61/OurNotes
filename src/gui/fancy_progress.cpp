#include "gui/fancy_progress.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace on::gui {

FancyProgressBar::FancyProgressBar(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(14);
    anim_.setDuration(450);
    anim_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        shown_ = v.toReal();
        update();
    });
    tick_.setInterval(16);
    connect(&tick_, &QTimer::timeout, this, [this] {
        phase_ = std::fmod(phase_ + 0.6, 1000.0);
        update();
    });
}

void FancyProgressBar::setMaximum(int maximum)
{
    maximum_ = std::max(1, maximum);
    setValue(value_);
}

void FancyProgressBar::setValue(int value)
{
    value_ = std::clamp(value, 0, maximum_);
    anim_.stop();
    anim_.setStartValue(shown_);
    anim_.setEndValue(qreal(value_) / maximum_);
    anim_.start();
}

void FancyProgressBar::setState(State state)
{
    state_ = state;
    updateTicking();
    update();
}

void FancyProgressBar::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    updateTicking();
}

void FancyProgressBar::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    tick_.stop();
}

void FancyProgressBar::updateTicking()
{
    if (state_ == State::Running && isVisible())
        tick_.start();
    else
        tick_.stop();
}

void FancyProgressBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    qreal h = r.height(), radius = h / 2;
    bool dark = palette().color(QPalette::Window).lightness() < 128;

    // track with a soft inner shadow
    QPainterPath track;
    track.addRoundedRect(r, radius, radius);
    p.fillPath(track, dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 22));
    QLinearGradient inner(r.topLeft(), r.bottomLeft());
    inner.setColorAt(0, QColor(0, 0, 0, dark ? 70 : 30));
    inner.setColorAt(0.5, QColor(0, 0, 0, 0));
    p.fillPath(track, inner);

    // fill: the gradient spans the whole track, so the colour shifts as it grows
    qreal w = shown_ <= 0 ? 0 : std::max(h, r.width() * shown_);
    if (w > 0) {
        QPainterPath fill;
        fill.addRoundedRect(QRectF(r.left(), r.top(), w, h), radius, radius);
        QColor a, b;
        switch (state_) {
        case State::Running:
            a = QColor("#3d7ff0");
            b = QColor("#22c1c3");
            break;
        case State::Done:
            a = QColor("#2fa85a");
            b = QColor("#62d38f");
            break;
        case State::Warning:
            a = QColor("#e8892b");
            b = QColor("#f5c542");
            break;
        }
        QLinearGradient g(r.topLeft(), r.topRight());
        g.setColorAt(0, a);
        g.setColorAt(1, b);
        p.fillPath(fill, g);

        p.save();
        p.setClipPath(fill);
        // moving diagonal stripes while running
        if (state_ == State::Running) {
            QColor stripe(255, 255, 255, 46);
            qreal step = h * 1.6;
            for (qreal x = r.left() - 2 * h + std::fmod(phase_, step); x < r.left() + w + h; x += step) {
                QPolygonF band;
                band << QPointF(x, r.bottom()) << QPointF(x + h * 0.8, r.bottom()) << QPointF(x + h * 1.6, r.top())
                     << QPointF(x + h * 0.8, r.top());
                p.setPen(Qt::NoPen);
                p.setBrush(stripe);
                p.drawPolygon(band);
            }
        }
        // gloss on the upper half
        QLinearGradient gloss(r.topLeft(), QPointF(r.left(), r.center().y()));
        gloss.setColorAt(0, QColor(255, 255, 255, 110));
        gloss.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillRect(QRectF(r.left(), r.top(), w, h / 2), gloss);
        p.restore();

        // glow at the leading edge
        if (state_ == State::Running && shown_ < 0.999) {
            p.save();
            p.setClipPath(track);
            QRadialGradient glow(QPointF(r.left() + w - radius, r.center().y()), h * 1.4);
            glow.setColorAt(0, QColor(255, 255, 255, 150));
            glow.setColorAt(1, QColor(255, 255, 255, 0));
            p.fillRect(QRectF(r.left() + w - h * 1.6, r.top(), h * 2.2, h), glow);
            p.restore();
        }
    }
    // thin outline for definition
    p.setPen(QPen(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 35), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(track);
}

} // namespace on::gui
