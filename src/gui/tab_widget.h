#pragma once

#include <QTabWidget>

namespace fp::gui {

class TabStrip;

// A QTabWidget whose tabs wrap onto more rows when they do not fit (like VS Code's
// wrapped tabs) instead of scrolling in one row. Its own tab bar is hidden but stays
// the model: text, icon, tooltip, order and closability are read from it. Place
// tabStrip() above the widget.
class TabWidget : public QTabWidget {
    Q_OBJECT
public:
    explicit TabWidget(QWidget* parent = nullptr);
    QWidget* tabStrip() const;
    // A widget on the right of the strip's first row, e.g. buttons that apply to all tabs.
    void addStripCornerWidget(QWidget* widget);

protected:
    void tabInserted(int index) override;
    void tabRemoved(int index) override;

private:
    TabStrip* strip_;
};

} // namespace fp::gui
