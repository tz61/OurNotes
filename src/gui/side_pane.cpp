#include "gui/side_pane.h"

#include <QDataStream>
#include <QEvent>
#include <QPointer>
#include <QSplitter>
#include <QTabWidget>

#include <algorithm>
#include <vector>

#include "gui/app_context.h"

namespace on::gui {

namespace {

constexpr const char* kWidthKey = "/ui/sidePaneWidth";
constexpr const char* kTabKey = "/ui/itemPaneTab";  // "info" | "notes"

std::vector<QPointer<QSplitter>>& sharing()
{
    static std::vector<QPointer<QSplitter>> splits;
    return splits;
}

int sharedWidth() { return AppContext::get().config().get(kWidthKey, 0).get<int>(); }

// The widths `split` gives its widgets when there is room: as set or dragged to. Its
// sizes() are what they have now, less in a small window (at startup, before the window
// gets its size); set back as they were, those stayed, and the collections came back
// narrow every time. saveState() has the widths meant.
QList<int> intendedSizes(QSplitter* split)
{
    QDataStream ds(split->saveState());
    ds.setVersion(QDataStream::Qt_5_0);
    qint32 magic = 0, version = 0;
    QList<int> sizes;
    ds >> magic >> version >> sizes;
    if (sizes.size() != split->count() || std::any_of(sizes.begin(), sizes.end(), [](int s) { return s < 0; }))
        return split->sizes();
    return sizes;
}

// The pane `width` wide, the widget before it giving or taking the difference.
void setPaneWidth(QSplitter* split, int width)
{
    int n = split->count();
    if (width <= 0 || n < 2 || split->widget(n - 1)->isHidden())
        return;  // nothing shared yet, or the pane hidden
    auto sizes = intendedSizes(split);
    int diff = width - sizes[n - 1];
    if (diff == 0)
        return;
    sizes[n - 1] = width;
    sizes[n - 2] = std::max(1, sizes[n - 2] - diff);
    QSignalBlocker block(split);
    split->setSizes(sizes);
}

std::vector<QPointer<QTabWidget>>& tabSharing()
{
    static std::vector<QPointer<QTabWidget>> tabs;
    return tabs;
}

void showTab(QTabWidget* tabs, int index)
{
    tabs->setCurrentIndex(tabs->isTabEnabled(index) ? index : 1 - index);
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

void shareInfoNotesTab(QTabWidget* tabs)
{
    auto& all = tabSharing();
    all.erase(std::remove_if(all.begin(), all.end(), [](auto& t) { return t.isNull(); }), all.end());
    all.push_back(tabs);
    auto chosen = AppContext::get().config().get(kTabKey, "info").get<std::string>();
    showTab(tabs, chosen == "notes" ? 1 : 0);
    // a click, not any change: the pages deleted with a closed tab change it, too
    QObject::connect(tabs, &QTabWidget::tabBarClicked, tabs, [tabs](int index) {
        if (index < 0 || index > 1 || !tabs->isTabEnabled(index))
            return;
        AppContext::get().config().set(kTabKey, index == 1 ? "notes" : "info");
        for (auto& other : tabSharing())
            if (other && other != tabs)
                showTab(other, index);
    });
}

} // namespace on::gui
