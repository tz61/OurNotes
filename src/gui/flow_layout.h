#pragma once

#include <QLayout>
#include <QList>

namespace on::gui {

// Lays out widgets left to right, wrapping to new lines (Qt's flow layout example).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 4);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override { return (int)items_.size(); }
    QLayoutItem* itemAt(int index) const override { return items_.value(index); }
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;
    void clear();

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    QList<QLayoutItem*> items_;
    int spacing_;
};

} // namespace on::gui
