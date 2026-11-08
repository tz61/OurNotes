#pragma once

#include <QDate>
#include <QDialog>
#include <QTimer>
#include <QToolButton>

#include <vector>

#include "core/reading_log.h"

class QLabel;
class QTreeWidget;

namespace on::gui {

class DayLine;
class DaysChart;
class ReadingTimer;

// Today's reading time at the right of the status bar, dimmed while not counting; a click
// shows the Reading Time window.
class ReadingStatusButton : public QToolButton {
    Q_OBJECT
public:
    ReadingStatusButton(ReadingTimer* timer, QWidget* parent = nullptr);
    void refresh();

private:
    ReadingTimer* timer_;
    QTimer later_;   // after a sync brought another computer's
    QTimer minute_;  // a new day, while not reading
};

// The reading time of the last days: a bar for each day, and for the day picked its
// sessions on a line through the day and as a list; and the papers read most.
class ReadingWindow : public QDialog {
    Q_OBJECT
public:
    ReadingWindow(ReadingTimer* timer, QWidget* parent = nullptr);
    void reload();
    void selectDay(QDate day);
    QDate selectedDay() const { return selected_; }
    static constexpr int kDays = 14;       // bars
    static constexpr int kPaperDays = 30;  // the papers read most, of these days

signals:
    void openRequested(qint64 libraryId, const QString& attachmentKey);

private:
    void showDay();

    ReadingTimer* timer_;
    QDate selected_;
    std::vector<ReadingSession> sessions_;  // the last kPaperDays days'
    QLabel* totals_;
    DaysChart* days_;
    QLabel* dayTitle_;
    DayLine* line_;
    QTreeWidget* list_;
    QTreeWidget* papers_;
    QTimer later_;
};

} // namespace on::gui
