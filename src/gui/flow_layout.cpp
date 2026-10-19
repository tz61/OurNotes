#include "gui/flow_layout.h"

#include <QWidget>

#include <vector>

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
    // Items are narrowed to the available width, so only their height constrains us;
    // never force the parent wider (that would push content off-screen).
    int h = 0;
    for (auto* item : items_)
        h = std::max(h, item->minimumSize().height());
    auto m = contentsMargins();
    return QSize(m.left() + m.right(), h + m.top() + m.bottom());
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
    std::vector<std::pair<QLayoutItem*, QRect>> line;  // items of the current row
    auto finishLine = [&] {
        if (!testOnly)  // centre the items of a row vertically
            for (auto& [item, r] : line)
                item->setGeometry(r.translated(0, (lineHeight - r.height()) / 2));
        line.clear();
    };
    for (auto* item : items_) {
        if (item->isEmpty())  // explicitly hidden widgets take no space
            continue;
        QSize hint = item->sizeHint();
        hint.setWidth(std::min(hint.width(), std::max(1, area.width())));
        int nextX = x + hint.width() + spacing_;
        if (nextX - spacing_ > area.right() + 1 && lineHeight > 0) {
            finishLine();
            x = area.x();
            y += lineHeight + spacing_;
            nextX = x + hint.width() + spacing_;
            lineHeight = 0;
        }
        line.push_back({item, QRect(QPoint(x, y), hint)});
        x = nextX;
        lineHeight = std::max(lineHeight, hint.height());
    }
    finishLine();
    return y + lineHeight - rect.y() + m.bottom();
}

} // namespace fp::gui
