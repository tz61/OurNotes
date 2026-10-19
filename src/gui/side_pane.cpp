#include "gui/side_pane.h"

#include <QEvent>
#include <QPointer>
#include <QSplitter>

#include <algorithm>
#include <vector>

#include "gui/app_context.h"

namespace on::gui {

namespace {

constexpr const char* kWidthKey = "/ui/sidePaneWidth";

std::vector<QPointer<QSplitter>>& sharing()
{
    static std::vector<QPointer<QSplitter>> splits;
    return splits;
}

int sharedWidth() { return AppContext::get().config().get(kWidthKey, 0).get<int>(); }

// The pane `width` wide, the widget before it giving or taking the difference.
void setPaneWidth(QSplitter* split, int width)
{
    auto sizes = split->sizes();
    int n = (int)sizes.size();
    if (width <= 0 || n < 2 || sizes[n - 1] <= 0 || sizes[n - 2] <= 0)
        return;  // nothing shared yet, the pane hidden, or not laid out
    int diff = width - sizes[n - 1];
    if (diff == 0)
        return;
    sizes[n - 1] = width;
    sizes[n - 2] = std::max(1, sizes[n - 2] - diff);
    QSignalBlocker block(split);
    split->setSizes(sizes);
}

// Laid out (a tab shown, the window resized): the shared width.
class Follower : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (e->type() == QEvent::Show || e->type() == QEvent::Resize)
            setPaneWidth(static_cast<QSplitter*>(watched), sharedWidth());
        return false;
    }
};

} // namespace

void shareSidePaneWidth(QSplitter* split)
{
    auto& splits = sharing();
    splits.erase(std::remove_if(splits.begin(), splits.end(), [](auto& s) { return s.isNull(); }), splits.end());
    splits.push_back(split);
    split->installEventFilter(new Follower(split));
    QObject::connect(split, &QSplitter::splitterMoved, split, [split](int, int index) {
        if (index != split->count() - 1)
            return;  // another edge
        int width = split->sizes().back();
        if (width <= 0)
            return;
        AppContext::get().config().set(kWidthKey, width);
        for (auto& other : sharing())
            if (other && other != split)
                setPaneWidth(other, width);
    });
}

void applySidePaneWidth(QSplitter* split) { setPaneWidth(split, sharedWidth()); }

} // namespace on::gui
