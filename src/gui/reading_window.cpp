#include "gui/reading_window.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QTabWidget>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "core/library.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/reader/reader_tab.h"
#include "gui/reading_timer.h"

namespace on::gui {

namespace {

int64_t startOf(QDate day) { return day.startOfDay().toSecsSinceEpoch(); }

QColor dimmed(QColor c, double alpha)
{
    c.setAlphaF(alpha);
    return c;
}

QString clock(int64_t seconds)
{
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds).time(), QLocale::ShortFormat);
}

QString span(const ReadingSession& s) { return clock(s.start) + " – " + clock(s.end); }

// The title of the paper a session was in as it is now in the library, or as it was then.
QString paperTitle(const ReadingSession& s)
{
    auto& lib = AppContext::get().library();
    if (auto info = lib.libraryByUid(s.library)) {
        auto title = ReaderTab::titleFor(info->id, qs(s.attachment));
        if (!title.isEmpty())
            return title;
    }
    return s.title.empty() ? QObject::tr("(unknown)") : qs(s.title);
}

enum { LibraryRole = Qt::UserRole + 1, KeyRole };

// How reading is counted, for the pause set.
QString howCounted()
{
    int minutes = ReadingTimer::idleSeconds() / 60;
    return QObject::tr("Reading counts while a PDF is shown in OurNotes, in front, until %1 pass without mouse or "
                       "keyboard input; the time after the last input is not counted.")
        .arg(minutes == 1 ? QObject::tr("a minute") : QObject::tr("%1 minutes").arg(minutes));
}

} // namespace

// --- the days' bars ----------------------------------------------------------------------------

class DaysChart : public QWidget {
public:
    using QWidget::QWidget;
    std::vector<std::pair<QDate, int64_t>> days;  // oldest first
    QDate selected;
    std::function<void(QDate)> picked;

    QSize sizeHint() const override
    {
        int h = fontMetrics().height();
        return {h * 26, h * 7};
    }

protected:
    QRectF column(int i, const QRectF& area) const
    {
        double w = area.width() / std::max<size_t>(days.size(), 1);
        return {area.left() + i * w, area.top(), w, area.height()};
    }
    QRectF area() const
    {
        int h = fontMetrics().height();
        return QRectF(rect()).adjusted(0, h, 0, -h - 4);
    }
    int dayAt(QPointF p) const
    {
        auto a = area();
        for (int i = 0; i < (int)days.size(); ++i)
            if (p.x() >= column(i, a).left() && p.x() < column(i, a).right())
                return i;
        return -1;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        auto a = area();
        int64_t most = 3600;
        for (auto& [day, seconds] : days)
            most = std::max(most, seconds);
        int64_t hours = (most + 3599) / 3600;
        auto faint = palette().color(QPalette::PlaceholderText);
        // the scale: a line at the top, at so many hours
        p.setPen(QPen(dimmed(faint, 0.5), 1, Qt::DotLine));
        p.drawLine(QPointF(a.left(), a.top() + 0.5), QPointF(a.right(), a.top() + 0.5));
        p.setPen(faint);
        p.drawText(QRectF(a.left(), 0, a.width(), a.top()), Qt::AlignRight | Qt::AlignVCenter, tr("%1 h").arg(hours));
        auto accent = palette().color(QPalette::Highlight);
        auto today = QDate::currentDate();
        for (int i = 0; i < (int)days.size(); ++i) {
            auto [day, seconds] = days[i];
            auto c = column(i, a);
            double barWidth = std::max(3.0, c.width() * 0.6);
            double h = a.height() * seconds / double(hours * 3600);
            QRectF bar(c.center().x() - barWidth / 2, a.bottom() - h, barWidth, h);
            if (seconds > 0)
                p.fillRect(bar, day == selected ? accent : dimmed(accent, 0.45));
            else
                p.fillRect(QRectF(bar.left(), a.bottom() - 1, bar.width(), 1), dimmed(faint, 0.6));
            QFont f = font();
            f.setBold(day == selected);
            p.setFont(f);
            p.setPen(day == today || day == selected ? palette().color(QPalette::Text) : faint);
            p.drawText(QRectF(c.left(), a.bottom() + 4, c.width(), height() - a.bottom() - 4),
                       Qt::AlignHCenter | Qt::AlignTop, QString::number(day.day()));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        int i = dayAt(e->position());
        if (i >= 0 && picked)
            picked(days[i].first);
    }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(e);
            int i = dayAt(help->pos());
            if (i >= 0)
                QToolTip::showText(help->globalPos(),
                                   QLocale().toString(days[i].first, QLocale::LongFormat) + "\n" +
                                       readingDuration(days[i].second),
                                   this);
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }
};

