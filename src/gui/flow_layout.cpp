#include "gui/flow_layout.h"

#include <QWidget>

#include <vector>

namespace on::gui {

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), spacing_(spacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() { clear(); }

// (invalidated, as Qt's layouts are: else one whose widget is hidden then, a tab not in
// front, kept its old layout when shown again, its new items at 640 x 480 over the rest)
void FlowLayout::addItem(QLayoutItem* item)
{
    items_.append(item);
    invalidate();
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= items_.size())
        return nullptr;
    invalidate();
    return items_.takeAt(index);
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

QSize FlowLayout::sizeHint() const
{
    // All items on one row. Qt asks heightForWidth() of this width for the preferred
    // height; a width of nothing would put every item on a row of its own, and for a
    // fixed-height bar (the tab strip, the note toolbar) that height becomes the
    // minimum: the window grew with every tab, beyond the screen.
    int w = 0, n = 0;
    for (auto* item : items_)
        if (!item->isEmpty()) {
            w += item->sizeHint().width();
            ++n;
        }
    auto m = contentsMargins();
    return QSize(w + std::max(0, n - 1) * spacing_ + m.left() + m.right(), minimumSize().height());
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
    auto finishLine = [&](bool last) {
        if (testOnly) {
            line.clear();
            return;
        }
        int widths = 0;
        for (auto& [item, r] : line)
            widths += r.width();
        int n = (int)line.size();
        int extra = std::max(0, area.width() - widths - (n - 1) * spacing_);
        // the last row too when it is mostly full; a few items would stretch too far
        if (justified_ && n > 0 && (!last || extra * 10 <= area.width() * 3)) {
            // share what is left of the row among its items, in proportion to their
            // widths (a short tab stays short), then place them again
            int given = 0, at = area.x();
            for (int i = 0; i < n; ++i) {
                auto& r = line[i].second;
                int add = i == n - 1 ? extra - given : int(qint64(extra) * r.width() / std::max(1, widths));
                given += add;
                r.setWidth(r.width() + add);
                r.moveLeft(at);
                at += r.width() + spacing_;
            }
        }
        for (auto& [item, r] : line)  // centre the items of a row vertically
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
            finishLine(false);
            x = area.x();
            y += lineHeight + spacing_;
            nextX = x + hint.width() + spacing_;
            lineHeight = 0;
        }
        line.push_back({item, QRect(QPoint(x, y), hint)});
        x = nextX;
        lineHeight = std::max(lineHeight, hint.height());
    }
    finishLine(true);
    return y + lineHeight - rect.y() + m.bottom();
}

} // namespace on::gui
