#pragma once

#include <QWidget>

class QToolButton;

namespace fp::gui {

class FlowLayout;

// A toolbar whose buttons wrap onto more rows when it is narrow, instead of hiding
// them behind an overflow menu. Actions added with QWidget::addAction() become
// tool buttons; widgets and separators can be added too.
class WrapToolBar : public QWidget {
    Q_OBJECT
public:
    explicit WrapToolBar(QWidget* parent = nullptr);

    void addWidget(QWidget* widget);
    void addSeparator();
    QToolButton* buttonFor(QAction* action) const;

protected:
    void actionEvent(QActionEvent* e) override;

private:
    FlowLayout* flow_;
};

} // namespace fp::gui