// --- a day's sessions on a line through it -----------------------------------------------------

class DayLine : public QWidget {
public:
    using QWidget::QWidget;
    QDate day;
    std::vector<ReadingSession> sessions;

    QSize sizeHint() const override
    {
        int h = fontMetrics().height();
        return {h * 26, h * 3};
    }

protected:
    QRectF track() const
    {
        int h = fontMetrics().height();
        return QRectF(rect()).adjusted(1, 2, -1, 0).adjusted(0, 0, 0, -(height() - 2 - h));
    }
    QRectF block(const ReadingSession& s) const
    {
        auto t = track();
        double from = startOf(day), length = startOf(day.addDays(1)) - from;
        double a = (std::max<double>(s.start, from) - from) / length, b = (std::min<double>(s.end, from + length) - from) / length;
        double x = t.left() + a * t.width(), w = std::max(2.0, (b - a) * t.width());
        return {x, t.top(), w, t.height()};
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        auto t = track();
        auto faint = palette().color(QPalette::PlaceholderText);
        p.setPen(Qt::NoPen);
        p.setBrush(dimmed(faint, 0.18));
        p.drawRoundedRect(t, 3, 3);
        p.setBrush(palette().color(QPalette::Highlight));
        for (auto& s : sessions)
            p.drawRect(block(s));
        // the hours: a mark for each, a time every six
        p.setPen(dimmed(faint, 0.8));
        for (int hour = 0; hour <= 24; ++hour) {
            double x = t.left() + t.width() * hour / 24.0;
            p.drawLine(QPointF(x, t.bottom() + 1), QPointF(x, t.bottom() + (hour % 6 ? 3 : 6)));
            if (hour % 6 == 0) {
                auto text = QLocale().toString(QTime(hour % 24, 0), QLocale::ShortFormat);
                int w = fontMetrics().horizontalAdvance(text);
                double left = std::clamp(x - w / 2.0, 0.0, width() - w - 1.0);
                p.drawText(QRectF(left, t.bottom() + 6, w + 1, fontMetrics().height()), Qt::AlignLeft, text);
            }
        }
    }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(e);
            for (auto& s : sessions)
                if (block(s).adjusted(-2, -2, 2, 2).contains(help->pos())) {
                    QToolTip::showText(help->globalPos(),
                                       span(s) + "  ·  " + readingDuration(s.seconds()) + "\n" + paperTitle(s), this);
                    return true;
                }
            QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }
};

// --- the status bar's -------------------------------------------------------------------------

ReadingStatusButton::ReadingStatusButton(ReadingTimer* timer, QWidget* parent) : QToolButton(parent), timer_(timer)
{
    setObjectName("readingTime");
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setFocusPolicy(Qt::NoFocus);
    connect(timer_, &ReadingTimer::changed, this, &ReadingStatusButton::refresh);
    later_.setSingleShot(true);
    later_.setInterval(300);
    connect(&later_, &QTimer::timeout, this, &ReadingStatusButton::refresh);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64, const QString& type, const QString&, const QStringList&) {
                if (type == "setting" || type == "*")
                    later_.start();
            });
    connect(&AppContext::get(), &AppContext::readingSettingsChanged, this, &ReadingStatusButton::refresh);
    minute_.setInterval(60000);
    connect(&minute_, &QTimer::timeout, this, &ReadingStatusButton::refresh);
    minute_.start();
    refresh();
}

