#pragma once

#include <QDialog>

#include "core/json.h"
#include "core/sync/engine.h"
#include "core/util.h"

class QTreeWidget;

namespace on::gui {

// The last syncs, kept in the data directory (sync-history.json) only: what each
// brought in and sent out. An object is listed once, under the last sync that
// changed it, so a note typed over several syncs is one entry.
class SyncHistory {
public:
    static constexpr int kMaxRuns = 30;
    static constexpr int kMaxChanges = 200;  // per sync; the rest are counted

    explicit SyncHistory(fs::path file);

    // Newest first: {time, target, ok, summary, errors[], changes[], more}
    const Json& runs() const { return runs_; }
    void add(const std::string& target, const sync::SyncReport& report, bool ok, const std::string& summary);
    void addFailure(const std::string& target, const std::string& error);

private:
    void add(Json run);
    void save() const;

    fs::path file_;
    Json runs_ = Json::array();
};

// The window that shows it.
class SyncHistoryWindow : public QDialog {
    Q_OBJECT
public:
    explicit SyncHistoryWindow(QWidget* parent = nullptr);
    void reload(const SyncHistory& history);

signals:
    void syncRequested();
    void itemRequested(qint64 libraryId, const QString& key);
    void annotationRequested(qint64 libraryId, const QString& attachmentKey, const QString& annotationKey);

private:
    QTreeWidget* tree_;
};

} // namespace on::gui
