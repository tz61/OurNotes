#pragma once

#include <QDate>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>
#include <vector>

#include "core/reading_log.h"

namespace on::gui {

// Counts the time PDFs are read: while the app is in front with a PDF's tab shown, from
// the first input (the mouse, the keyboard, a touch) to the last before none comes for a
// while (Preferences, 2 minutes at first): the time after the last input is not counted.
// Leaving the PDF (another tab, another window or program) ends the session there and
// then. Sessions are written to the reading log (core/reading_log.h) when they end, and
// every few minutes while they go on.
class ReadingTimer : public QObject {
    Q_OBJECT
public:
    struct Target {
        int64_t libraryId = 0;
        QString attachmentKey;
        QString title;
        bool operator==(const Target& o) const { return libraryId == o.libraryId && attachmentKey == o.attachmentKey; }
    };

    explicit ReadingTimer(QObject* parent = nullptr);
    ~ReadingTimer() override;

    // What is being read now: none unless the app is in front with a PDF shown.
    std::function<std::optional<Target>()> target;
    // Milliseconds since the epoch (the wall clock: the computer's sleep is time going by).
    std::function<int64_t()> clock;

    void activity();  // an input; or the PDF shown, the app brought to the front, left
    void check();     // what may have ended meanwhile (no input for a while)
    void stop();      // the app quits: the session ends
    bool reading() const { return current_.has_value(); }
    // The session going on, up to its last input; none when not reading.
    std::optional<ReadingSession> session() const;

    // Every computer's sessions of the days from `first` to `last` (both counted), the
    // session going on with them.
    std::vector<ReadingSession> sessions(QDate first, QDate last) const;
    int64_t secondsOn(QDate day) const;  // read that day here (local time), on every computer

    static bool enabled();
    static int idleSeconds();
    static constexpr int kShortestSeconds = 5;  // a session shorter is not kept (a tab passed through)
    static constexpr int kSaveEveryMs = 5 * 60 * 1000;

signals:
    void changed();  // a session started, ended, was written, or went on a while

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    struct Current {
        Target target;
        ReadingSession session;
        int64_t last = 0;     // ms of the last input
        int64_t savedAt = 0;  // ms it was last written, 0 before
    };
    void evaluate(int64_t now, bool input);
    void end(int64_t at);
    void save(const ReadingSession& s);

    std::optional<Current> current_;
    QTimer tick_;
};

// The local date of a moment as the reading log names days: YYYYMMDD.
std::string readingDay(QDate date);
// "1 h 23 min", "23 min"
QString readingDuration(int64_t seconds);

} // namespace on::gui