void ReadingStatusButton::refresh()
{
    auto& cfg = AppContext::get().config();
    bool shown = ReadingTimer::enabled() && cfg.get("/reading/showInStatusBar", true).get<bool>();
    setVisible(shown);
    if (!shown)
        return;
    setText(tr("Read today: %1").arg(readingDuration(timer_->secondsOn(QDate::currentDate()))));
    auto pal = QToolButton::palette();
    pal.setColor(QPalette::ButtonText, palette().color(QPalette::Active, timer_->reading() ? QPalette::WindowText
                                                                                            : QPalette::PlaceholderText));
    setPalette(pal);
    QString state;
    if (auto s = timer_->session())
        state = tr("Counting: “%1”, %2 so far.").arg(asPlainText(paperTitle(*s)), readingDuration(s->seconds()));
    else
        state = tr("Paused.") + " " + howCounted();
    setToolTip(state + "\n" + tr("Click to see your reading time."));
}

// --- the window -------------------------------------------------------------------------------

ReadingWindow::ReadingWindow(ReadingTimer* timer, QWidget* parent) : QDialog(parent), timer_(timer)
{
    setWindowTitle(tr("Reading Time"));
    resize(760, 640);
    auto* v = new QVBoxLayout(this);
    totals_ = new QLabel(this);
    totals_->setObjectName("readingTotals");
    totals_->setTextFormat(Qt::PlainText);
    v->addWidget(totals_);
    days_ = new DaysChart(this);
    days_->setObjectName("readingDays");
    days_->picked = [this](QDate day) { selectDay(day); };
    v->addWidget(days_);
    dayTitle_ = new QLabel(this);
    dayTitle_->setObjectName("readingDay");
    dayTitle_->setTextFormat(Qt::PlainText);
    QFont bold = dayTitle_->font();
    bold.setBold(true);
    dayTitle_->setFont(bold);
    v->addSpacing(fontMetrics().height() / 2);
    v->addWidget(dayTitle_);
    line_ = new DayLine(this);
    line_->setObjectName("readingDayLine");
    v->addWidget(line_);

    auto* tabs = new QTabWidget(this);
    list_ = new QTreeWidget(tabs);
    list_->setObjectName("readingSessions");
    list_->setHeaderLabels({tr("Time"), tr("Read"), tr("Paper"), tr("Computer")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    tabs->addTab(list_, tr("Sessions"));
    papers_ = new QTreeWidget(tabs);
    papers_->setObjectName("readingPapers");
    papers_->setHeaderLabels({tr("Paper"), tr("Read"), tr("Sessions"), tr("Last read")});
    papers_->setRootIsDecorated(false);
    papers_->setUniformRowHeights(true);
    papers_->header()->setStretchLastSection(false);
    papers_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 4; ++c)
        papers_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    tabs->addTab(papers_, tr("Most Read, %1 Days").arg(kPaperDays));
    v->addWidget(tabs, 1);
    for (auto* tree : {list_, papers_})
        connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
            auto key = item->data(0, KeyRole).toString();
            if (!key.isEmpty())
                emit openRequested(item->data(0, LibraryRole).toLongLong(), key);
        });

    auto* hint = new QLabel(this);
    hint->setObjectName("readingHint");
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(hint);
    auto setHint = [hint] {
        auto& lib = AppContext::get().library();
        auto id = readingLogLibrary(lib);
        auto info = id ? lib.library(*id) : std::nullopt;
        hint->setText(howCounted() + " " + (info ? tr("Kept in “%1” and synced with it.").arg(asPlainText(qs(info->name)))
                                        : tr("Not kept: there is no library of your own.")));
    };
    setHint();
    connect(&AppContext::get(), &AppContext::readingSettingsChanged, this, setHint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    v->addWidget(buttons);

    later_.setSingleShot(true);
    later_.setInterval(300);
    connect(&later_, &QTimer::timeout, this, [this] {
        if (isVisible())
            reload();
    });
    connect(timer_, &ReadingTimer::changed, &later_, qOverload<>(&QTimer::start));
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64, const QString& type, const QString&, const QStringList&) {
                if (type == "setting" || type == "*" || type == "item")
                    later_.start();
            });
    reload();
}

