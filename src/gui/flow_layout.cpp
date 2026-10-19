#include "gui/flow_layout.h"

#include <QWidget>

namespace fp::gui {

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), spacing_(spacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() { clear(); }

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

QLayoutItem* FlowLayout::takeAt(int index)
{
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

void FlowLayout::clear()
{
    while (auto* item = takeAt(0)) {
        if (auto* w = item->widget())
            w->deleteLater();
        delete item;
    }
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (auto* item : items_)
        size = size.expandedTo(item->minimumSize());
    auto m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    auto m = contentsMargins();
    QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int x = area.x(), y = area.y(), lineHeight = 0;
    for (auto* item : items_) {
        if (item->widget() && !item->widget()->isVisible() && !testOnly)
            continue;
        QSize hint = item->sizeHint();
        int nextX = x + hint.width() + spacing_;
        if (nextX - spacing_ > area.right() + 1 && lineHeight > 0) {
            x = area.x();
            y += lineHeight + spacing_;
            nextX = x + hint.width() + spacing_;
            lineHeight = 0;
        }
        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), hint));
        x = nextX;
        lineHeight = std::max(lineHeight, hint.height());
    }
    return y + lineHeight - rect.y() + m.bottom();
}

} // namespace fp::gui
