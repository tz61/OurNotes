#include "gui/close_shortcut.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QWidget>

namespace on::gui {

namespace {

class CloseShortcuts : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (e->type() != QEvent::Show || !watched->isWidgetType())
            return false;
        auto* w = static_cast<QWidget*>(watched);
        if (!w->isWindow() || w->inherits("on::gui::MainWindow") || w->property(kGiven).toBool())
            return false;
        auto type = w->windowType();
        if (type != Qt::Window && type != Qt::Dialog && type != Qt::Sheet && type != Qt::Tool)
            return false;  // menus, tooltips and other popups have their own ways
        w->setProperty(kGiven, true);
        auto* close = new QAction(w);
        auto keys = QKeySequence::keyBindings(QKeySequence::Close);
        if (!keys.contains(QKeySequence("Ctrl+W")))
            keys.prepend(QKeySequence("Ctrl+W"));
        close->setShortcuts(keys);
        close->setShortcutContext(Qt::WindowShortcut);
        connect(close, &QAction::triggered, w, &QWidget::close);  // as its title bar's button
        w->addAction(close);
        return false;
    }

private:
    static constexpr const char* kGiven = "_ournotesCloseShortcut";
};

} // namespace

void installCloseShortcuts()
{
    static CloseShortcuts* filter = nullptr;
    if (filter || !qApp)
        return;
    filter = new CloseShortcuts(qApp);
    qApp->installEventFilter(filter);
}

} // namespace on::gui