void ReadingWindow::reload()
{
    auto today = QDate::currentDate();
    sessions_ = timer_->sessions(today.addDays(1 - kPaperDays), today);
    if (!selected_.isValid() || selected_ > today || selected_ <= today.addDays(-kDays))
        selected_ = today;
    days_->days.clear();
    for (int i = kDays - 1; i >= 0; --i) {
        auto day = today.addDays(-i);
        days_->days.emplace_back(day, readingSeconds(sessions_, startOf(day), startOf(day.addDays(1))));
    }
    auto end = startOf(today.addDays(1));
    totals_->setText(tr("Today %1    Last 7 days %2    Last %3 days %4")
                         .arg(readingDuration(readingSeconds(sessions_, startOf(today), end)),
                              readingDuration(readingSeconds(sessions_, startOf(today.addDays(-6)), end)))
                         .arg(kPaperDays)
                         .arg(readingDuration(readingSeconds(sessions_, startOf(today.addDays(1 - kPaperDays)), end))));

    // the papers read most: their time in these days, with the PDF last read of each
    struct Paper {
        int64_t seconds = 0;
        int sessions = 0;
        int64_t last = 0;
        ReadingSession latest;
    };
    std::map<std::pair<std::string, std::string>, Paper> papers;
    int64_t from = startOf(today.addDays(1 - kPaperDays));
    for (auto& s : sessions_) {
        auto& p = papers[{s.library, s.item}];
        p.seconds += readingSeconds({s}, from, end);
        ++p.sessions;
        if (s.end >= p.last) {
            p.last = s.end;
            p.latest = s;
        }
    }
    std::vector<const Paper*> order;
    for (auto& [key, p] : papers)
        if (p.seconds > 0)
            order.push_back(&p);
    std::stable_sort(order.begin(), order.end(), [](const Paper* a, const Paper* b) { return a->seconds > b->seconds; });
    papers_->clear();
    auto& lib = AppContext::get().library();
    for (auto* p : order) {
        auto* row = new QTreeWidgetItem(papers_);
        row->setText(0, paperTitle(p->latest));
        row->setToolTip(0, row->text(0));
        row->setText(1, readingDuration(p->seconds));
        row->setText(2, QString::number(p->sessions));
        row->setText(3, QLocale().toString(QDateTime::fromSecsSinceEpoch(p->last).date(), QLocale::ShortFormat));
        if (auto info = lib.libraryByUid(p->latest.library)) {
            row->setData(0, LibraryRole, (qint64)info->id);
            row->setData(0, KeyRole, qs(p->latest.attachment));
        }
    }
    showDay();
}

void ReadingWindow::selectDay(QDate day)
{
    selected_ = day;
    showDay();
}

void ReadingWindow::showDay()
{
    days_->selected = selected_;
    days_->update();
    int64_t from = startOf(selected_), to = startOf(selected_.addDays(1));
    std::vector<ReadingSession> day;
    for (auto& s : sessions_)
        if (s.end > from && s.start < to)
            day.push_back(s);
    auto seconds = readingSeconds(day, from, to);
    auto date = QLocale().toString(selected_, QLocale::LongFormat);
    dayTitle_->setText(day.empty() ? tr("%1: nothing read").arg(date)
                                   : tr("%1: %2, %3")
                                         .arg(date, readingDuration(seconds),
                                              day.size() == 1 ? tr("1 session") : tr("%1 sessions").arg(day.size())));
    line_->day = selected_;
    line_->sessions = day;
    line_->update();
    list_->clear();
    std::set<std::string> devices;
    auto& lib = AppContext::get().library();
    auto now = timer_->session();
    for (auto& s : day) {
        auto* row = new QTreeWidgetItem(list_);
        row->setText(0, now && now->continues(s) ? clock(s.start) + " – " + tr("now") : span(s));
        row->setText(1, readingDuration(s.seconds()));
        row->setText(2, paperTitle(s));
        row->setToolTip(2, row->text(2));
        row->setText(3, qs(s.device));
        devices.insert(s.device);
        if (auto info = lib.libraryByUid(s.library)) {
            row->setData(0, LibraryRole, (qint64)info->id);
            row->setData(0, KeyRole, qs(s.attachment));
        }
    }
    list_->setColumnHidden(3, devices.size() < 2);
}

} // namespace on::gui
