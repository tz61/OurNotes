#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"

namespace on::sync {
class ChangeWatcher;
class SyncHistory;
class Transport;
}

namespace on::gui {

// A connection to a sync target: a server, or a folder.
std::unique_ptr<sync::Transport> makeTransport(const SyncTarget& target);

// Runs synchronisation with the active sync target on a background thread
// (with its own database connection) and schedules automatic syncs: after changes
// here, every few minutes, and (with automatic syncs on) as soon as the target tells
// of another computer's changes.
class SyncController : public QObject {
    Q_OBJECT
public:
    explicit SyncController(QObject* parent = nullptr);
    ~SyncController() override;

    bool isRunning() const { return running_; }
    bool hasTarget() const;
    bool isWatching() const { return watcher_ != nullptr; }  // for other computers' changes
    void reloadSettings();
    const sync::SyncHistory& history() const { return *history_; }
    // Download attachment files from the active target on a thread of its own (to copy
    // them: see SyncEngine::downloadAttachments), then done(those that could not be, with
    // why) on the GUI thread, if `context` is still there.
    static void downloadFiles(qint64 libraryId, const std::vector<std::string>& keys, QObject* context,
                              std::function<void(const std::map<std::string, std::string>& failed)> done);

public slots:
    void syncNow();
    void cancel();
    // Download one attachment file from the sync target (on-demand file mode).
    void downloadFile(qint64 libraryId, const QString& key);
    // Delete an empty library on the sync target (not here): remoteLibraryDeleted tells how
    // it went.
    void deleteRemoteLibrary(qint64 libraryId);

signals:
    void started();
    void progress(const QString& message);
    void finished(bool ok, const QString& summary);
    void fileDownloaded(qint64 libraryId, const QString& key, bool ok, const QString& error);
    void remoteLibraryDeleted(qint64 libraryId, bool ok, const QString& error);
    void historyChanged();

private:
    void join();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    QTimer autoTimer_;
    QTimer intervalTimer_;
    QTimer historyTimer_;
    bool pendingAfterRun_ = false;
    std::unique_ptr<sync::SyncHistory> history_;
    std::atomic<uint64_t> syncCount_{0};  // syncs started and finished: odd while one runs
    std::unique_ptr<sync::ChangeWatcher> watcher_;
};

} // namespace on::gui
