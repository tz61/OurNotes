#pragma once

#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

namespace fp::gui {

// Rounded progress bar with a gradient fill that eases to new values, animated
// stripes while running, a glossy highlight and a glow at the leading edge.
class FancyProgressBar : public QWidget {
    Q_OBJECT
public:
    enum class State { Running, Done, Warning };

    explicit FancyProgressBar(QWidget* parent = nullptr);

    void setMaximum(int maximum);
    void setValue(int value);  // animated
    void setState(State state);
    State state() const { return state_; }
    qreal fraction() const { return shown_; }

    QSize sizeHint() const override { return {420, 16}; }
    QSize minimumSizeHint() const override { return {80, 12}; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void updateTicking();

    int maximum_ = 1;
    int value_ = 0;
    qreal shown_ = 0;  // currently displayed fraction (animated)
    qreal phase_ = 0;  // stripe offset
    State state_ = State::Running;
    QVariantAnimation anim_;
    QTimer tick_;
};

} // namespace fp::gui
