#include "gui/wrap_toolbar.h"

#include <QActionEvent>
#include <QFrame>
#include <QToolButton>

#include "gui/flow_layout.h"

namespace on::gui {

WrapToolBar::WrapToolBar(QWidget* parent) : QWidget(parent)
{
    flow_ = new FlowLayout(this, 2);
    flow_->setContentsMargins(4, 2, 4, 2);
    // at least one row, and as many as the buttons need at the width it gets
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
}

void WrapToolBar::addWidget(QWidget* widget)
{
    widget->setParent(this);
    flow_->addWidget(widget);
}

void WrapToolBar::addSeparator()
{
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setFixedSize(6, fontMetrics().height() + 4);
    flow_->addWidget(line);
}

QToolButton* WrapToolBar::buttonFor(QAction* action) const
{
    for (auto* b : findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
        if (b->defaultAction() == action)
            return b;
    return nullptr;
}

void WrapToolBar::actionEvent(QActionEvent* e)
{
    if (e->type() == QEvent::ActionAdded) {
        auto* b = new QToolButton(this);
        b->setDefaultAction(e->action());
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setFocusPolicy(Qt::NoFocus);  // keep the focus in the text
        flow_->addWidget(b);
    } else if (e->type() == QEvent::ActionRemoved) {
        if (auto* b = buttonFor(e->action())) {
            flow_->removeWidget(b);
            b->deleteLater();
        }
    }
    QWidget::actionEvent(e);
}

} // namespace on::gui
