#include "gui/reading_timer.h"

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QSysInfo>

#include <algorithm>

#include "core/library.h"
#include "gui/app_context.h"
#include "gui/common.h"

namespace on::gui {

namespace {

// This computer in the reading log: an id of its own, and its name. A configuration
// copied to another computer gets a new id there, so that two never write one record.
std::pair<std::string, std::string> device()
{
    auto& cfg = AppContext::get().config();
    auto host = ss(QSysInfo::machineHostName());
    auto d = cfg.get("/reading/device");
    if (!d.is_object() || !isValidKey(jstr(d, "id")) || jstr(d, "host") != host) {
        d = Json{{"id", newKey()}, {"host", host}};
        cfg.set("/reading/device", d);
        try {
            cfg.save();
        } catch (const std::exception&) {
        }
    }
    return {jstr(d, "id"), host.empty() ? std::string("?") : host};
}

int64_t startOf(QDate day) { return day.startOfDay().toSecsSinceEpoch(); }

} // namespace

std::string readingDay(QDate date) { return ss(date.toString("yyyyMMdd")); }

QString readingDuration(int64_t seconds)
{
    if (seconds > 0 && seconds < 60)
        return QObject::tr("<1 min");
    int64_t minutes = std::max<int64_t>(seconds, 0) / 60;
    if (minutes < 60)
        return QObject::tr("%1 min").arg(minutes);
    return QObject::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

ReadingTimer::ReadingTimer(QObject* parent) : QObject(parent)
{
    clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
    tick_.setInterval(10000);
    connect(&tick_, &QTimer::timeout, this, [this] {
        check();
        if (current_)
            emit changed();  // (the minutes shown)
    });
    qApp->installEventFilter(this);
}

ReadingTimer::~ReadingTimer() { stop(); }

bool ReadingTimer::enabled()
{
    auto* ctx = AppContext::tryGet();
    return !ctx || ctx->config().get("/reading/enabled", true).get<bool>();
}

int ReadingTimer::idleSeconds()
{
    auto* ctx = AppContext::tryGet();
    int minutes = ctx ? ctx->config().get("/reading/idleMinutes", 2).get<int>() : 2;
    return std::clamp(minutes, 1, 60) * 60;
}

void ReadingTimer::activity() { evaluate(clock(), true); }

void ReadingTimer::check() { evaluate(clock(), false); }

void ReadingTimer::stop()
{
    if (!current_)
        return;
    auto now = clock();
    auto gap = now - current_->last;
    end(gap < 0 || gap >= idleSeconds() * 1000LL ? current_->last : now);
}

void ReadingTimer::evaluate(int64_t now, bool input)
{
    if (!enabled()) {
        if (current_)
            end(current_->last);
        return;
    }
    auto t = target ? target() : std::nullopt;
    if (current_) {
        auto gap = now - current_->last;
        if (gap < 0 || gap >= idleSeconds() * 1000LL)
            end(current_->last);  // no input for a while (or the clock went back): ended at the last
        else if (!t || !(*t == current_->target))
            end(now);  // another PDF, the library, another window or program
    }
    if (t && input) {
        if (!current_) {
            Current c;
            c.target = *t;
            c.last = now;
            c.session.start = c.session.end = now / 1000;
            c.session.attachment = ss(t->attachmentKey);
            c.session.item = c.session.attachment;
            c.session.title = ss(t->title.left(200));
            if (auto* ctx = AppContext::tryGet()) {
                auto& lib = ctx->library();
                if (auto info = lib.library(t->libraryId))
                    c.session.library = info->uid;
                c.session.item = lib.topLevelKey(t->libraryId, c.session.attachment);
            }
            current_ = std::move(c);
            tick_.start();
            emit changed();
        } else {
            current_->last = now;
            if (!t->title.isEmpty())
                current_->session.title = ss(t->title.left(200));
        }
    }
    if (current_ && current_->last - std::max(current_->savedAt, current_->session.start * 1000) >= kSaveEveryMs) {
        save(*session());  // (lost in a crash otherwise)
        current_->savedAt = current_->last;
    }
}

void ReadingTimer::end(int64_t at)
{
    auto c = std::move(*current_);
    current_.reset();
    tick_.stop();
    c.session.end = std::max(c.session.start, at / 1000);
    if (c.session.seconds() >= kShortestSeconds || c.savedAt)
        save(c.session);
    emit changed();
}

std::optional<ReadingSession> ReadingTimer::session() const
{
    if (!current_)
        return std::nullopt;
    auto s = current_->session;
    s.end = std::max(s.start, current_->last / 1000);
    return s;
}

void ReadingTimer::save(const ReadingSession& s)
{
    auto* ctx = AppContext::tryGet();
    if (!ctx)
        return;
    try {
        auto& lib = ctx->library();
        auto id = readingLogLibrary(lib);
        if (!id)
            return;
        auto [deviceId, name] = device();
        recordReadingSession(lib, *id, readingDay(QDateTime::fromSecsSinceEpoch(s.start).date()), deviceId, name, s);
    } catch (const std::exception&) {
        // (a session lost rather than the app)
    }
}

std::vector<ReadingSession> ReadingTimer::sessions(QDate first, QDate last) const
{
    std::vector<ReadingSession> out;
    auto* ctx = AppContext::tryGet();
    if (ctx)
        try {
            auto& lib = ctx->library();
            // and the day before's, which may go on past midnight
            if (auto id = readingLogLibrary(lib))
                out = readingSessions(lib, *id, readingDay(first.addDays(-1)), readingDay(last));
        } catch (const std::exception&) {
        }
    if (auto s = session()) {
        if (ctx)
            s->device = device().second;
        mergeReadingSession(out, *s);
    }
    int64_t from = startOf(first), to = startOf(last.addDays(1));
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const ReadingSession& s) { return s.end <= from || s.start >= to; }),
              out.end());
    std::stable_sort(out.begin(), out.end(),
                     [](const ReadingSession& a, const ReadingSession& b) { return a.start < b.start; });
    return out;
}

int64_t ReadingTimer::secondsOn(QDate day) const
{
    return readingSeconds(sessions(day, day), startOf(day), startOf(day.addDays(1)));
}

bool ReadingTimer::eventFilter(QObject* watched, QEvent* e)
{
    switch (e->type()) {
    case QEvent::InputMethod:  // typing through an input method (not spontaneous)
        break;
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::NativeGesture:
        if (!e->spontaneous())
            return false;  // (sent by the app itself, not the user's)
        break;
    default:
        return false;
    }
    auto now = clock();
    if (current_ && now >= current_->last && now - current_->last < 1000) {
        current_->last = now;  // (a mouse moving sends a great many: each one is not looked at)
        return false;
    }
    activity();
    return QObject::eventFilter(watched, e);
}

} // namespace on::gui
