#include "gui/tab_widget.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>
#include <QToolTip>

#include "gui/flow_layout.h"

namespace on::gui {

// One tab of the strip: icon, elided title and a close button.
class TabButton : public QWidget {
public:
    TabButton(QTabWidget* tabs, QWidget* page, QWidget* parent) : QWidget(parent), tabs_(tabs), page_(page)
    {
        setObjectName("tabButton");
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    QWidget* page() const { return page_; }
    int index() const { return tabs_->indexOf(page_); }
    bool closable() const
    {
        int i = index();
        return i >= 0 && tabs_->tabsClosable() && tabs_->tabBar()->tabButton(i, QTabBar::RightSide);
    }
    QString text() const { return tabs_->tabText(index()); }

    void sync()
    {
        setToolTip(tabs_->tabToolTip(index()));
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        auto fm = fontMetrics();
        int w = kPad + kIcon + 6 + std::min(fm.horizontalAdvance(text()), kMaxText) + (closable() ? 6 + kClose : 0) + kPad;
        return {w, std::max(fm.height(), kIcon) + 14};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        bool current = tabs_->currentWidget() == page_;
        QColor bg = current ? palette().color(QPalette::Base) : palette().color(QPalette::Window);
        if (!current && underMouse())
            bg = palette().color(QPalette::Midlight);
        p.fillRect(rect(), bg);
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(QPointF(width() - 0.5, 4), QPointF(width() - 0.5, height() - 4));
        if (current)
            p.fillRect(QRectF(0, 0, width(), 2), palette().color(QPalette::Highlight));
        int x = kPad;
        tabs_->tabIcon(index()).paint(&p, QRect(x, (height() - kIcon) / 2, kIcon, kIcon));
        x += kIcon + 6;
        int right = width() - kPad - (closable() ? kClose + 6 : 0);
        p.setPen(palette().color(current ? QPalette::Text : QPalette::WindowText));
        p.drawText(QRect(x, 0, right - x, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(text(), Qt::ElideRight, right - x));
        if (closable() && (current || underMouse())) {
            QRectF c = closeRect();
            if (closeHot_) {
                p.setPen(Qt::NoPen);
                p.setBrush(palette().color(QPalette::Mid));
                p.drawEllipse(c);
            }
            p.setPen(QPen(palette().color(QPalette::WindowText), 1.4, Qt::SolidLine, Qt::RoundCap));
            QPointF m = c.center();
            double d = c.width() * 0.22;
            p.drawLine(m + QPointF(-d, -d), m + QPointF(d, d));
            p.drawLine(m + QPointF(d, -d), m + QPointF(-d, d));
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton && closable()) {
            emit tabs_->tabCloseRequested(index());
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        pressPos_ = e->position().toPoint();
        pressedClose_ = closable() && closeRect().contains(e->position());
        if (!pressedClose_)
            tabs_->setCurrentWidget(page_);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        bool hot = closable() && closeRect().contains(e->position());
        if (hot != closeHot_) {
            closeHot_ = hot;
            update();
        }
        // drag to reorder; the first tab (the library) stays first
        if (!(e->buttons() & Qt::LeftButton) || pressedClose_ || index() == 0 ||
            (e->position().toPoint() - pressPos_).manhattanLength() < QApplication::startDragDistance())
            return;
        auto* strip = parentWidget();
        auto* target = strip->childAt(mapTo(strip, e->position().toPoint()));
        while (target && target->parentWidget() != strip)
            target = target->parentWidget();
        if (auto* other = dynamic_cast<TabButton*>(target); other && other != this && other->index() > 0)
            tabs_->tabBar()->moveTab(index(), other->index());
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && pressedClose_ && closeRect().contains(e->position()))
            emit tabs_->tabCloseRequested(index());
        pressedClose_ = false;
    }

    void leaveEvent(QEvent*) override
    {
        closeHot_ = false;
        update();
    }

    void contextMenuEvent(QContextMenuEvent* e) override
    {
        QMenu menu(this);
        int i = index();
        auto closeWhere = [this](auto pred) {
            for (int k = tabs_->count() - 1; k > 0; --k)
                if (pred(k))
                    emit tabs_->tabCloseRequested(k);
        };
        menu.addAction(tr("Close Tab"), this, [this] { emit tabs_->tabCloseRequested(index()); })->setEnabled(closable());
        menu.addAction(tr("Close Other Tabs"), this, [closeWhere, page = page_, this] {
            closeWhere([&](int k) { return tabs_->widget(k) != page; });
        });
        menu.addAction(tr("Close Tabs to the Right"), this, [closeWhere, i] { closeWhere([i](int k) { return k > i; }); })
            ->setEnabled(i < tabs_->count() - 1);
        menu.exec(e->globalPos());
    }

private:
    static constexpr int kPad = 10, kIcon = 16, kClose = 16, kMaxText = 220;
    QRectF closeRect() const { return QRectF(width() - kPad - kClose, (height() - kClose) / 2.0, kClose, kClose); }

    QTabWidget* tabs_;
    QWidget* page_;
    QPoint pressPos_;
    bool pressedClose_ = false;
    bool closeHot_ = false;
};

// The rows of tabs.
class TabStrip : public QWidget {
public:
    TabStrip(QTabWidget* tabs, QWidget* parent) : QWidget(parent), tabs_(tabs)
    {
        setObjectName("tabStrip");
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 4, 0);
        row->setSpacing(4);
        area_ = new QWidget(this);
        flow_ = new FlowLayout(area_, 0);
        row->addWidget(area_, 1);
        corner_ = new QHBoxLayout;
        row->addLayout(corner_);
        row->setAlignment(corner_, Qt::AlignTop);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void addCornerWidget(QWidget* w)
    {
        w->setParent(this);
        corner_->addWidget(w);
    }

    // Coalesces the many changes of one operation (insert, then set the tooltip).
    void scheduleRefresh()
    {
        if (queued_)
            return;
        queued_ = true;
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    }

    // Buttons are kept per page, so a tab being dragged keeps the mouse while the
    // order changes.
    void refresh()
    {
        queued_ = false;
        QList<TabButton*> ordered;
        for (int i = 0; i < tabs_->count(); ++i) {
            TabButton* button = nullptr;
            for (auto* b : buttons_)
                if (b->page() == tabs_->widget(i))
                    button = b;
            if (!button)
                button = new TabButton(tabs_, tabs_->widget(i), area_);
            ordered << button;
        }
        for (auto* b : buttons_)
            if (!ordered.contains(b))
                b->deleteLater();
        while (auto* item = flow_->takeAt(0))
            delete item;
        for (auto* b : ordered) {
            flow_->addWidget(b);
            b->sync();
            b->show();
        }
        buttons_ = ordered;
        updateGeometry();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

private:
    QTabWidget* tabs_;
    QWidget* area_;  // the tabs, in wrapping rows
    FlowLayout* flow_;
    QHBoxLayout* corner_;
    QList<TabButton*> buttons_;
    bool queued_ = false;
};

TabWidget::TabWidget(QWidget* parent) : QTabWidget(parent)
{
    strip_ = new TabStrip(this, parent);
    tabBar()->hide();
    connect(this, &QTabWidget::currentChanged, strip_, &TabStrip::scheduleRefresh);
    connect(tabBar(), &QTabBar::tabMoved, strip_, &TabStrip::refresh);
}

QWidget* TabWidget::tabStrip() const { return strip_; }

void TabWidget::addStripCornerWidget(QWidget* widget) { strip_->addCornerWidget(widget); }

void TabWidget::tabInserted(int index)
{
    QTabWidget::tabInserted(index);
    tabBar()->hide();
    strip_->scheduleRefresh();
}

void TabWidget::tabRemoved(int index)
{
    QTabWidget::tabRemoved(index);
    strip_->scheduleRefresh();
}

} // namespace on::gui
